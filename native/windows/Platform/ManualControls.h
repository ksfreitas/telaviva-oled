#pragma once
// Local slider drafts for monitor brightness and volume. A draft shows the
// user's target at once; the coordinator submits the newest target to the core
// and keeps at most one request in flight per monitor and feature.
// Threading: call from the owning UI thread only. Scheduler callbacks may run on
// a pool thread, so the implementation must hand them back to that thread.
#include "IClock.h"
#include "JsonCodec.h"
#include "Scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace telaviva {

enum class ManualFeature { Brightness, Volume };

// Ready: no unsettled draft. Pending: a draft is queued or in flight. Failed: the
// core or the hardware rejected the last submission, so the draft fell back to the observed value.
enum class ManualStatus { Ready, Pending, Failed };

// Most monitor/feature drafts held at once. Targets beyond this are dropped so the
// queue stays bounded, and the UI keeps showing the observed value for them.
inline constexpr std::size_t kManualMaxDrafts = 128;

// Readback distance, in percent, that counts as the draft having been reached.
// Matches the macOS HardwareKeyRouter default.
inline constexpr double kManualReadbackTolerance = 0.51;

// How long a submitted command may stay in flight without a completion before a snapshot
// may release it. A completion normally releases it first; this only covers a lost completion.
inline constexpr double kManualInFlightSafetySeconds = 3.0;

// Observes slider targets for the key router, as the macOS coordinator does. Calls are made outside
// ManualControls' lock. Target changes are reported in setTarget, and the order against a drain
// that is already pending is not guaranteed; the router then ignores that acceptance.
struct IManualObserver {
    virtual ~IManualObserver() = default;
    // A slider target was stored with this revision, before its submission.
    virtual void onTargetChanged(const std::string& monitorId, uint64_t connectionGeneration, ManualFeature feature,
                                 double value, uint64_t revision) = 0;
    // The sink accepted the slider target with this revision.
    virtual void onTargetSubmitted(const std::string& monitorId, uint64_t connectionGeneration, ManualFeature feature,
                                   double value, uint64_t revision) = 0;
};

// Receives one submission at a time per monitor and feature.
struct IManualSink {
    virtual ~IManualSink() = default;
    // Returns the TV_ status: 0 accepts the command, non-zero rejects it.
    virtual int submit(std::string_view monitorId, ManualFeature feature, double value,
                       uint64_t connectionGeneration) = 0;
};

class ManualControls {
public:
    // The sink, scheduler and clock must outlive this object. The clock times the safety window
    // and is never polled; the coordinator schedules no periodic work.
    ManualControls(IManualSink& sink, IScheduler& scheduler, IClock& clock);
    // Cancels a pending drain timer.
    ~ManualControls();

    ManualControls(const ManualControls&) = delete;
    ManualControls& operator=(const ManualControls&) = delete;

    // Optional decision trace, one line per event. Set once before the first call that can trace;
    // it is read without a lock afterwards. Empty means no line is built at all. The sink must not
    // call back into this object.
    using TraceSink = std::function<void(const std::string&)>;
    void setTrace(TraceSink trace);

    // Refreshes the known monitors from a snapshot. Monitors that disappeared, changed
    // generation, went asleep, or lost a stable identity lose their drafts and queued
    // targets. A draft clears when the readback is within kManualReadbackTolerance of
    // it, or when its request completed with nothing queued. A request is released by
    // onHardwareCompleted; a snapshot releases it only after kManualInFlightSafetySeconds
    // without a completion. Schedules a drain only when a target is waiting.
    void apply(const Snapshot& snapshot);

    // Stores the draft at once, clamped to 0..100, and replaces any unsent target with
    // it. Submission waits for one scheduled drain, so rapid calls coalesce to the
    // latest value. A target whose raw value equals the in-flight one is not queued
    // again. Ignored for non-finite values, unknown, asleep or unstable monitors, and
    // when the cap is reached for a new draft.
    void setTarget(const std::string& monitorId, ManualFeature feature, double value);

    // The same queue for a hardware-key write. Not reported to the observer: the key router owns that write.
    void setKeyTarget(const std::string& monitorId, ManualFeature feature, double value);

