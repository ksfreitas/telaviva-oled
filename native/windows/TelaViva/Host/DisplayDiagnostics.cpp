#include "pch.h"

#include "DisplayDiagnostics.h"

#include "Platform/DdcTimingProfile.h"
#include "Platform/DdcTransaction.h"
#include "Platform/DisplayCatalog.h"
#include "Platform/Dxva2DdcPort.h"
#include "Platform/LaptopDetection.h"
#include "Platform/LaptopPanel.h"
#include "Platform/SystemSleeper.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace telaviva::host {

namespace {

std::string toUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// Input is UTF-8; non-ASCII bytes pass through unchanged, which JSON allows.
std::string jsonString(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
                out += escaped;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    out += '"';
    return out;
}

std::string jsonStringW(std::wstring_view text)
{
    return jsonString(toUtf8(text));
}

constexpr const char* jsonBool(bool value)
{
    return value ? "true" : "false";
}

std::string jsonOptionalBool(std::optional<bool> value)
{
    return value ? std::string(jsonBool(*value)) : std::string("null");
}

// Writes without a BOM, matching the other diagnostic outputs.
bool writeTextFile(const std::wstring& path, const std::string& text)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const DWORD expected = static_cast<DWORD>(text.size());
    const BOOL ok = WriteFile(file, text.data(), expected, &written, nullptr);
    CloseHandle(file);
    return ok && written == expected;
}

// One VCP read. The raw EDID is never emitted; only its size is.
std::string probeCode(DdcTransaction& transaction, uint8_t code)
{
    // steady_clock so a wall-clock change during the read cannot skew the timing.
    const auto start = std::chrono::steady_clock::now();
    const DdcResult result = transaction.readVcp(code);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    std::string json = "{\"ok\":";
    json += jsonBool(result.ok);
    json += ",\"current\":";
    json += result.value ? std::to_string(*result.value) : std::string("null");
    json += ",\"error\":";
    json += jsonString(result.error);
    json += ",\"elapsedMs\":";
    json += std::to_string(elapsed.count());
    json += '}';
    return json;
}

}  // namespace

int runListDisplays(const std::wstring& outFile)
{
    telaviva::Win32DisplayCatalog catalog;
    const std::vector<telaviva::CatalogEntry> entries = catalog.enumerate();

    std::string json = "{\"monitors\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const telaviva::CatalogEntry& entry = entries[i];
        if (i > 0) {
            json += ',';
        }
        json += "{\"id\":";
        json += jsonString(entry.identity.id);
        json += ",\"stable\":";
        json += jsonBool(entry.identity.stable);
        json += ",\"name\":";
        json += jsonString(entry.identity.name);
        json += ",\"manufacturer\":";
        json += jsonString(entry.manufacturer);
        json += ",\"connectionPath\":";
        json += jsonString(entry.connectionPath);
        json += ",\"gdiDeviceName\":";
        json += jsonStringW(entry.gdiDeviceName);
        json += ",\"isInternalPanel\":";
        json += jsonBool(entry.isInternalPanel);
        json += ",\"active\":";
        json += jsonBool(entry.active);
        json += ",\"hasHmonitor\":";
        json += jsonBool(entry.hmonitor != 0);
        json += ",\"edidBytes\":";
        json += std::to_string(entry.edid.size());
        json += '}';
    }
    json += "]}\n";

    return writeTextFile(outFile, json) ? 0 : 4;
}

int runProbeDdc(const std::wstring& outFile)
{
    telaviva::Win32DisplayCatalog catalog;
    const std::vector<telaviva::CatalogEntry> entries = catalog.enumerate();
    // Only reads are issued below; writeVcp and setAndVerify are never called.
    telaviva::SystemSleeper sleeper;

    std::string json = "{\"monitors\":[";
    bool first = true;
    for (const telaviva::CatalogEntry& entry : entries) {
        // Internal panels and detached sources have no DDC/CI channel to probe.
        if (entry.isInternalPanel || !entry.active || entry.hmonitor == 0) {
            continue;
        }
        if (!first) {
            json += ',';
        }
        first = false;

        std::vector<telaviva::PhysicalMonitor> physical;
        std::string openError;
        try {
            physical = telaviva::PhysicalMonitor::openForHMonitor(entry.hmonitor);
        } catch (const std::exception& error) {
            // Record the failure so one bad monitor does not hide the others.
            openError = error.what();
        }

        json += "{\"id\":";
        json += jsonString(entry.identity.id);
        json += ",\"manufacturer\":";
        json += jsonString(entry.manufacturer);
        json += ",\"name\":";
        json += jsonString(entry.identity.name);
        json += ",\"gdiDeviceName\":";
        json += jsonStringW(entry.gdiDeviceName);
        json += ",\"physicalCount\":";
        json += std::to_string(physical.size());
        json += ",\"openError\":";
        json += openError.empty() ? std::string("null") : jsonString(openError);
        json += ",\"physicalMonitors\":[";

        // Pacing comes from the manufacturer so the probe matches real writes.
        const telaviva::DdcTimingProfile profile = telaviva::DdcTimingProfile::forManufacturer(entry.manufacturer);
        for (std::size_t p = 0; p < physical.size(); ++p) {
            if (p > 0) {
                json += ',';
            }
            telaviva::Dxva2DdcPort port(physical[p]);
            telaviva::DdcTransaction transaction(port, sleeper, profile);

            json += "{\"description\":";
            json += jsonStringW(physical[p].description());
            json += ",\"vcp\":{\"0x10\":";
            json += probeCode(transaction, 0x10);
            json += ",\"0x62\":";
            json += probeCode(transaction, 0x62);
            json += "}}";
        }
        json += "]}";
    }
    json += "]}\n";

    return writeTextFile(outFile, json) ? 0 : 4;
}

int runProbeLaptop(const std::wstring& outFile)
{
    // Set only if detectLaptop reaches the probe, so a desktop leaves it null.
    std::optional<double> wmiPercent;

    telaviva::Win32LaptopEnvironment environment;
    // WmiPanel is built inside the probe, not up front. Only readPercent is called;
    // setPercent is never called.
    environment.panelBrightnessProbe = [&wmiPercent]() -> telaviva::BrightnessReadability {
        telaviva::WmiPanel panel;
        wmiPercent = panel.readPercent();
        return wmiPercent ? telaviva::BrightnessReadability::Ok : telaviva::BrightnessReadability::NotReadable;
    };

    const telaviva::LaptopAvailability availability = telaviva::detectLaptop(environment);

    // Plain reads of the same environment; none of them touch the panel.
    const bool internalPath = environment.internalDisplayPath().has_value();
    const std::optional<bool> battery = environment.hasBattery();
    const std::optional<bool> lid = environment.lidOpen();
    const bool displayOn = environment.displayOn();

    std::string percentJson = "null";
    if (wmiPercent && std::isfinite(*wmiPercent)) {
        percentJson = std::to_string(*wmiPercent);
    }

    std::string json = "{\"available\":";
    json += jsonBool(availability.available);
    json += ",\"reason\":";
    json += jsonString(telaviva::laptopReasonString(availability.reason));
    json += ",\"internal_display_path\":";
    json += jsonBool(internalPath);
    json += ",\"has_battery\":";
    json += jsonOptionalBool(battery);
    json += ",\"lid_open\":";
    json += jsonOptionalBool(lid);
    json += ",\"display_on\":";
    json += jsonBool(displayOn);
    json += ",\"wmi_read_percent\":";
    json += percentJson;
    json += ",\"wrote_to_panel\":false}\n";

    return writeTextFile(outFile, json) ? 0 : 4;
}

}  // namespace telaviva::host
