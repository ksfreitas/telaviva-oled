#include "CalibrationSession.h"

#include "JsonCodec.h"

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace telaviva {
namespace {

constexpr int kSchema = 1;

namespace json = winrt::Windows::Data::Json;

// Same apartment setup as InferenceFailureStore.cpp; returns false when WinRT cannot start here.
bool ensureApartment()
{
    thread_local bool initialized = false;
    if (initialized) {
        return true;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        // The thread already has another apartment; parsing works there too.
        if (error.code() != static_cast<winrt::hresult>(RPC_E_CHANGED_MODE)) {
            return false;
        }
    }
    initialized = true;
    return true;
}

std::string utcTimestampNow()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    if (gmtime_s(&utc, &now) != 0) {
        return "1970-01-01T00:00:00Z";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

// Only whole numbers in the uint32 range are valid values.
std::optional<uint32_t> toValue(double number)
{
    if (!(number >= 0.0 && number <= 4294967295.0) || std::floor(number) != number) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(number);
}

// Levels are percents: finite and within 0..100.
bool isPercentValue(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

// A record needs the adaptive part or the monitor part, and every present part must be valid.
bool validRecord(const CalibrationRecord& record)
{
    if (record.adaptiveValid ? !isSchemeGuidText(record.adaptive.schemeGuid) : !record.monitor.has_value()) {
        return false;
    }
    if (record.monitor && (record.monitor->monitorId.empty() || !isPercentValue(record.monitor->percent))) {
        return false;
    }
    return !record.panelLevel || isPercentValue(*record.panelLevel);
}

// Returns false for any shape other than the schema 1 layouts, including wrong types. A monitor_id
// without monitor_percent, or the reverse, is corrupt.
bool parseFile(const std::string& text, CalibrationRecord& out)
{
    try {
        const json::JsonObject root = json::JsonObject::Parse(winrt::to_hstring(std::string_view(text)));
        if (root.GetNamedNumber(L"schema") != kSchema) {
            return false;
        }
        CalibrationRecord record;
        record.adaptiveValid = root.HasKey(L"adaptive_brightness");
        if (record.adaptiveValid) {
            const json::JsonObject adaptive = root.GetNamedObject(L"adaptive_brightness");
            SavedAdaptiveState state;
            state.schemeGuid = winrt::to_string(adaptive.GetNamedString(L"scheme"));
            const std::optional<uint32_t> acValue = toValue(adaptive.GetNamedNumber(L"ac"));
            const std::optional<uint32_t> dcValue = toValue(adaptive.GetNamedNumber(L"dc"));
            if (!acValue || !dcValue || !isSchemeGuidText(state.schemeGuid)) {
                return false;
            }
            state.acValue = *acValue;
            state.dcValue = *dcValue;
            record.adaptive = state;
        }
        const bool hasMonitorId = root.HasKey(L"monitor_id");
        if (hasMonitorId != root.HasKey(L"monitor_percent")) {
            return false;
        }
        if (hasMonitorId) {
            SavedMonitorState monitor;
            monitor.monitorId = winrt::to_string(root.GetNamedString(L"monitor_id"));
            monitor.percent = root.GetNamedNumber(L"monitor_percent");
            record.monitor = monitor;
        }
        if (root.HasKey(L"panel_level")) {
            record.panelLevel = root.GetNamedNumber(L"panel_level");
        }
        // started_at is required for a valid record, but recovery does not use it.
        static_cast<void>(root.GetNamedString(L"started_at"));
        if (!validRecord(record)) {
            return false;
        }
        out = record;
        return true;
    } catch (...) {
        return false;
    }
}

// Shortest form that reads back exactly for percents: whole numbers print without a fraction.
std::string numberText(double value)
{
    std::ostringstream out;
    out.precision(15);
    out << value;
    return out.str();
}

}  // namespace

CalibrationSession::CalibrationSession(std::filesystem::path directory)
    : file_(std::move(directory))
{
    file_ /= L"calibration-session.json";
}

bool CalibrationSession::begin(const SavedAdaptiveState& saved)
{
    CalibrationRecord record;
    record.adaptive = saved;
    return begin(record);
}

bool CalibrationSession::begin(const CalibrationRecord& record)
{
    if (!record.adaptiveValid || !validRecord(record)) {
        return false;
    }
    CalibrationRecord existing;
    const Load state = load(existing);
    if (state == Load::Loaded || state == Load::Unreadable) {
        return false;
    }
    return save(record);
}

bool CalibrationSession::rewrite(const CalibrationRecord& record)
{
    if (!validRecord(record)) {
        return false;
    }
    return save(record);
}

void CalibrationSession::end()
{
    static_cast<void>(removeFile());
}

std::optional<SavedAdaptiveState> CalibrationSession::pending() const
{
    const std::optional<CalibrationRecord> record = pendingRecord();
    if (record && record->adaptiveValid) {
        return record->adaptive;
    }
    return std::nullopt;
}

std::optional<CalibrationRecord> CalibrationSession::pendingRecord() const
{
    CalibrationRecord record;
    if (load(record) == Load::Loaded) {
        return record;
    }
    return std::nullopt;
}

bool CalibrationSession::recover(AdaptiveBrightness& adaptive)
{
    CalibrationRecord record;
    switch (load(record)) {
    case Load::Absent:
    case Load::Corrupt:
        // A corrupt file was just quarantined, so there is nothing to restore.
        return true;
    case Load::Unreadable:
        return false;
    case Load::Loaded:
        break;
    }
    if (!record.adaptiveValid) {
        // Monitor-only: no adaptive values are left, and the monitor part stays until the monitor is back.
        return true;
    }
    // Keep the record unless the restore succeeded, so the next launch retries it.
    if (!adaptive.restore(record.adaptive)) {
        return false;
    }
    if (!record.monitor) {
        return removeFile();
    }
    // The adaptive values are back, so the record keeps only the monitor part. A later launch must not
    // restore the old values over a later change. The panel level is restored by the caller and is dropped.
    CalibrationRecord monitorOnly;
    monitorOnly.adaptiveValid = false;
    monitorOnly.monitor = record.monitor;
    return save(monitorOnly);
}

std::filesystem::path CalibrationSession::file() const
{
    return file_;
}

CalibrationSession::Load CalibrationSession::load(CalibrationRecord& out) const
{
    std::error_code ec;
    const bool present = std::filesystem::exists(file_, ec);
    if (ec) {
        return Load::Unreadable;
    }
    if (!present) {
        return Load::Absent;
    }
    if (!ensureApartment()) {
        // Without WinRT the file cannot be judged, so it is left alone.
        return Load::Unreadable;
    }
    std::string text;
    try {
        std::ifstream in(file_, std::ios::binary);
        if (!in) {
            return Load::Unreadable;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (...) {
        return Load::Unreadable;
    }
    CalibrationRecord parsed;
    if (parseFile(text, parsed)) {
        out = parsed;
        return Load::Loaded;
    }
    quarantine();
    return Load::Corrupt;
}

bool CalibrationSession::save(const CalibrationRecord& record) const
{
    // Adaptive-only records keep the schema 1 text they always had. The monitor and panel parts are added when present.
    std::string text = "{\"schema\":1";
    if (record.adaptiveValid) {
        text += ",\"adaptive_brightness\":{\"scheme\":" + jsonQuote(record.adaptive.schemeGuid) +
                ",\"ac\":" + std::to_string(record.adaptive.acValue) +
                ",\"dc\":" + std::to_string(record.adaptive.dcValue) + "}";
    }
    if (record.monitor) {
        text += ",\"monitor_id\":" + jsonQuote(record.monitor->monitorId) +
                ",\"monitor_percent\":" + numberText(record.monitor->percent);
    }
    if (record.panelLevel) {
        text += ",\"panel_level\":" + numberText(*record.panelLevel);
    }
    text += ",\"started_at\":" + jsonQuote(utcTimestampNow()) + "}";
    try {
        std::error_code ec;
        static_cast<void>(std::filesystem::create_directories(file_.parent_path(), ec));

        std::filesystem::path temp = file_;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out << text;
            out.flush();
            if (!out) {
                out.close();
                static_cast<void>(std::filesystem::remove(temp, ec));
                return false;
            }
        }
        // rename replaces an existing target on Windows, so readers see either version.
        std::filesystem::rename(temp, file_, ec);
        if (ec) {
            static_cast<void>(std::filesystem::remove(temp, ec));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void CalibrationSession::quarantine() const
{
    std::filesystem::path corrupt = file_;
    corrupt += L".corrupt";
    std::error_code ec;
    std::filesystem::rename(file_, corrupt, ec);
}

bool CalibrationSession::removeFile() const
{
    std::error_code ec;
    static_cast<void>(std::filesystem::remove(file_, ec));
    return !ec;
}

}  // namespace telaviva