    // Optional observer, set once before the first target. Null means none.
    void setObserver(IManualObserver* observer);

    // Completion of a set_brightness or set_volume hardware action that this controller
    // submitted, reported after the core accepted its hardware_result. Thread-safe.
    // Matched by monitor and feature only: the core runs one action per monitor in order, and
    // its action generation is a different counter from the connection generation. Releases the
    // in-flight slot. A queued newer target goes out at once. Otherwise ok settles the draft to
    // Ready and clears it, and a failure marks it Failed with the observed value (observedPercent
    // when the hardware reported one). Ignored for an unknown monitor or when nothing is in flight.
    void onHardwareCompleted(const std::string& monitorId, ManualFeature feature, bool ok,
                             std::optional<double> observedPercent);

    // The value the UI should show: the draft while one exists, else the observed
    // value. Returns nullopt when the monitor or channel is unknown or ineligible.
    std::optional<double> draft(const std::string& monitorId, ManualFeature feature) const;

    // True while a draft exists for the monitor and feature.
    bool hasDraft(const std::string& monitorId, ManualFeature feature) const;

    // Status of the monitor and feature; Ready when no draft exists.
    ManualStatus status(const std::string& monitorId, ManualFeature feature) const;

    // Shows a hardware-key intent as a draft and drops any unsent slider target for the
    // same monitor and feature. The key router owns that submission, so nothing is
    // submitted here. The draft stays until the readback matches it or the core finishes
    // the action. Returns false, with no change, for unknown or stale generations,
    // non-finite or out-of-range values, and when the cap is reached.
    bool adoptExternalIntent(const std::string& monitorId, uint64_t connectionGeneration,
                             ManualFeature feature, double value);

    // Stops all work: cancels the drain timer, discards every draft and queued target,
    // and turns later calls into no-ops. Requests the core already accepted are left to
    // the core's own shutdown policy.
    void shutdown();

private:
    struct Channel {
        bool eligible = false;  // supported with an observed value, awake, stable identity
        double observed = 0.0;
        double min = 0.0;
        double max = 100.0;
        bool actionPending = false;  // the latest snapshot lists a pending action for this channel
    };

    struct MonitorState {
        uint64_t generation = 0;
        Channel channels[2];  // indexed by static_cast<std::size_t>(ManualFeature)
    };

    struct Slot {
        bool hasDraft = false;
        double draft = 0.0;
        ManualStatus status = ManualStatus::Ready;
        std::optional<double> queued;  // newest target not yet submitted
        std::optional<uint64_t> queuedRevision;  // revision of the queued slider target; empty for a key target
        bool inFlight = false;         // accepted by the sink, released by a completion or the safety window
        int64_t inFlightRaw = 0;       // raw value of the in-flight target, for deduplication
        uint64_t submittedRevision = 0;  // snapshot revision current when it was submitted
        double submittedAt = 0.0;      // steady clock reading at submission, for the safety window
        bool external = false;         // adopted hardware-key intent awaiting readback or completion
        bool corePendingSeen = false;  // the core listed the action at least once
        uint64_t submitToken = 0;      // identifies the in-flight sink call; 0 when none
    };

    void scheduleDrain(std::chrono::milliseconds delay);
    void drain();
    void setTargetFrom(const std::string& monitorId, ManualFeature feature, double value, bool fromSlider);

    IManualSink& sink_;
    IScheduler& scheduler_;
    IClock& clock_;
    IManualObserver* observer_ = nullptr;
    uint64_t targetRevision_ = 0;  // numbers slider targets, as the macOS coordinator does
    TraceSink trace_;
    std::map<std::string, MonitorState> monitors_;
    std::map<std::pair<std::string, ManualFeature>, Slot> slots_;
    std::optional<TimerId> drainTimer_;
    uint64_t snapshotRevision_ = 0;
    uint64_t submitCounter_ = 0;
    bool stopped_ = false;
    // Guards all state above; const accessors lock it too.
    mutable std::mutex mutex_;
};

}  // namespace telaviva
