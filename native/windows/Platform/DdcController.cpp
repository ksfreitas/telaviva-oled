#include "DdcController.h"

#include "DdcTimingProfile.h"
#include "DdcTransaction.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {

namespace {

constexpr uint8_t kBrightnessCode = 0x10;
constexpr uint8_t kVolumeCode = 0x62;

// DdcResult carries only the current value, but discovery also reports the maximum.
// This keeps the last successful reading so DdcTransaction::readVcp can still do the retries.
class CapturingPort final : public IDdcPort {
public:
    explicit CapturingPort(IDdcPort& inner) : inner_(inner) {}

    bool readVcp(uint8_t code, DdcReading& out, DdcError& error) override
    {
        if (!inner_.readVcp(code, out, error)) {
            return false;
        }
        reading_ = out;
        return true;
    }

    bool writeVcp(uint8_t code, uint32_t value, DdcError& error) override
    {
        return inner_.writeVcp(code, value, error);
    }

    const std::optional<DdcReading>& reading() const { return reading_; }

private:
    IDdcPort& inner_;
    std::optional<DdcReading> reading_;
};

struct Outcome {
    bool ok = false;
    std::optional<double> value;
    std::string error;
};

struct MonitorRecord {
    bool ambiguous = false;
    std::shared_ptr<IDdcPort> port;  // null when unstable or without a DDC handle; used only on the executor
    DdcTimingProfile profile;
    bool hasGeneration = false;
    uint64_t generation = 0;
    std::string displayState;
    // Reported maximum per feature, for the readback percent; 100 when the feature is unknown.
    double brightnessMax = 100.0;
    double volumeMax = 100.0;
};

std::string errorOrIo(const std::string& error)
{
    return error.empty() ? std::string("io") : error;
}

// Rounds to the nearest raw value and clamps at 0 (NaN maps to 0).
uint32_t rawTarget(double value)
{
    const double rounded = std::round(value);
    if (!(rounded > 0.0)) {
        return 0;
    }
    if (rounded >= static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(rounded);
}

}  // namespace

// Every member is guarded by mutex_. Port calls, catalog enumeration, submissions and
// ISleeper calls happen with the lock released, so a re-entrant or inline executor cannot
// deadlock and the scheduler's cancel (which waits for a running callback) cannot either.
struct DdcController::Impl {
    Impl(IDisplayCatalog& catalog, IDdcPortProvider& ports, ICoreSubmitter& submitter, IDdcExecutor& executor,
         ISleeper& sleeper, IScheduler& scheduler, IHardwareObserver* observer)
        : catalog_(catalog), ports_(ports), submitter_(submitter), executor_(executor), sleeper_(sleeper),
          scheduler_(scheduler), observer_(observer)
    {
    }

    ~Impl() { stop(); }

    IDisplayCatalog& catalog_;
    IDdcPortProvider& ports_;
    ICoreSubmitter& submitter_;
    IDdcExecutor& executor_;
    ISleeper& sleeper_;
    IScheduler& scheduler_;
    IHardwareObserver* observer_;  // fixed at construction; may be null
    DdcTimingProfile graceProfile_;

    std::recursive_mutex mutex_;
    bool stopped_ = false;
    std::vector<CatalogEntry> knownCatalog_;
    std::map<std::string, MonitorRecord> monitors_;
    std::vector<PendingAction> latestActions_;
    std::set<uint64_t> running_;
    std::set<uint64_t> completed_;
    bool graceActive_ = false;
    uint64_t graceToken_ = 0;  // bumped on every display-state change so stale grace callbacks do nothing
    TimerId graceTimer_ = kInvalidTimerId;

    // Runs on the executor.
    void refreshOnExecutor()
    {
        std::vector<CatalogEntry> previous;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            previous = knownCatalog_;
        }

        const std::vector<CatalogEntry> current = catalog_.enumerate();
        const CatalogDiff diff = diffCatalogs(previous, current);
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            knownCatalog_ = current;
        }

