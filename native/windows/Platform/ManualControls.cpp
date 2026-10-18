#include "ManualControls.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace telaviva {
namespace {

// Long enough that a drag coalesces into one submission, short enough to feel immediate.
constexpr std::chrono::milliseconds kDrainDelay{20};

// A completion that finds a queued target sends it without the drag debounce.
constexpr std::chrono::milliseconds kImmediateDrain{0};

std::size_t channelIndex(ManualFeature feature)
{
    return static_cast<std::size_t>(feature);
}

// Action kinds the core lists while a manual command is pending.
std::string_view actionKind(ManualFeature feature)
{
    return feature == ManualFeature::Brightness ? "set_brightness" : "set_volume";
}

// Builds the line only when a sink is set, so an idle controller formats nothing.
template <typename Build>
void emitTrace(const ManualControls::TraceSink& sink, Build build)
{
    if (sink) {
        sink(build());
    }
}

// Short form of a monitor id for traces: its last eight characters.
std::string traceId(const std::string& monitorId)
{
    return monitorId.size() > 8 ? monitorId.substr(monitorId.size() - 8) : monitorId;
}

std::string featureName(ManualFeature feature)
{
    return feature == ManualFeature::Brightness ? "brightness" : "volume";
}

std::string slotTag(const std::string& monitorId, ManualFeature feature)
{
    return traceId(monitorId) + " " + featureName(feature);
}

std::string traceNumber(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", value);
    return buffer;
}

// Same rounding as the macOS coordinator. Narrow ranges can map two percents to one
// raw value, which is what deduplication relies on.
int64_t rawFor(double min, double max, double percent)
{
    return static_cast<int64_t>(std::llround(min + (max - min) * percent / 100.0));
}

template <typename Slots>
std::size_t countDrafts(const Slots& slots)
{
    std::size_t drafts = 0;
    for (const auto& entry : slots) {
        if (entry.second.hasDraft) {
            ++drafts;
        }
    }
    return drafts;
}

struct ManualSubmission {
    std::string monitorId;
    ManualFeature feature;
    double value;
    uint64_t generation;
    uint64_t token;
    std::optional<uint64_t> revision;  // empty for a key target
};

}  // namespace

ManualControls::ManualControls(IManualSink& sink, IScheduler& scheduler, IClock& clock)
    : sink_(sink), scheduler_(scheduler), clock_(clock)
{
}

void ManualControls::setTrace(TraceSink trace)
{
    trace_ = std::move(trace);
}

ManualControls::~ManualControls()
{
    std::optional<TimerId> timer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        timer = drainTimer_;
        drainTimer_.reset();
    }
    // Cancel outside the lock: cancel waits for a running drain, and that drain needs the lock.
    if (timer.has_value()) {
        scheduler_.cancel(*timer);
    }
}

