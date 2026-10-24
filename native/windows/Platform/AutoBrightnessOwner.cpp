#include "AutoBrightnessOwner.h"

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

// Same apartment setup as CalibrationSession.cpp; returns false when WinRT cannot start here.
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
bool parseRecord(const std::string& text, SavedAdaptiveState& out)
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

// Absent: no file. Corrupt: the file exists but is not a schema 1 record. Unavailable: it cannot be judged now.
enum class RecordLoad { Absent, Loaded, Corrupt, Unavailable };

RecordLoad loadRecord(const std::filesystem::path& file, SavedAdaptiveState& out)
{
    std::error_code ec;
    const bool present = std::filesystem::exists(file, ec);
    if (ec) {
        return RecordLoad::Unavailable;
    }
    if (!present) {
        return RecordLoad::Absent;
    }
    if (!ensureApartment()) {
        // Without WinRT the file cannot be judged, so it is left alone.
        return RecordLoad::Unavailable;
    }
    std::string text;
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return RecordLoad::Unavailable;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (...) {
        return RecordLoad::Unavailable;
    }
    SavedAdaptiveState parsed;
    if (parseRecord(text, parsed)) {
        out = parsed;
        return RecordLoad::Loaded;
    }
    return RecordLoad::Corrupt;
}

// Writes to a temp file and renames it over the target, so readers see either the old or the new record.
bool saveRecord(const std::filesystem::path& file, const SavedAdaptiveState& state)
{
    const std::string text = "{\"schema\":1,\"adaptive_brightness\":{\"scheme\":" + jsonQuote(state.schemeGuid) +
                             ",\"ac\":" + std::to_string(state.acValue) + ",\"dc\":" + std::to_string(state.dcValue) +
                             "},\"started_at\":" + jsonQuote(utcTimestampNow()) + "}";
    try {
        std::error_code ec;
        static_cast<void>(std::filesystem::create_directories(file.parent_path(), ec));

        std::filesystem::path temp = file;
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
        std::filesystem::rename(temp, file, ec);
        if (ec) {
            static_cast<void>(std::filesystem::remove(temp, ec));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

AutoBrightnessOwner::AutoBrightnessOwner(AdaptiveBrightness& adaptive, std::filesystem::path recordFile,
                                         Diagnostics* diagnostics)
    : adaptive_(adaptive), recordFile_(std::move(recordFile)), diagnostics_(diagnostics)
{
}

AutoBrightnessOutcome AutoBrightnessOwner::acquire(AutoBrightnessHolder holder,
                                                   const AdaptiveBrightness::PersistFirst& persistFirst)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!holders_.empty()) {
        holders_.insert(holder);
        return held_.value_or(AutoBrightnessOutcome::unknown);
    }
    // While a failed restore is owed, the setting reads off because of this owner, not the user. Joining
    // the owed restore keeps the record, so the next release restores it.
    const AutoBrightnessOutcome outcome =
        pendingRestore_ ? AutoBrightnessOutcome::turnedOff : beginHoldLocked(persistFirst);
    held_ = outcome;
    holders_.insert(holder);
    return outcome;
}

AutoBrightnessOutcome AutoBrightnessOwner::beginHoldLocked(const AdaptiveBrightness::PersistFirst& persistFirst)
{
    // suspend reads the state, runs this callback only when the setting is on, and rolls back its own writes
    // when a write fails. The record is written before the setting changes, so a crash leaves it for recovery.
    bool recordWritten = false;
    // Set only when the callback runs, which needs a successful read of an on state. A failure after that is a
    // refused change; a failure before it is an unreadable state.
    bool settingWasOn = false;
    const SuspendResult result = adaptive_.suspend(saved_, [&](const SavedAdaptiveState& state) {
        settingWasOn = true;
        if (!saveRecord(recordFile_, state)) {
            return false;
        }
        recordWritten = true;
        if (persistFirst && !persistFirst(state)) {
            removeRecordLocked();
            recordWritten = false;
            return false;
        }
        return true;
    });
    if (result == SuspendResult::NotApplicable || result == SuspendResult::WasOff) {
        return AutoBrightnessOutcome::notApplicable;
    }
    if (result == SuspendResult::Suspended) {
        return AutoBrightnessOutcome::turnedOff;
    }
    // Failed: suspend rolled back its writes, so the record must not claim that a restore is owed.
    if (recordWritten) {
        removeRecordLocked();
    }
    if (settingWasOn) {
        logLocked(LogLevel::Notice, "auto brightness: on but not changed; nothing written");
        return AutoBrightnessOutcome::notChanged;
    }
    logLocked(LogLevel::Notice, "auto brightness: state unknown; nothing written");
    return AutoBrightnessOutcome::unknown;
}

bool AutoBrightnessOwner::release(AutoBrightnessHolder holder)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A holder that never acquired, or already released, changes nothing.
    if (holders_.erase(holder) == 0 || !holders_.empty()) {
        return true;
    }
    const bool owed = held_ == AutoBrightnessOutcome::turnedOff;
    held_.reset();
    if (owed && !restoreLocked(saved_)) {
        // Keep the record: deleting it would leave the setting off with no retry.
        logLocked(LogLevel::Notice, "auto brightness: restore failed; record kept for retry");
        pendingRestore_ = true;
        return false;
    }
    // Reached when nothing is owed, the restore succeeded, or a fresh read says on.
    removeRecordLocked();
    pendingRestore_ = false;
    return true;
}

bool AutoBrightnessOwner::isHeld(AutoBrightnessHolder holder) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return holders_.count(holder) != 0;
}

bool AutoBrightnessOwner::anyHeld() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !holders_.empty();
}

void AutoBrightnessOwner::recoverIfNeeded()
{
    std::lock_guard<std::mutex> lock(mutex_);
    SavedAdaptiveState state;
    switch (loadRecord(recordFile_, state)) {
    case RecordLoad::Absent:
        return;
    case RecordLoad::Corrupt:
        removeRecordLocked();
        pendingRestore_ = false;
        logLocked(LogLevel::Notice, "auto brightness: unparsable hold record deleted");
        return;
    case RecordLoad::Unavailable:
        logLocked(LogLevel::Notice, "auto brightness: hold record unavailable; kept");
        return;
    case RecordLoad::Loaded:
        break;
    }
    saved_ = state;
    if (!restoreLocked(state)) {
        // The setting reads off because this restore failed, so a later acquire must join it.
        logLocked(LogLevel::Notice, "auto brightness: launch restore failed; record kept for retry");
        pendingRestore_ = true;
        return;
    }
    removeRecordLocked();
    pendingRestore_ = false;
    logLocked(LogLevel::Notice, "auto brightness: restored from hold record");
}

bool AutoBrightnessOwner::restoreLocked(const SavedAdaptiveState& state)
{
    // Only a fresh read of on skips the write: the user turned it on while the hold was active.
    // Off, nil and unreadable all restore, because this owner turned the setting off.
    if (freshReadOnLocked()) {
        return true;
    }
    return adaptive_.restore(state);
}

bool AutoBrightnessOwner::freshReadOnLocked()
{
    SavedAdaptiveState current;
    if (adaptive_.readActive(current) != AdaptiveReadResult::Read) {
        return false;
    }
    return current.acValue != 0 || current.dcValue != 0;
}

void AutoBrightnessOwner::removeRecordLocked()
{
    std::error_code ec;
    static_cast<void>(std::filesystem::remove(recordFile_, ec));
}

void AutoBrightnessOwner::logLocked(LogLevel level, const char* message)
{
    if (diagnostics_ != nullptr) {
        diagnostics_->log(level, message);
    }
}

}  // namespace telaviva
