#pragma once
// Manual-control adapter over the core's controls handle (contracts.md, architecture Decision 3).
// It turns UI, key-consumer, snapshot, discovery and executor callbacks into controls events,
// runs the returned effects after the lock is released, and owns the one timer that follows
// next_deadline through IScheduler. It holds no queue, step or retry policy of its own.
// No windows.h here.
#include "ControlsApi.h"
#include "ControlsTypes.h"
#include "DdcController.h"
#include "IAttemptRunner.h"
#include "IClock.h"
#include "JsonCodec.h"
#include "Scheduler.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace telaviva {

class ControlsDriver final : public IDdcListener {
public:
    using DraftObserver = std::function<void(const std::string& monitorId, const std::string& feature)>;

    // The driver holds references only. Every argument must outlive the driver. Work posted to
    // the executor must be drained before destruction, because it captures this driver.
    ControlsDriver(IControlsApi& api, ICoreSubmitter& submitter, IAttemptRunner& runner, IDdcExecutor& executor,
                   IScheduler& scheduler, IClock& clock);
    // Calls shutdown().
    ~ControlsDriver() override;

    ControlsDriver(const ControlsDriver&) = delete;
    ControlsDriver& operator=(const ControlsDriver&) = delete;

    // Called once before use. The observer runs outside the driver's lock, for draft_changed effects.
    void setDraftObserver(DraftObserver observer);

    // Sends the configure event. volumeOwner: absent leaves the owner unchanged.
    void configure(double brightnessStep, double volumeStep, bool keysEnabled,
                   const std::optional<std::optional<std::string>>& volumeOwner = std::nullopt);

    // Sends one core snapshot as an observe event, in snapshot order.
    void observe(const Snapshot& snapshot);

    // Slider value change. Returns the reply's draft; nullopt means the target was ignored.
    std::optional<DraftView> slider(const std::string& monitorId, const std::string& feature, double value);

    // Key event. Returns the core's consume decision (false when the reply has none).
    bool key(const std::string& key, bool down, bool repeat, bool fine, const std::optional<KeyOwnerRef>& owner);

    // Current draft for drawing a control. Thread-safe. Returns a default DraftView when the
    // core gives no draft.
    DraftView queryDraft(const std::string& monitorId, const std::string& feature) const;

    // IDdcListener: discovery reports from the DDC side.
    void onMonitorProfile(const std::string& monitorId, const std::string& manufacturer) override;
    void onMonitorRemoved(const std::string& monitorId) override;

    // Session display status: true when displays are on, false when they are off.
    void displayPower(bool on);

    // Sends the shutdown event once, cancels the timer and stops every later callback.
    // Idempotent.
    void shutdown();

private:
    // Sends one event under the lock, parses the reply and updates the timer bookkeeping.
    // Timer calls and the parse run outside the lock where the scheduler is involved.
    // An empty or invalid reply, or any call after shutdown, yields an empty result.
    ControlsResult send(const std::string& eventJson) const;

    // Sends an event and runs the effects of its reply.
    ControlsResult dispatch(const std::string& eventJson);

    // Runs effects in order, with the mutex released. depth bounds chains of effects that
    // answer effects (submitted, attempt_done).
    void execute(const ControlsResult& result, int depth) const;

    // Posts one DDC primitive to the executor; the outcome is sent back as attempt_done.
    void postAttempt(const ControlsEffect& effect, int depth) const;

    // Timer callback for one arming. Ignored when the epoch is stale or after shutdown.
    void onTimer(uint64_t epoch) const;

    double now() const;
    bool stoppedNow() const;

    IControlsApi& api_;
    ICoreSubmitter& submitter_;
    IAttemptRunner& runner_;
    IDdcExecutor& executor_;
    IScheduler& scheduler_;
    IClock& clock_;
    DraftObserver observer_;

    // Every field below is guarded by mutex_. Fields are mutable because queryDraft is const and
    // runs the reply's effects through the same path as the other calls.
    mutable std::mutex mutex_;
    mutable bool stopped_ = false;
    mutable bool armed_ = false;                                 // a timer is armed, or being armed
    mutable double armedDeadline_ = 0.0;                         // absolute steady seconds of that timer
    mutable TimerId timerId_ = kInvalidTimerId;                  // invalid while scheduleOnce is in flight
    mutable uint64_t armEpoch_ = 0;                              // bumped on every arm and cancel
};

}  // namespace telaviva
