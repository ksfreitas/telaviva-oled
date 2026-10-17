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

// Returns false for any shape other than the schema 1 layout, including wrong types.
bool parseFile(const std::string& text, SavedAdaptiveState& out)
{
    try {
        const json::JsonObject root = json::JsonObject::Parse(winrt::to_hstring(std::string_view(text)));
        if (root.GetNamedNumber(L"schema") != kSchema) {
            return false;
        }
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
        // started_at is required for a valid record, but recovery does not use it.
        static_cast<void>(root.GetNamedString(L"started_at"));
        out = state;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

CalibrationSession::CalibrationSession(std::filesystem::path directory)
    : file_(std::move(directory))
{
    file_ /= L"calibration-session.json";
}

bool CalibrationSession::begin(const SavedAdaptiveState& saved)
{
    if (!isSchemeGuidText(saved.schemeGuid)) {
        return false;
    }
    SavedAdaptiveState existing;
    const Load state = load(existing);
    if (state == Load::Loaded || state == Load::Unreadable) {
        return false;
    }
    return save(saved);
}

void CalibrationSession::end()
{
    static_cast<void>(removeFile());
}

std::optional<SavedAdaptiveState> CalibrationSession::pending() const
{
    SavedAdaptiveState state;
    if (load(state) == Load::Loaded) {
        return state;
    }
    return std::nullopt;
}

bool CalibrationSession::recover(AdaptiveBrightness& adaptive)
{
    SavedAdaptiveState state;
    switch (load(state)) {
    case Load::Absent:
    case Load::Corrupt:
        // A corrupt file was just quarantined, so there is nothing to restore.
        return true;
    case Load::Unreadable:
        return false;
    case Load::Loaded:
        break;
    }
    // Keep the record unless the restore succeeded, so the next launch retries it.
    if (!adaptive.restore(state)) {
        return false;
    }
    return removeFile();
}

std::filesystem::path CalibrationSession::file() const
{
    return file_;
}

CalibrationSession::Load CalibrationSession::load(SavedAdaptiveState& out) const
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
    SavedAdaptiveState parsed;
    if (parseFile(text, parsed)) {
        out = parsed;
        return Load::Loaded;
    }
    quarantine();
    return Load::Corrupt;
}

bool CalibrationSession::save(const SavedAdaptiveState& state) const
{
    const std::string text = "{\"schema\":1,\"adaptive_brightness\":{\"scheme\":" + jsonQuote(state.schemeGuid) +
                             ",\"ac\":" + std::to_string(state.acValue) + ",\"dc\":" + std::to_string(state.dcValue) +
                             "},\"started_at\":" + jsonQuote(utcTimestampNow()) + "}";
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
