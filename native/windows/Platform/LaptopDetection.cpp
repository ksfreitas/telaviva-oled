#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "LaptopDetection.h"

#include <cstddef>
#include <string>
#include <vector>

namespace telaviva {

namespace {

// GetSystemPowerStatus reports BatteryFlag 128 for "no system battery" and 255 for "unknown".
constexpr BYTE kBatteryFlagNoSystemBattery = 128;
constexpr BYTE kBatteryFlagUnknown = 255;

bool isEmbeddedPanel(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
{
    return technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
}

std::string toUtf8(const wchar_t* text)
{
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

}  // namespace

LaptopAvailability detectLaptop(ILaptopEnvironment& environment)
{
    // A device without a battery counts as a desktop even when an embedded
    // target is listed. An unknown battery state does not.
    const std::optional<DisplayPathInfo> panel = environment.internalDisplayPath();
    const std::optional<bool> battery = environment.hasBattery();
    if (!panel || (battery && !*battery)) {
        return {false, LaptopReason::NoBuiltInDisplay};
    }

    // An unreported lid state is treated as open. Only an explicit closed lid pauses.
    const std::optional<bool> lid = environment.lidOpen();
    const bool lidClosed = lid && !*lid;
    if (!panel->active || lidClosed || !environment.displayOn()) {
        return {false, LaptopReason::DisplayOffline};
    }

    // The probe needs the WMI namespace before a read can be judged, so a
    // missing API outranks a failed read.
    const BrightnessReadability readability = environment.panelBrightnessReadable();
    if (readability == BrightnessReadability::ApiMissing) {
        return {false, LaptopReason::ApiUnavailable};
    }
    if (readability == BrightnessReadability::NotReadable) {
        return {false, LaptopReason::BrightnessNotAdjustable};
    }
    return {true, LaptopReason::None};
}

const char* laptopReasonString(LaptopReason reason)
{
    switch (reason) {
    case LaptopReason::None:
        return "none";
    case LaptopReason::NoBuiltInDisplay:
        return "noBuiltInDisplay";
    case LaptopReason::DisplayOffline:
        return "displayOffline";
    case LaptopReason::BrightnessNotAdjustable:
        return "brightnessNotAdjustable";
    case LaptopReason::ApiUnavailable:
        return "apiUnavailable";
    case LaptopReason::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::optional<DisplayPathInfo> Win32LaptopEnvironment::internalDisplayPath()
{
    // The buffer can grow between the size query and the query, so retry a few times.
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        paths.assign(pathCount, DISPLAYCONFIG_PATH_INFO{});
        modes.assign(modeCount, DISPLAYCONFIG_MODE_INFO{});
        status = QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_SUCCESS) {
            paths.resize(pathCount);
        }
    }
    if (status != ERROR_SUCCESS) {
        // A failed query looks the same as no panel. The seam has no error channel.
        return std::nullopt;
    }

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = static_cast<UINT32>(sizeof(target));
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }
        if (!isEmbeddedPanel(target.outputTechnology)) {
            continue;
        }
        DisplayPathInfo info;
        info.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        info.name = toUtf8(target.monitorFriendlyDeviceName);
        return info;
    }
    return std::nullopt;
}

std::optional<bool> Win32LaptopEnvironment::lidOpen()
{
    const int state = lidState_.load();
    if (state < 0) {
        return std::nullopt;
    }
    return state != 0;
}

bool Win32LaptopEnvironment::displayOn()
{
    return displayOn_.load();
}

std::optional<bool> Win32LaptopEnvironment::hasBattery()
{
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        return std::nullopt;
    }
    if (status.BatteryFlag == kBatteryFlagNoSystemBattery) {
        return false;
    }
    if (status.BatteryFlag == kBatteryFlagUnknown) {
        return std::nullopt;
    }
    return true;
}

BrightnessReadability Win32LaptopEnvironment::panelBrightnessReadable()
{
    if (!panelBrightnessProbe) {
        return BrightnessReadability::ApiMissing;
    }
    return panelBrightnessProbe();
}

void Win32LaptopEnvironment::recordLidState(bool open)
{
    lidState_.store(open ? 1 : 0);
}

void Win32LaptopEnvironment::recordDisplayStatus(bool on)
{
    displayOn_.store(on);
}

}  // namespace telaviva
