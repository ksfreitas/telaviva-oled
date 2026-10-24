#include "BuiltInProtection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace telaviva {
namespace {

namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;

constexpr int kRecordVersion = 1;
// One episode is the first attempt plus two retries; then it parks until the next trigger.
constexpr int kMaxAttempts = 3;
constexpr std::chrono::milliseconds kRetryDelay{1000};
// A verified dim is trusted for this long while the same decision repeats without a readback.
constexpr double kVerifyIntervalSeconds = 5.0;
// Readbacks within this many points of the supported level count as applied.
constexpr double kTolerancePoints = 1.0;

struct Record {
    double baseline = 0.0;
    std::string startedAt;
};

// Absent: no file. Corrupt: the file exists but is not a version 1 record. Unavailable: it cannot be judged now.
enum class RecordLoad { Absent, Loaded, Corrupt, Unavailable };

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

// ISO 8601 UTC text for the record's started_at, from the injected epoch clock.
std::string utcTimestamp(uint64_t epochMillis)
{
    const std::time_t seconds = static_cast<std::time_t>(epochMillis / 1000);
    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0) {
        return "1970-01-01T00:00:00Z";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

// Round-trip text for a level, so a fractional baseline survives the record.
std::string numberText(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

// Short text for the diagnostic lines.
std::string levelText(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

RecordLoad loadRecord(const fs::path& file, Record& out)
{
    std::error_code ec;
    const bool present = fs::exists(file, ec);
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
    try {
        const json::JsonObject root = json::JsonObject::Parse(winrt::to_hstring(std::string_view(text)));
        if (root.GetNamedNumber(L"version") != static_cast<double>(kRecordVersion)) {
            return RecordLoad::Corrupt;
        }
        const double baseline = root.GetNamedNumber(L"baseline_percent");
        if (!std::isfinite(baseline) || baseline < 0.0 || baseline > 100.0) {
            return RecordLoad::Corrupt;
        }
        Record parsed;
        parsed.baseline = baseline;
        parsed.startedAt = winrt::to_string(root.GetNamedString(L"started_at"));
        out = parsed;
        return RecordLoad::Loaded;
    } catch (...) {
        return RecordLoad::Corrupt;
    }
}

// Writes to a temp file and renames it over the target, so readers see either the old or the new record.
bool saveRecord(const fs::path& file, double baseline, const std::string& startedAt)
{
    const std::string text = "{\"version\":1,\"started_at\":" + jsonQuote(startedAt) +
                             ",\"baseline_percent\":" + numberText(baseline) + "}";
    try {
        std::error_code ec;
        static_cast<void>(fs::create_directories(file.parent_path(), ec));

        fs::path temp = file;
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
                static_cast<void>(fs::remove(temp, ec));
                return false;
            }
        }
        // rename replaces an existing target on Windows, so readers see either version.
        fs::rename(temp, file, ec);
        if (ec) {
            static_cast<void>(fs::remove(temp, ec));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void removeRecordFile(const fs::path& file)
{
    std::error_code ec;
    static_cast<void>(fs::remove(file, ec));
}

// Two decisions are the same dim when they agree on everything the dim path acts on.
bool sameDecision(const BuiltinProtectionSnapshot& lhs, const BuiltinProtectionSnapshot& rhs)
{
    return lhs.reasons == rhs.reasons && lhs.targetPercent == rhs.targetPercent &&
           lhs.holdAutoBrightness == rhs.holdAutoBrightness && lhs.available == rhs.available;
}

}  // namespace

BuiltInProtection::BuiltInProtection(ILaptopPanel& panel, AutoBrightnessOwner& owner, IScheduler& scheduler,
                                     IClock& clock, std::filesystem::path recordFile, Diagnostics* diagnostics,
                                     std::function<void(bool)> onHolding, std::function<void()> onNoticeChanged)
    : panel_(panel),
      owner_(owner),
      scheduler_(scheduler),
      clock_(clock),
      recordFile_(std::move(recordFile)),
      diagnostics_(diagnostics),
      onHolding_(std::move(onHolding)),
      onNoticeChanged_(std::move(onNoticeChanged))
{
}

BuiltInProtection::~BuiltInProtection()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        takeRetryLocked();
    }
    flushCancels();
}

void BuiltInProtection::reconcile(const BuiltinProtectionSnapshot& snapshot)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reconcileLocked(snapshot);
        // The caller builds the flyout from notice() after this call, so the change is not reported to onNoticeChanged_.
        reportedNotice_ = noticeLocked();
    }
    flushCancels();
}