void ManualControls::apply(const Snapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || snapshot.revision < snapshotRevision_) {
        return;
    }
    snapshotRevision_ = snapshot.revision;

    auto dropSlots = [this](const std::string& monitorId) {
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (it->first.first == monitorId) {
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    };

    for (auto it = monitors_.begin(); it != monitors_.end();) {
        const std::string& id = it->first;
        const bool present = std::any_of(snapshot.monitors.begin(), snapshot.monitors.end(),
                                         [&id](const MonitorSnapshot& monitor) { return monitor.id == id; });
        if (present) {
            ++it;
            continue;
        }
        dropSlots(id);
        it = monitors_.erase(it);
    }

    for (const MonitorSnapshot& monitor : snapshot.monitors) {
        const auto existing = monitors_.find(monitor.id);
        if (existing != monitors_.end() && existing->second.generation != monitor.generation) {
            // A new connection generation must not inherit drafts from the old one.
            dropSlots(monitor.id);
        }
        MonitorState& state = monitors_[monitor.id];
        state.generation = monitor.generation;

        for (const ManualFeature feature : {ManualFeature::Brightness, ManualFeature::Volume}) {
            const ChannelSnapshot& source = feature == ManualFeature::Brightness ? monitor.brightness : monitor.volume;
            Channel& channel = state.channels[channelIndex(feature)];
            const std::string_view kind = actionKind(feature);

            channel = Channel{};
            channel.observed = source.observed.value_or(0.0);
            channel.min = source.min;
            channel.max = source.max;
            channel.eligible = source.supported && source.observed.has_value() &&
                               monitor.displayState != "asleep" && monitor.identityStatus == "stable" &&
                               source.max > source.min;
            channel.actionPending = std::any_of(
                snapshot.pendingActions.begin(), snapshot.pendingActions.end(),
                [&monitor, kind](const PendingAction& action) {
                    // Matched by monitor and kind only: the action generation is not the connection generation.
                    return action.monitorId == monitor.id && std::string_view(action.kind) == kind;
                });

            const auto found = slots_.find(std::make_pair(monitor.id, feature));
            if (found == slots_.end()) {
                continue;
            }
            Slot& slot = found->second;
            if (!channel.eligible) {
                // Sleep, unstable identity or an unsupported channel ends the draft and any in-flight state.
                emitTrace(trace_, [&] { return "manual: " + slotTag(monitor.id, feature) + " slot reset reason=not eligible"; });
                slot = Slot{};
                continue;
            }

            if (channel.actionPending) {
                slot.corePendingSeen = true;
            }
            // Safety net only: the completion normally releases the slot. A snapshot may release it
            // once it has outlived the window, so a lost completion cannot wedge the channel. A snapshot
            // published while the command is still being submitted is always inside the window.
            const double age = clock_.steadySeconds() - slot.submittedAt;
            const bool outlivedWindow = age >= kManualInFlightSafetySeconds;
            if (slot.inFlight && outlivedWindow && snapshot.revision > slot.submittedRevision &&
                !channel.actionPending) {
                slot.inFlight = false;
                emitTrace(trace_, [&] { return "manual: " + slotTag(monitor.id, feature) + " in-flight released by safety net age=" + traceNumber(age) + "s"; });
            }
            // An adopted hardware-key action is done once the core lists it and then stops listing it.
            if (slot.external && slot.corePendingSeen && !channel.actionPending) {
                slot.external = false;
                slot.corePendingSeen = false;
            }

            if (slot.hasDraft && slot.status == ManualStatus::Pending && !slot.queued) {
                const bool readback = std::fabs(channel.observed - slot.draft) <= kManualReadbackTolerance;
                const bool completed = !slot.inFlight && !slot.external;
                if (readback || completed) {
                    slot.hasDraft = false;
                    slot.status = ManualStatus::Ready;
                    slot.draft = 0.0;
                    slot.external = false;
                    slot.corePendingSeen = false;
                }
            }
            // A failed draft follows the observed value; it never retries on its own.
            if (slot.hasDraft && slot.status == ManualStatus::Failed) {
                slot.draft = channel.observed;
            }
            if (slot.queued && !slot.inFlight) {
                scheduleDrain(kDrainDelay);
            }
        }
    }
}

void ManualControls::setTarget(const std::string& monitorId, ManualFeature feature, double value)
{
    setTargetFrom(monitorId, feature, value, true);
}

void ManualControls::setKeyTarget(const std::string& monitorId, ManualFeature feature, double value)
{
    setTargetFrom(monitorId, feature, value, false);
}

void ManualControls::setObserver(IManualObserver* observer)
{
    observer_ = observer;
}

void ManualControls::setTargetFrom(const std::string& monitorId, ManualFeature feature, double value, bool fromSlider)
{
    if (!std::isfinite(value)) {
        return;
    }
    const double target = value < 0.0 ? 0.0 : (value > 100.0 ? 100.0 : value);

    std::optional<uint64_t> changedRevision;
    uint64_t changedGeneration = 0;
    {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " target ignored reason=stopped"; });
        return;
    }
    const auto monitor = monitors_.find(monitorId);
    if (monitor == monitors_.end()) {
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " target ignored reason=unknown monitor"; });
        return;
    }
    const Channel& channel = monitor->second.channels[channelIndex(feature)];
    if (!channel.eligible) {
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " target ignored reason=not eligible"; });
        return;
    }

    const auto key = std::make_pair(monitorId, feature);
    auto found = slots_.find(key);
    if (found == slots_.end() || !found->second.hasDraft) {
        if (countDrafts(slots_) >= kManualMaxDrafts) {
            emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " target ignored reason=draft cap"; });
            return;
        }
        if (found == slots_.end()) {
            found = slots_.emplace(key, Slot{}).first;
        }
    }

    Slot& slot = found->second;
    slot.hasDraft = true;
    slot.draft = target;
    slot.status = ManualStatus::Pending;
    slot.external = false;
    slot.corePendingSeen = false;

    // Same raw value as the in-flight command: nothing new to send, and any unsent target is stale.
    const int64_t raw = rawFor(channel.min, channel.max, target);
    if (slot.inFlight && raw == slot.inFlightRaw) {
        slot.queued.reset();
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " target=" + traceNumber(target) + " raw=" + std::to_string(raw) + " same as in flight, not queued"; });
        return;
    }
    slot.queued = target;
    if (fromSlider) {
        slot.queuedRevision = ++targetRevision_;
        changedRevision = slot.queuedRevision;
        changedGeneration = monitor->second.generation;
    } else {
        slot.queuedRevision.reset();
    }
    emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " target accepted value=" + traceNumber(target) + " raw=" + std::to_string(raw); });
    scheduleDrain(kDrainDelay);
    }
    // Outside the lock: the observer may call back into the key router, which has its own lock.
    if (changedRevision.has_value() && observer_ != nullptr) {
        observer_->onTargetChanged(monitorId, changedGeneration, feature, target, *changedRevision);
    }
}