        for (const std::string& removedId : diff.removedIds) {
            bool wasKnown = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                wasKnown = monitors_.erase(removedId) != 0;
            }
            // Only monitors the core knows get a disconnect; internal panels never do.
            if (wasKnown) {
                submit(buildMonitorDisconnected(removedId));
            }
        }

        for (const CatalogEntry& entry : diff.added) {
            if (entry.isInternalPanel) {
                continue;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
            }

            MonitorRecord record;
            record.ambiguous = !entry.identity.stable;
            record.profile = DdcTimingProfile::forManufacturer(entry.manufacturer);

            MonitorInfo info;
            info.id = entry.identity.id;
            info.name = entry.identity.name;
            info.identityStable = entry.identity.stable;
            info.connectionPath = entry.connectionPath;

            // An ambiguous identity may address the wrong panel, so it is never read or opened.
            if (!record.ambiguous) {
                record.port = ports_.open(entry);
                if (record.port) {
                    info.brightness = readFeature(*record.port, record.profile, kBrightnessCode);
                    info.volume = readFeature(*record.port, record.profile, kVolumeCode);
                }
            }
            // An unknown maximum keeps the 100 default; it is never read as 0.
            if (info.brightness.supported && info.brightness.max.has_value() && *info.brightness.max > 0.0) {
                record.brightnessMax = *info.brightness.max;
            }
            if (info.volume.supported && info.volume.max.has_value() && *info.volume.max > 0.0) {
                record.volumeMax = *info.volume.max;
            }

            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                // A re-discovered id replaces the old record; the next snapshot supplies the new generation.
                monitors_[info.id] = std::move(record);
            }
            submit(buildMonitorDiscovered(info));
        }
    }

    MonitorInfo::Channel readFeature(IDdcPort& port, const DdcTimingProfile& profile, uint8_t code)
    {
        CapturingPort capture(port);
        DdcTransaction transaction(capture, sleeper_, profile);
        const DdcResult result = transaction.readVcp(code);
        MonitorInfo::Channel channel;
        if (!result.ok || !capture.reading()) {
            return channel;
        }
        channel.supported = true;
        channel.raw = static_cast<double>(capture.reading()->current);
        channel.max = static_cast<double>(capture.reading()->maximum);
        return channel;
    }

    void handleSnapshot(const Snapshot& snapshot)
    {
        std::vector<PendingAction> ready;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            for (const MonitorSnapshot& monitor : snapshot.monitors) {
                auto found = monitors_.find(monitor.id);
                if (found == monitors_.end()) {
                    continue;
                }
                found->second.hasGeneration = true;
                found->second.generation = monitor.generation;
                found->second.displayState = monitor.displayState;
            }

            latestActions_ = snapshot.pendingActions;

            // Bound completed_ by dropping ids the core no longer lists.
            std::set<uint64_t> live;
            for (const PendingAction& action : latestActions_) {
                live.insert(action.id);
            }
            for (auto it = completed_.begin(); it != completed_.end();) {
                if (live.count(*it) == 0) {
                    it = completed_.erase(it);
                } else {
                    ++it;
                }
            }

            ready = takeReadyLocked();
        }
        startActions(std::move(ready));
    }

    void handleDisplayState(bool displayOn)
    {
        const char* state = displayOn ? "awake" : "asleep";
        std::vector<std::string> reports;
        TimerId previous = kInvalidTimerId;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            for (auto& entry : monitors_) {
                entry.second.displayState = state;
                if (entry.second.hasGeneration) {
                    reports.push_back(buildMonitorDisplayState(entry.first, entry.second.generation, state));
                }
            }

            ++graceToken_;
            previous = graceTimer_;
            graceTimer_ = kInvalidTimerId;
            graceActive_ = false;
            if (displayOn) {
                graceActive_ = true;
                const std::chrono::milliseconds grace = graceProfile_.wakeGrace;
                const uint64_t token = graceToken_;
                graceTimer_ = scheduler_.scheduleOnce(grace, toleranceFor(grace),
                                                      [this, token] { handleGraceEnd(token); });
                // Without a timer the grace cannot end on its own, so it is not armed.
                if (graceTimer_ == kInvalidTimerId) {
                    graceActive_ = false;
                }
            }
        }

        // Cancel outside the lock: cancel waits for a running grace callback, which needs the lock.
        if (previous != kInvalidTimerId) {
            scheduler_.cancel(previous);
        }
        for (const std::string& report : reports) {
            submit(report);
        }
    }

    void handleGraceEnd(uint64_t token)
    {
        std::vector<PendingAction> ready;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_ || token != graceToken_) {
                return;
            }
            graceActive_ = false;
            graceTimer_ = kInvalidTimerId;
            ready = takeReadyLocked();
        }
        startActions(std::move(ready));
    }

    void stop()
    {
        TimerId timer = kInvalidTimerId;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            stopped_ = true;
            graceActive_ = false;
            timer = graceTimer_;
            graceTimer_ = kInvalidTimerId;
        }
        if (timer != kInvalidTimerId) {
            scheduler_.cancel(timer);
        }
    }

    // Caller holds mutex_. Picks the first action of each monitor that may run now and marks it running.
    std::vector<PendingAction> takeReadyLocked()
    {
        std::vector<PendingAction> ready;
        std::set<std::string> seenMonitors;
        for (const PendingAction& action : latestActions_) {
            // The core exposes at most one action per monitor; only that first one counts.
            if (!seenMonitors.insert(action.monitorId).second) {
                continue;
            }
            if (completed_.count(action.id) != 0 || running_.count(action.id) != 0) {
                continue;
            }
            if (!mayRunLocked(action.monitorId)) {
                continue;
            }
            running_.insert(action.id);
            ready.push_back(action);
        }
        return ready;
    }

    // Caller holds mutex_. An undiscovered monitor may run (it is answered "disconnected").
    bool mayRunLocked(const std::string& monitorId) const
    {
        if (graceActive_) {
            return false;
        }
        auto found = monitors_.find(monitorId);
        return found == monitors_.end() || found->second.displayState != "asleep";
    }

    // Called without mutex_ held.
    void startActions(std::vector<PendingAction> ready)
    {
        for (const PendingAction& action : ready) {
            executor_.post([this, action] { runAction(action); });
        }
    }

    // Runs on the executor.
    void runAction(const PendingAction& action)
    {
        std::shared_ptr<IDdcPort> port;
        DdcTimingProfile profile;
        double maximum = 100.0;
        Outcome outcome;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_ || !mayRunLocked(action.monitorId)) {
                // Held: not completed, so a later snapshot or the end of the grace offers it again.
                running_.erase(action.id);
                return;
            }
            auto found = monitors_.find(action.monitorId);
            if (found == monitors_.end()) {
                outcome.error = "disconnected";
            } else if (found->second.ambiguous) {
                outcome.error = "ambiguous";
            } else if (!found->second.port) {
                outcome.error = "unsupported";
            } else {
                port = found->second.port;
                profile = found->second.profile;
                maximum = action.kind == "set_volume" ? found->second.volumeMax : found->second.brightnessMax;
            }
        }

        if (port) {
            outcome = perform(action, *port, profile);
        }

        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            running_.erase(action.id);
            if (stopped_) {
                return;
            }
            completed_.insert(action.id);
        }

        std::optional<double> value;
        std::optional<std::string> error;
        if (outcome.ok) {
            value = outcome.value;
        } else {
            error = outcome.error;
        }
        const std::string json = buildHardwareResult(action.id, action.generation, outcome.ok, value, error);
        // A rejected submission is not completed, so the next snapshot offers the action again.
        // The observer is told only once the core has the result, so a slot is never released early.
        if (submit(json) != 0) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            completed_.erase(action.id);
            return;
        }
        notifyCompletion(action, outcome, maximum);
    }

    // Runs on the executor, without mutex_ held. Only set actions answer a manual draft;
    // read_brightness is a probe and must not release one.
    void notifyCompletion(const PendingAction& action, const Outcome& outcome, double maximum)
    {
        if (observer_ == nullptr) {
            return;
        }
        const bool brightness = action.kind == "set_brightness";
        if (!brightness && action.kind != "set_volume") {
            return;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
        }
        HardwareCompletion completion;
        completion.monitorId = action.monitorId;
        completion.feature = brightness ? "brightness" : "volume";
        completion.generation = action.generation;
        completion.ok = outcome.ok;
        if (outcome.ok && outcome.value && maximum > 0.0) {
            completion.observedPercent = std::clamp(*outcome.value / maximum * 100.0, 0.0, 100.0);
        }
        observer_->onHardwareCompleted(completion);
    }

    Outcome perform(const PendingAction& action, IDdcPort& port, const DdcTimingProfile& profile)
    {
        DdcTransaction transaction(port, sleeper_, profile);
        Outcome outcome;
        if (action.kind == "set_brightness" || action.kind == "set_volume") {
            if (!action.value) {
                outcome.error = "unsupported";
                return outcome;
            }
            const uint8_t code = action.kind == "set_brightness" ? kBrightnessCode : kVolumeCode;
            const uint32_t target = rawTarget(*action.value);
            const DdcResult result = transaction.setAndVerify(code, target);
            if (result.ok) {
                outcome.ok = true;
                outcome.value = static_cast<double>(result.value ? *result.value : target);
            } else {
                outcome.error = errorOrIo(result.error);
            }
            return outcome;
        }
        if (action.kind == "read_brightness") {
            const DdcResult result = transaction.readVcp(kBrightnessCode);
            if (result.ok && result.value) {
                outcome.ok = true;
                outcome.value = static_cast<double>(*result.value);
            } else {
                outcome.error = errorOrIo(result.error);
            }
            return outcome;
        }
        outcome.error = "unsupported";
        return outcome;
    }

    // Returns 0 when the core accepted the command and -1 when the controller is stopped.
    int submit(const std::string& json)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return -1;
            }
        }
        return submitter_.submit(json);
    }
};

DdcController::DdcController(IDisplayCatalog& catalog, IDdcPortProvider& ports, ICoreSubmitter& submitter,
                             IDdcExecutor& executor, ISleeper& sleeper, IScheduler& scheduler,
                             IHardwareObserver* observer)
    : impl_(std::make_unique<Impl>(catalog, ports, submitter, executor, sleeper, scheduler, observer))
{
}

DdcController::~DdcController() = default;

void DdcController::refresh()
{
    Impl* impl = impl_.get();
    impl->executor_.post([impl] { impl->refreshOnExecutor(); });
}

void DdcController::onSnapshot(const Snapshot& snapshot)
{
    impl_->handleSnapshot(snapshot);
}

void DdcController::onDisplayState(bool displayOn)
{
    impl_->handleDisplayState(displayOn);
}

void DdcController::shutdown()
{
    impl_->stop();
}

}  // namespace telaviva
