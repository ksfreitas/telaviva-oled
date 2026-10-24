#pragma once
// Built-in (laptop) panel protection: dims the built-in screen while the core reports presence or screensaver
// reasons, restores it, and keeps a crash-recovery record. This is the Windows mirror of
// native/macos/TelaViva/BuiltInProtectionController.swift. The owner of the OS "automatic brightness" setting
// is AutoBrightnessOwner; the app's "Automatic brightness" page is a different feature.
//
// Threading: the public calls come from one thread, the thread that calls AppController's Services::publish.
// The retry timer fires on the scheduler's thread, so every method takes mutex_. The panel, the owner and
// onHolding run while mutex_ is held; none of them calls back into this controller. Timers are cancelled only
// after mutex_ is released, because ThreadpoolScheduler::cancel may wait for a running callback. onNoticeChanged
// runs after mutex_ is released, so it may call notice() and isHolding().
//
// Record: builtin-protection-session.json (schema version 1: started_at, baseline_percent), written atomically
// before the first change. Never throws.
#include "AutoBrightnessOwner.h"
#include "Diagnostics.h"
#include "IClock.h"
#include "ILaptopPanel.h"
#include "JsonCodec.h"
#include "Scheduler.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace telaviva {

// The built-in screen is listed in the popup card and the General page protection list only while this is true.
// A lid closed or display off makes available false; protectable stays true so the camera logic still sees it.
inline bool builtinCardVisible(const BuiltinProtectionSnapshot& snapshot)
{
    return snapshot.protectable && snapshot.available;
}

// The one note the built-in card shows; none shows no note. The meanings mirror BuiltInProtectionController's notice on
// macOS. autoBrightnessUnknown is an unreadable setting; autoBrightnessNotChanged is a setting that was on
// but could not be turned off.
enum class BuiltInNotice { none, autoBrightnessUnknown, autoBrightnessNotChanged, restoreFailed };

class BuiltInProtection final {
public:
    // panel and owner must outlive the controller. recordFile is builtin-protection-session.json. diagnostics may be
    // null. onHolding is called with each change of holding, after the record is written (true) or deleted (false).
    // onNoticeChanged is called after a change of notice() made by the retry timer. reconcile and restoreNow do not call
    // it: their callers build the flyout from notice() right after the call. clock supplies the monotonic reading for
    // the 5 s verification window and the record timestamp.
    BuiltInProtection(ILaptopPanel& panel, AutoBrightnessOwner& owner, IScheduler& scheduler, IClock& clock,
                      std::filesystem::path recordFile, Diagnostics* diagnostics, std::function<void(bool)> onHolding,
                      std::function<void()> onNoticeChanged);
    // Cancels the pending retry, if any.
    ~BuiltInProtection();

    BuiltInProtection(const BuiltInProtection&) = delete;
    BuiltInProtection& operator=(const BuiltInProtection&) = delete;

    // Applies one decision from the core. Runs on every snapshot and is level-triggered, so repeats are safe.
    void reconcile(const BuiltinProtectionSnapshot& snapshot);
    // Launch path, before the first reconcile. A leftover record starts holding with the stored baseline, and the
    // level is not read. An unreadable record is deleted and logged.
    void recoverIfNeeded();
    // Quit path. One synchronous restore attempt and no timers. A failed write keeps the record for the next launch.
    // Later reconcile calls are ignored.
    void restoreNow();
    // True from the first dim (or a leftover record) until a verified restore.
    bool isHolding() const;
    // The note for the built-in card. restoreFailed wins while a restore is parked or pending; the automatic
    // brightness note shows only while the option is requested, and is cleared when holding ends.
    BuiltInNotice notice() const;

private:
    // Each of these expects mutex_ held.
    BuiltInNotice noticeLocked() const;
    void reconcileLocked(const BuiltinProtectionSnapshot& snapshot);
    void dimLocked(double target, const BuiltinProtectionSnapshot& decision);
    bool startHoldingLocked();
    void syncAutoBrightnessLocked(bool holdAutoBrightness);
    void releaseAutoBrightnessLocked();
    void dimLevelLocked(double target, double baseline, const BuiltinProtectionSnapshot& decision);
    void restoreLocked(bool available);
    void finishRestoreLocked(double baseline);
    bool levelAcceptedLocked(double level, double target) const;
    double nearestLevelLocked(double target) const;
    void markVerifiedLocked(const BuiltinProtectionSnapshot& decision);
    void forgetVerificationLocked();
    // Moves the pending retry to pendingCancels_, to be cancelled by flushCancels after unlocking.
    void takeRetryLocked();
    void scheduleRetryLocked();
    void reportHolding(bool holding);
    void log(std::string_view message) const;

    void onRetryFired(uint64_t token);
    // Cancels the queued timers. Called without mutex_ held.
    void flushCancels();
    // Calls onNoticeChanged_ when notice() differs from the last one reported. Called without mutex_ held.
    void reportNoticeIfChanged();

    ILaptopPanel& panel_;
    AutoBrightnessOwner& owner_;
    IScheduler& scheduler_;
    IClock& clock_;
    std::filesystem::path recordFile_;
    Diagnostics* diagnostics_;
    std::function<void(bool)> onHolding_;
    std::function<void()> onNoticeChanged_;

    mutable std::mutex mutex_;
    std::optional<double> baseline_;
    bool holding_ = false;
    // True after this controller asked the owner for automatic brightness, until it released it.
    bool autoHeld_ = false;
    // The automatic brightness note for the current hold; none unless the option was requested and the acquire failed.
    BuiltInNotice autoNotice_ = BuiltInNotice::none;
    // True while a restore is parked after its attempts, or pending after a failed quit restore.
    bool restoreParked_ = false;
    // The notice last passed to onNoticeChanged_.
    BuiltInNotice reportedNotice_ = BuiltInNotice::none;
    // Failed dim or restore attempts in the current episode.
    int retries_ = 0;
    // Set by restoreNow and the destructor. Later reconcile calls and retries do nothing.
    bool stopped_ = false;
    std::optional<BuiltinProtectionSnapshot> last_;
    // The decision of the last verified level, and when it was verified (steadySeconds). Used for dims only.
    std::optional<BuiltinProtectionSnapshot> verified_;
    double verifiedAt_ = 0.0;
    // The pending retry, if any. Cleared when it fires. retryToken_ changes on every schedule and take, so a
    // superseded callback does nothing.
    TimerId retryId_ = kInvalidTimerId;
    uint64_t retryToken_ = 0;
    std::vector<TimerId> pendingCancels_;
};

}  // namespace telaviva