void ManualControls::onHardwareCompleted(const std::string& monitorId, ManualFeature feature, bool ok,
                                         std::optional<double> observedPercent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " completion ignored reason=stopped"; });
        return;
    }
    const auto monitor = monitors_.find(monitorId);
    if (monitor == monitors_.end()) {
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " completion ignored reason=unknown monitor"; });
        return;
    }
    const auto found = slots_.find(std::make_pair(monitorId, feature));
    // Nothing in flight: the safety window already released the slot, or the draft was dropped.
    if (found == slots_.end() || !found->second.inFlight) {
        emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " completion ignored reason=not in flight"; });
        return;
    }

    Slot& slot = found->second;
    emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " completion received ok=" + (ok ? "1" : "0"); });
    slot.inFlight = false;
    if (slot.queued.has_value()) {
        // The newer target gets its own attempt now, whatever this result was. Its draft is already shown.
        scheduleDrain(kImmediateDrain);
        return;
    }

    if (ok) {
        // The readback reaches the UI with the next snapshot; until then the observed value shows.
        slot.hasDraft = false;
        slot.status = ManualStatus::Ready;
        slot.draft = 0.0;
        slot.external = false;
        slot.corePendingSeen = false;
        return;
    }

    const Channel& channel = monitor->second.channels[channelIndex(feature)];
    slot.hasDraft = true;
    slot.status = ManualStatus::Failed;
    slot.draft = observedPercent.value_or(channel.observed);
    slot.external = false;
    slot.corePendingSeen = false;
}

std::optional<double> ManualControls::draft(const std::string& monitorId, ManualFeature feature) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto monitor = monitors_.find(monitorId);
    if (monitor == monitors_.end()) {
        return std::nullopt;
    }
    const Channel& channel = monitor->second.channels[channelIndex(feature)];
    if (!channel.eligible) {
        return std::nullopt;
    }
    const auto found = slots_.find(std::make_pair(monitorId, feature));
    if (found != slots_.end() && found->second.hasDraft) {
        return found->second.draft;
    }
    return channel.observed;
}

bool ManualControls::hasDraft(const std::string& monitorId, ManualFeature feature) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(std::make_pair(monitorId, feature));
    return found != slots_.end() && found->second.hasDraft;
}

ManualStatus ManualControls::status(const std::string& monitorId, ManualFeature feature) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slots_.find(std::make_pair(monitorId, feature));
    if (found != slots_.end() && found->second.hasDraft) {
        return found->second.status;
    }
    return ManualStatus::Ready;
}

bool ManualControls::adoptExternalIntent(const std::string& monitorId, uint64_t connectionGeneration,
                                         ManualFeature feature, double value)
{
    if (!std::isfinite(value) || value < 0.0 || value > 100.0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        return false;
    }
    const auto monitor = monitors_.find(monitorId);
    if (monitor == monitors_.end() || monitor->second.generation != connectionGeneration) {
        return false;
    }
    if (!monitor->second.channels[channelIndex(feature)].eligible) {
        return false;
    }

    const auto key = std::make_pair(monitorId, feature);
    auto found = slots_.find(key);
    if (found == slots_.end() || !found->second.hasDraft) {
        if (countDrafts(slots_) >= kManualMaxDrafts) {
            return false;
        }
        if (found == slots_.end()) {
            found = slots_.emplace(key, Slot{}).first;
        }
    }

    // The key router owns the submission, so the unsent slider target is dropped rather than sent.
    Slot& slot = found->second;
    slot.hasDraft = true;
    slot.draft = value;
    slot.status = ManualStatus::Pending;
    slot.queued.reset();
    slot.external = true;
    slot.corePendingSeen = false;
    return true;
}