void BuiltInProtection::recoverIfNeeded()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || holding_) {
        return;
    }
    Record record;
    switch (loadRecord(recordFile_, record)) {
    case RecordLoad::Absent:
        return;
    case RecordLoad::Corrupt:
        removeRecordFile(recordFile_);
        log("builtin_recovery unreadable record removed");
        return;
    case RecordLoad::Unavailable:
        log("builtin_recovery record unavailable; kept");
        return;
    case RecordLoad::Loaded:
        break;
    }
    // The level is not read: the screen is still dimmed from the crashed run.
    baseline_ = record.baseline;
    holding_ = true;
    log("builtin_recovery baseline=" + levelText(record.baseline));
    reportHolding(true);
}

void BuiltInProtection::restoreNow()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // First, so no snapshot after this point can dim the screen again.
        stopped_ = true;
        takeRetryLocked();
        forgetVerificationLocked();
        if (holding_ && baseline_) {
            const double base = *baseline_;
            static_cast<void>(panel_.setPercent(base));
            const std::optional<double> readBack = panel_.readPercent();
            const bool restored = readBack && levelAcceptedLocked(*readBack, base);
            // Automatic brightness goes back either way; the record is what restores the level later.
            releaseAutoBrightnessLocked();
            if (restored) {
                finishRestoreLocked(base);
            } else {
                // The record is kept for the next launch, so the restore is pending.
                restoreParked_ = true;
                log("builtin_restore_failed quit baseline=" + levelText(base));
            }
        }
    }
    flushCancels();
}

bool BuiltInProtection::isHolding() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return holding_;
}

BuiltInNotice BuiltInProtection::notice() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return noticeLocked();
}

BuiltInNotice BuiltInProtection::noticeLocked() const
{
    if (restoreParked_) {
        return BuiltInNotice::restoreFailed;
    }
    return autoNotice_;
}

void BuiltInProtection::reconcileLocked(const BuiltinProtectionSnapshot& snapshot)
{
    if (stopped_) {
        return;
    }
    // A changed target, option or availability starts a new episode, so attempts from an older one cannot park
    // it early. A lid close or display wake changes availability and so gets its own attempts.
    if (last_ && (last_->targetPercent != snapshot.targetPercent ||
                  last_->holdAutoBrightness != snapshot.holdAutoBrightness || last_->available != snapshot.available)) {
        retries_ = 0;
        restoreParked_ = false;
        forgetVerificationLocked();
        takeRetryLocked();
    }
    last_ = snapshot;
    if (snapshot.protectable && snapshot.available && !snapshot.reasons.empty() && snapshot.targetPercent) {
        dimLocked(*snapshot.targetPercent, snapshot);
    } else if (holding_) {
        restoreLocked(snapshot.available);
    }
}

void BuiltInProtection::dimLocked(double target, const BuiltinProtectionSnapshot& decision)
{
    if (!holding_ && !startHoldingLocked()) {
        return;
    }
    if (!baseline_) {
        return;
    }
    // The owner goes first, so the system cannot move the level after the write.
    syncAutoBrightnessLocked(decision.holdAutoBrightness);
    // Never above the baseline: a target higher than the baseline must not raise the level.
    dimLevelLocked(std::min(*baseline_, target), *baseline_, decision);
}

// Reads the baseline and saves the record before anything changes. False when there is nothing safe to restore to.
bool BuiltInProtection::startHoldingLocked()
{
    const std::optional<double> level = panel_.readPercent();
    if (!level || !std::isfinite(*level)) {
        log("builtin_dim skipped: baseline unreadable");
        return false;
    }
    const uint64_t nowMillis = clock_.epochMillis();
    if (!saveRecord(recordFile_, *level, utcTimestamp(nowMillis))) {
        log("builtin_dim skipped: record unwritable");
        return false;
    }
    baseline_ = *level;
    holding_ = true;
    reportHolding(true);
    return true;
}

void BuiltInProtection::syncAutoBrightnessLocked(bool holdAutoBrightness)
{
    if (holdAutoBrightness) {
        if (!autoHeld_) {
            // The dim happens either way, as on macOS when the owner cannot change the setting. An unknown outcome
            // (unreadable) and a notChanged outcome (on, but not turned off) are shown as notes; turnedOff and
            // notApplicable (already off, or no adaptive scheme) are not, as on macOS when already off.
            const AutoBrightnessOutcome outcome = owner_.acquire(AutoBrightnessHolder::protection);
            autoHeld_ = true;
            switch (outcome) {
            case AutoBrightnessOutcome::unknown:
                autoNotice_ = BuiltInNotice::autoBrightnessUnknown;
                break;
            case AutoBrightnessOutcome::notChanged:
                autoNotice_ = BuiltInNotice::autoBrightnessNotChanged;
                break;
            case AutoBrightnessOutcome::turnedOff:
            case AutoBrightnessOutcome::notApplicable:
                autoNotice_ = BuiltInNotice::none;
                break;
            }
        }
    } else {
        releaseAutoBrightnessLocked();
        // The note is shown only while the option is requested.
        autoNotice_ = BuiltInNotice::none;
    }
}

void BuiltInProtection::releaseAutoBrightnessLocked()
{
    if (!autoHeld_) {
        return;
    }
    // A false result means the owner kept its record for a retry; this controller's hold is over either way.
    owner_.release(AutoBrightnessHolder::protection);
    autoHeld_ = false;
}

// Writes the target when the level differs. The readback decides, because the setter's status is not trusted.
void BuiltInProtection::dimLevelLocked(double target, double baseline, const BuiltinProtectionSnapshot& decision)
{
    if (verified_ && sameDecision(*verified_, decision)) {
        const double elapsed = clock_.steadySeconds() - verifiedAt_;
        // A negative elapsed time means the clock moved back, so the old verification is not trusted.
        if (elapsed >= 0.0 && elapsed < kVerifyIntervalSeconds) {
            return;
        }
    }
    const std::optional<double> current = panel_.readPercent();
    if (current && levelAcceptedLocked(*current, target)) {
        // The level already matches, so the episode is over. A later drift gets fresh attempts.
        retries_ = 0;
        takeRetryLocked();
        markVerifiedLocked(decision);
        return;
    }
    // Parked: this episode has used its attempts. Later snapshots with the same decision neither write nor log.
    if (retries_ >= kMaxAttempts) {
        return;
    }
    static_cast<void>(panel_.setPercent(target));
    const std::optional<double> readBack = panel_.readPercent();
    if (!readBack || !levelAcceptedLocked(*readBack, target)) {
        // A failed check must not be trusted by the next snapshot, so the window is closed before anything else.
        forgetVerificationLocked();
        ++retries_;
        if (retries_ >= kMaxAttempts) {
            takeRetryLocked();
            log("builtin_dim_failed attempts=" + std::to_string(retries_));
            return;
        }
        log("builtin_dim unverified attempt=" + std::to_string(retries_));
        scheduleRetryLocked();
        return;
    }
    retries_ = 0;
    takeRetryLocked();
    markVerifiedLocked(decision);
    log("builtin_dim baseline=" + levelText(baseline) + " target=" + levelText(target));
}

// Writes the baseline back and verifies it by readback. Nothing is written while the display is unavailable.
void BuiltInProtection::restoreLocked(bool available)
{
    // A restore ends any verified dim. Without this, a display that goes away and comes back within the window
    // would be trusted at its old level, because the decision would match the verified one again.
    forgetVerificationLocked();
    if (!available || !baseline_) {
        return;
    }
    const double base = *baseline_;
    static_cast<void>(panel_.setPercent(base));
    const std::optional<double> readBack = panel_.readPercent();
    if (readBack && levelAcceptedLocked(*readBack, base)) {
        finishRestoreLocked(base);
        return;
    }
    ++retries_;
    if (retries_ >= kMaxAttempts) {
        // Parked: no timer. Each later snapshot, availability change or launch tries again.
        takeRetryLocked();
        restoreParked_ = true;
        log("builtin_restore_failed attempts=" + std::to_string(retries_));
        return;
    }
    log("builtin_restore unverified attempt=" + std::to_string(retries_));
    scheduleRetryLocked();
}