void ManualControls::shutdown()
{
    std::optional<TimerId> timer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        timer = drainTimer_;
        drainTimer_.reset();
        slots_.clear();
        monitors_.clear();
    }
    if (timer.has_value()) {
        scheduler_.cancel(*timer);
    }
}

// Caller holds mutex_.
void ManualControls::scheduleDrain(std::chrono::milliseconds delay)
{
    if (stopped_ || drainTimer_.has_value()) {
        return;
    }
    const TimerId id = scheduler_.scheduleOnce(delay, toleranceFor(delay), [this] { drain(); });
    if (id != kInvalidTimerId) {
        drainTimer_ = id;
    }
}

void ManualControls::drain()
{
    std::vector<ManualSubmission> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drainTimer_.reset();
        if (stopped_) {
            return;
        }
        for (auto& entry : slots_) {
            Slot& slot = entry.second;
            if (!slot.queued.has_value()) {
                continue;
            }
            const std::string& monitorId = entry.first.first;
            const ManualFeature feature = entry.first.second;
            const auto monitor = monitors_.find(monitorId);
            if (monitor == monitors_.end()) {
                emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " drain skipped reason=no monitor"; });
                continue;
            }
            const Channel& channel = monitor->second.channels[channelIndex(feature)];
            if (!channel.eligible) {
                emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " drain skipped reason=not eligible"; });
                continue;
            }
            if (slot.inFlight) {
                emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " drain skipped reason=inFlight"; });
                continue;
            }
            // The core is already working on a manual command for this channel; wait for it to finish.
            if (channel.actionPending) {
                emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " drain skipped reason=actionPending"; });
                continue;
            }

            const double value = *slot.queued;
            const std::optional<uint64_t> revision = slot.queuedRevision;
            slot.queued.reset();
            slot.queuedRevision.reset();
            slot.draft = value;
            slot.inFlight = true;
            slot.inFlightRaw = rawFor(channel.min, channel.max, value);
            slot.submittedRevision = snapshotRevision_;
            slot.submittedAt = clock_.steadySeconds();
            slot.submitToken = ++submitCounter_;
            emitTrace(trace_, [&] { return "manual: " + slotTag(monitorId, feature) + " submitted value=" + traceNumber(value) + " raw=" + std::to_string(slot.inFlightRaw) + " generation=" + std::to_string(monitor->second.generation) + " token=" + std::to_string(slot.submitToken) + " revision=" + std::to_string(slot.submittedRevision); });
            batch.push_back(ManualSubmission{monitorId, feature, value, monitor->second.generation, slot.submitToken,
                                             revision});
        }
    }

    // The sink runs without the lock so a callback into this object cannot deadlock.
    for (const ManualSubmission& item : batch) {
        const int status = sink_.submit(item.monitorId, item.feature, item.value, item.generation);
        if (status == 0) {
            // Outside the lock, like the sink call. Only slider targets carry a revision.
            if (item.revision.has_value() && observer_ != nullptr) {
                observer_->onTargetSubmitted(item.monitorId, item.generation, item.feature, item.value, *item.revision);
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        emitTrace(trace_, [&] { return "manual: " + slotTag(item.monitorId, item.feature) + " sink rejected status=" + std::to_string(status) + " token=" + std::to_string(item.token); });
        const auto found = slots_.find(std::make_pair(item.monitorId, item.feature));
        // Stale when the slot was reset or replaced while the sink ran.
        if (found == slots_.end() || found->second.submitToken != item.token) {
            continue;
        }
        Slot& slot = found->second;
        slot.inFlight = false;
        if (slot.queued.has_value()) {
            // A newer target arrived meanwhile and gets its own attempt.
            scheduleDrain(kDrainDelay);
            continue;
        }
        slot.hasDraft = true;
        slot.status = ManualStatus::Failed;
        const auto monitor = monitors_.find(item.monitorId);
        if (monitor != monitors_.end() && monitor->second.channels[channelIndex(item.feature)].eligible) {
            slot.draft = monitor->second.channels[channelIndex(item.feature)].observed;
        }
    }
}

}  // namespace telaviva