// Ends a verified restore: gives automatic brightness back, deletes the record and reports the end of holding.
void BuiltInProtection::finishRestoreLocked(double baseline)
{
    releaseAutoBrightnessLocked();
    removeRecordFile(recordFile_);
    holding_ = false;
    baseline_.reset();
    retries_ = 0;
    // Holding has ended, so both notes go with it.
    autoNotice_ = BuiltInNotice::none;
    restoreParked_ = false;
    forgetVerificationLocked();
    takeRetryLocked();
    reportHolding(false);
    log("builtin_restore baseline=" + levelText(baseline));
}

// The level is accepted when it is within kTolerancePoints of the supported level nearest to the target.
bool BuiltInProtection::levelAcceptedLocked(double level, double target) const
{
    return std::fabs(level - nearestLevelLocked(target)) <= kTolerancePoints;
}

// The supported level nearest to the target, with ties going to the lower level. Without a list, the target itself.
// A target of 0 maps to the lowest level, because every level is at or above 0.
double BuiltInProtection::nearestLevelLocked(double target) const
{
    const std::vector<int> levels = panel_.supportedLevels();
    if (levels.empty()) {
        return target;
    }
    int nearest = levels.front();
    for (const int level : levels) {
        if (std::fabs(level - target) < std::fabs(nearest - target)) {
            nearest = level;
        }
    }
    return static_cast<double>(nearest);
}

void BuiltInProtection::markVerifiedLocked(const BuiltinProtectionSnapshot& decision)
{
    verified_ = decision;
    verifiedAt_ = clock_.steadySeconds();
}

void BuiltInProtection::forgetVerificationLocked()
{
    verified_.reset();
}

void BuiltInProtection::takeRetryLocked()
{
    // Bumping the token makes a superseded callback do nothing when it fires.
    ++retryToken_;
    if (retryId_ != kInvalidTimerId) {
        pendingCancels_.push_back(retryId_);
    }
    retryId_ = kInvalidTimerId;
}

void BuiltInProtection::scheduleRetryLocked()
{
    takeRetryLocked();
    // A retry only follows a failed check, but the firing must verify on its own, never through the window.
    forgetVerificationLocked();
    const uint64_t token = retryToken_;
    retryId_ = scheduler_.scheduleOnce(kRetryDelay, toleranceFor(kRetryDelay), [this, token] { onRetryFired(token); });
}

void BuiltInProtection::onRetryFired(uint64_t token)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == retryToken_) {
            // This timer has fired, so it is not cancelled. Cancelling it from its own callback is not allowed.
            retryId_ = kInvalidTimerId;
            if (last_) {
                reconcileLocked(*last_);
            }
        }
    }
    flushCancels();
    reportNoticeIfChanged();
}

void BuiltInProtection::reportNoticeIfChanged()
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const BuiltInNotice current = noticeLocked();
        changed = current != reportedNotice_;
        reportedNotice_ = current;
    }
    if (changed && onNoticeChanged_) {
        onNoticeChanged_();
    }
}

void BuiltInProtection::reportHolding(bool holding)
{
    if (onHolding_) {
        onHolding_(holding);
    }
}

void BuiltInProtection::log(std::string_view message) const
{
    if (diagnostics_ != nullptr) {
        diagnostics_->log(LogLevel::Notice, message);
    }
}

void BuiltInProtection::flushCancels()
{
    std::vector<TimerId> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.swap(pendingCancels_);
    }
    for (const TimerId id : ids) {
        if (id != kInvalidTimerId) {
            scheduler_.cancel(id);
        }
    }
}

}  // namespace telaviva
