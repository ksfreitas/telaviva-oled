#include "KeyIntentRouter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace telaviva {

KeyIntentRouter::KeyIntentRouter(IClock& clock) : clock_(clock) {}

double KeyIntentRouter::clampedStep(double value)
{
    if (!std::isfinite(value)) {
        return kKeyDefaultStep;
    }
    return std::min(kKeyMaxStep, std::max(kKeyMinStep, std::round(value)));
}

double KeyIntentRouter::steppedVolume(double value, bool increase, bool fine, double step)
{
    const double amount = fine ? kKeyFineStep : step;
    const double delta = increase ? amount : -amount;
    return std::min(100.0, std::max(0.0, value + delta));
}

void KeyIntentRouter::configure(const KeyRouteConfig& config)
{
    if (stopped_) {
        return;
    }
    enabled_ = config.enabled;
    if (!enabled_) {
        clearPending();
        owner_.clear();
    } else if (config.ownerMonitorId != owner_) {
        muteMemory_.clear();  // Remembered levels belong to the previous sound output.
        owner_ = config.ownerMonitorId;
    }
    step_ = clampedStep(config.step);
}

void KeyIntentRouter::observe(const VolumeObservation& observation)
{
    if (stopped_) {
        return;
    }
    observation_ = observation;
    reconcile();
}

bool KeyIntentRouter::handleKey(const KeyEvent& event)
{
    if (stopped_) {
        return false;
    }
    reconcile();
    return process(event);
}

std::vector<KeyIntent> KeyIntentRouter::takeIntents()
{
    std::vector<KeyIntent> taken;
    taken.swap(ready_);
    return taken;
}

void KeyIntentRouter::intentFinished(const KeyIntent& intent, bool accepted)
{
    complete(Key{intent.monitorId, intent.generation}, intent.revision, intent.authorizationEpoch, accepted);
}

void KeyIntentRouter::actionAccepted(const std::string& monitorId, uint64_t generation, uint64_t actionGeneration)
{
    const Key key{monitorId, generation};
    auto early = earlyCompleted_.find(key);
    if (early != earlyCompleted_.end() && early->second.erase(actionGeneration) > 0) {
        if (early->second.empty()) {
            earlyCompleted_.erase(early);
        }
        return;  // The completion arrived first and already settled this acceptance.
    }
    registerOutstanding(key, actionGeneration, nowMs());
}

void KeyIntentRouter::actionCompleted(const std::string& monitorId, uint64_t generation, uint64_t actionGeneration)
{
    const Key key{monitorId, generation};
    const uint64_t now = nowMs();
    auto outstanding = outstanding_.find(key);
    if (outstanding != outstanding_.end() && outstanding->second.erase(actionGeneration) > 0) {
        if (outstanding->second.empty()) {
            outstanding_.erase(outstanding);
        }
    } else {
        // The acceptance has not been registered yet; remember the completion so that it cancels the acceptance.
        std::size_t total = 0;
        for (const auto& entry : earlyCompleted_) {
            total += entry.second.size();
        }
        if (total >= kKeyMaxEarlyCompleted) {
            earlyCompleted_.clear();
        }
        earlyCompleted_[key].insert(actionGeneration);
    }
    rebaseBlockedUntil_[key] = now + kKeyRebaseSettleMs;
}

void KeyIntentRouter::manualControlChanged(const std::string& monitorId, uint64_t generation, double value,
                                           uint64_t revision)
{
    if (!std::isfinite(value) || value < 0.0 || value > 100.0) {
        return;
    }
    if (!enabled_ || stopped_) {
        return;
    }
    const Key key{monitorId, generation};
    ++intentRevision_;
    if (intents_.count(key) != 0 || intents_.size() < kKeyMaxIntents) {
        // Replacing the authorization epoch also invalidates a write already handed out.
        Intent intent;
        intent.target = value;
        intent.revision = intentRevision_;
        intent.authorizationEpoch = intentRevision_;
        intent.manualRevision = revision;
        intent.lastObserved = observedValue(key);
        intents_[key] = intent;
    }
}

void KeyIntentRouter::manualControlAccepted(const std::string& monitorId, uint64_t generation, double value,
                                            uint64_t revision)
{
    const Key key{monitorId, generation};
    auto intent = intents_.find(key);
    if (intent == intents_.end()) {
        return;
    }
    if (!intent->second.manualRevision || *intent->second.manualRevision != revision ||
        intent->second.target != value) {
        return;
    }
    intent->second.lastObserved = observedValue(key);
}

bool KeyIntentRouter::isIntentCurrent(const KeyIntent& intent) const
{
    if (!enabled_ || stopped_) {
        return false;
    }
    const Key key{intent.monitorId, intent.generation};
    const auto found = intents_.find(key);
    if (found == intents_.end()) {
        return false;
    }
    const Intent& current = found->second;
    if (current.authorizationEpoch != intent.authorizationEpoch || current.revision != intent.revision ||
        current.target != intent.targetVolume) {
        return false;
    }
    const VolumeObservation* display = displayFor(key);
    return display != nullptr && display->supported && display->awake && !display->failed;
}

void KeyIntentRouter::shutdown()
{
    enabled_ = false;
    stopped_ = true;
    clearPending();
    owner_.clear();
}

uint64_t KeyIntentRouter::nowMs() const
{
    const double seconds = clock_.steadySeconds();
    if (!(seconds > 0.0)) {
        return 0;
    }
    return static_cast<uint64_t>(std::llround(seconds * 1000.0));
}

const VolumeObservation* KeyIntentRouter::displayFor(const Key& key) const
{
    if (!observation_ || observation_->monitorId != key.first ||
        observation_->connectionGeneration != key.second) {
        return nullptr;
    }
    return &*observation_;
}

bool KeyIntentRouter::isOwner(const VolumeObservation& observation) const
{
    return !owner_.empty() && observation.monitorId == owner_;
}

// Volume follows the owner only while it is supported, awake and reports a value in range.
const VolumeObservation* KeyIntentRouter::ownerDisplay() const
{
    if (!observation_ || !isOwner(*observation_)) {
        return nullptr;
    }
    const VolumeObservation& display = *observation_;
    if (!display.supported || !display.awake || !std::isfinite(display.percent) || display.percent < 0.0 ||
        display.percent > 100.0) {
        return nullptr;
    }
    return &display;
}

std::optional<double> KeyIntentRouter::observedValue(const Key& key) const
{
    const VolumeObservation* display = displayFor(key);
    if (display == nullptr || !std::isfinite(display->percent)) {
        return std::nullopt;
    }
    return display->percent;
}

// The pending intent's target wins over the readback, so a held key keeps stepping from what it already wrote.
std::optional<double> KeyIntentRouter::valueFor(const Key& key) const
{
    const auto found = intents_.find(key);
    if (found != intents_.end()) {
        return found->second.target;
    }
    return observedValue(key);
}

bool KeyIntentRouter::process(const KeyEvent& event)
{
    std::optional<Latch>& slot = latches_[index(event.key)];
    const bool isMute = event.key == VolumeKey::Mute;
    const bool increase = event.key == VolumeKey::Up;

    if (event.phase == KeyPhase::Up) {
        const std::optional<Latch> held = std::exchange(slot, std::nullopt);
        return held && (held->kind == LatchKind::Handled || held->kind == LatchKind::Mute);
    }

    if (!event.isRepeat) {
        slot.reset();
    }
    if (slot) {
        // A repeat of a press already decided: keep the decision for the whole sequence.
        Latch& held = *slot;
        if (held.kind == LatchKind::Mute) {
            // Repeats of a held mute are consumed without toggling again.
            if (!held.cancelled) {
                const VolumeObservation* display = displayFor(held.key);
                if (display != nullptr && isOwner(*display) && display->supported && display->awake &&
                    valueFor(held.key)) {
                    return true;
                }
            }
            held.cancelled = true;
            return true;
        }
        if (held.kind == LatchKind::PassThrough) {
            return false;
        }
        if (!held.cancelled) {
            const VolumeObservation* display = displayFor(held.key);
            std::optional<double> value;
            if (display != nullptr && isOwner(*display) && display->supported && display->awake) {
                value = valueFor(held.key);
            }
            if (value) {
                reserveStep(held.key, held.increase, held.fine, *value);
                return true;
            }
        }
        held.cancelled = true;
        return true;
    }

    // A fresh press: decide once and latch the decision for its repeats and release.
    if (event.isRepeat || !enabled_ || !event.modifiersEligible) {
        slot = Latch{};
        return false;
    }
    const VolumeObservation* display = ownerDisplay();
    if (display == nullptr) {
        slot = Latch{};
        return false;
    }
    const Key key{display->monitorId, display->connectionGeneration};
    const std::optional<double> value = valueFor(key);
    bool reserved = false;
    if (value) {
        reserved = isMute ? reserveMute(key, *value) : reserveStep(key, increase, event.isFineStep, *value);
    }
    if (!reserved) {
        slot = Latch{};
        return false;
    }
    Latch latch;
    latch.kind = isMute ? LatchKind::Mute : LatchKind::Handled;
    latch.key = key;
    latch.increase = increase;
    latch.fine = event.isFineStep;
    slot = latch;
    return true;
}

bool KeyIntentRouter::reserveStep(const Key& key, bool increase, bool fine, double value)
{
    const auto remembered = muteMemory_.find(key);
    if (remembered != muteMemory_.end()) {
        const double level = remembered->second;
        if (value != 0.0) {
            muteMemory_.erase(remembered);  // Changed elsewhere since the mute.
        } else if (!increase) {
            return true;  // Still muted: consume without a write and keep the level.
        } else {
            // Restore the remembered level, then step up from it, as one write.
            if (!reserveWrite(key, value, steppedVolume(level, increase, fine, step_))) {
                return false;
            }
            muteMemory_.erase(key);
            return true;
        }
    }
    return reserveWrite(key, value, steppedVolume(value, increase, fine, step_));
}

// A mute press remembers the level and writes 0; the next mute press restores it.
bool KeyIntentRouter::reserveMute(const Key& key, double value)
{
    if (value != 0.0) {
        muteMemory_.erase(key);  // Stale memory when the level changed elsewhere.
    }
    if (value > 0.0) {
        if (muteMemory_.size() >= kKeyMaxMuteMemory || !reserveWrite(key, value, 0.0)) {
            return false;
        }
        muteMemory_[key] = value;
        return true;
    }
    const auto remembered = muteMemory_.find(key);
    if (remembered == muteMemory_.end()) {
        return true;  // At zero with nothing to restore.
    }
    const double level = remembered->second;
    if (!reserveWrite(key, value, level)) {
        return false;
    }
    muteMemory_.erase(key);
    return true;
}

bool KeyIntentRouter::reserveWrite(const Key& key, double value, double next)
{
    if (intents_.count(key) == 0 && intents_.size() >= kKeyMaxIntents) {
        return false;
    }
    if (inFlight_.count(key) == 0 && inFlight_.size() >= kKeyMaxInFlight) {
        return false;
    }
    if (next == value) {
        return true;
    }
    ++intentRevision_;
    auto found = intents_.find(key);
    if (found == intents_.end()) {
        Intent created;
        created.target = value;
        created.revision = intentRevision_;
        created.authorizationEpoch = intentRevision_;
        created.lastObserved = observedValue(key);
        found = intents_.emplace(key, created).first;
    }
    Intent& intent = found->second;
    intent.target = next;
    intent.revision = intentRevision_;
    const bool needsWorker = !intent.scheduled && inFlight_.count(key) == 0;
    intent.scheduled = true;
    if (needsWorker) {
        flush(key);
    }
    return true;
}

// Hands the newest target of a key to the caller, unless a write for that key is already in flight.
void KeyIntentRouter::flush(const Key& key)
{
    const auto found = intents_.find(key);
    const VolumeObservation* display = displayFor(key);
    if (!enabled_ || found == intents_.end() || display == nullptr || !display->supported || !display->awake) {
        intents_.erase(key);
        return;
    }
    if (inFlight_.count(key) != 0 || inFlight_.size() >= kKeyMaxInFlight) {
        return;
    }
    inFlight_.insert(key);
    const Intent& intent = found->second;
    KeyIntent taken;
    taken.monitorId = key.first;
    taken.targetVolume = intent.target;
    taken.generation = key.second;
    taken.revision = intent.revision;
    taken.authorizationEpoch = intent.authorizationEpoch;
    ready_.push_back(taken);
}

void KeyIntentRouter::complete(const Key& key, uint64_t revision, uint64_t epoch, bool accepted)
{
    inFlight_.erase(key);
    const auto found = intents_.find(key);
    if (found == intents_.end()) {
        return;
    }
    Intent& intent = found->second;
    if (intent.authorizationEpoch != epoch) {
        // Superseded by a manual change: send the newest target if one is waiting.
        if (enabled_ && !stopped_ && intent.scheduled) {
            flush(key);
        }
        return;
    }
    if (!accepted && intent.revision == revision) {
        // Rejected with nothing newer: drop the target, so the next press starts from the readback.
        intents_.erase(found);
        return;
    }
    if (intent.revision != revision) {
        intent.scheduled = true;
        flush(key);
    } else {
        intent.scheduled = false;
    }
}

// Runs before every observation and key event. Rebases a pending intent to the readback when the
// readback moved away from the last one seen and no write of ours is outstanding or settling.
void KeyIntentRouter::reconcile()
{
    const uint64_t now = nowMs();
    pruneRebaseState(now);

    for (auto it = muteMemory_.begin(); it != muteMemory_.end();) {
        const VolumeObservation* display = displayFor(it->first);
        const bool eligible = display != nullptr && display->supported && display->awake && !display->failed &&
                              std::isfinite(display->percent);
        if (eligible) {
            ++it;
        } else {
            it = muteMemory_.erase(it);
        }
    }

    std::vector<Key> keys;
    keys.reserve(intents_.size());
    for (const auto& entry : intents_) {
        keys.push_back(entry.first);
    }
    for (const Key& key : keys) {
        const VolumeObservation* display = displayFor(key);
        if (display == nullptr || !display->supported || !display->awake || display->failed ||
            !std::isfinite(display->percent)) {
            intents_.erase(key);
            forgetRebaseState(key);
            continue;
        }
        auto found = intents_.find(key);
        Intent& intent = found->second;
        if (intent.scheduled || display->pending) {
            continue;
        }
        // While our own write is outstanding or settling, the readback is not an external change.
        const auto outstanding = outstanding_.find(key);
        const bool outstandingWrites = outstanding != outstanding_.end() && !outstanding->second.empty();
        const auto blocked = rebaseBlockedUntil_.find(key);
        const uint64_t blockedUntil = blocked == rebaseBlockedUntil_.end() ? uint64_t{0} : blocked->second;
        if (outstandingWrites || now < blockedUntil) {
            continue;
        }
        const double observed = display->percent;
        if (intent.lastObserved && observed != *intent.lastObserved &&
            std::fabs(intent.target - observed) > kKeyReadbackTolerance) {
            ++intentRevision_;
            Intent rebased;
            rebased.target = observed;
            rebased.revision = intentRevision_;
            rebased.authorizationEpoch = intentRevision_;
            rebased.lastObserved = observed;
            intent = rebased;
        } else {
            intent.lastObserved = observed;
        }
    }
}

// Drops acceptances older than kKeyOutstandingExpiryMs, settle windows that have ended,
// and rebase state of monitors that are no longer observed.
void KeyIntentRouter::pruneRebaseState(uint64_t now)
{
    for (auto it = outstanding_.begin(); it != outstanding_.end();) {
        auto& actions = it->second;
        for (auto action = actions.begin(); action != actions.end();) {
            if (now <= action->second + kKeyOutstandingExpiryMs) {
                ++action;
            } else {
                action = actions.erase(action);
            }
        }
        if (actions.empty()) {
            it = outstanding_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = rebaseBlockedUntil_.begin(); it != rebaseBlockedUntil_.end();) {
        if (it->second > now) {
            ++it;
        } else {
            it = rebaseBlockedUntil_.erase(it);
        }
    }
    std::set<Key> keys;
    for (const auto& entry : outstanding_) {
        keys.insert(entry.first);
    }
    for (const auto& entry : rebaseBlockedUntil_) {
        keys.insert(entry.first);
    }
    for (const auto& entry : earlyCompleted_) {
        keys.insert(entry.first);
    }
    for (const Key& key : keys) {
        if (displayFor(key) == nullptr) {
            forgetRebaseState(key);
        }
    }
}

void KeyIntentRouter::forgetRebaseState(const Key& key)
{
    outstanding_.erase(key);
    rebaseBlockedUntil_.erase(key);
    earlyCompleted_.erase(key);
}

// Records an acceptance. At the bound the oldest acceptance is dropped: first for this key, then overall.
void KeyIntentRouter::registerOutstanding(const Key& key, uint64_t actionGeneration, uint64_t now)
{
    const auto perKey = outstanding_.find(key);
    if (perKey != outstanding_.end() && perKey->second.size() >= kKeyMaxOutstanding) {
        dropOldestOutstanding(&key);
    }
    std::size_t total = 0;
    for (const auto& entry : outstanding_) {
        total += entry.second.size();
    }
    if (total >= kKeyMaxOutstanding) {
        dropOldestOutstanding(nullptr);
    }
    outstanding_[key][actionGeneration] = now;
}

void KeyIntentRouter::dropOldestOutstanding(const Key* only)
{
    bool found = false;
    Key oldestKey;
    uint64_t oldestAction = 0;
    uint64_t oldestAt = 0;
    for (const auto& entry : outstanding_) {
        if (only != nullptr && *only != entry.first) {
            continue;
        }
        for (const auto& action : entry.second) {
            if (!found || action.second < oldestAt) {
                found = true;
                oldestKey = entry.first;
                oldestAction = action.first;
                oldestAt = action.second;
            }
        }
    }
    if (!found) {
        return;
    }
    auto it = outstanding_.find(oldestKey);
    it->second.erase(oldestAction);
    if (it->second.empty()) {
        outstanding_.erase(it);
    }
}

// Drops every pending target and latch. In-flight keys are kept: their intentFinished still arrives.
void KeyIntentRouter::clearPending()
{
    for (auto& latch : latches_) {
        latch.reset();
    }
    intents_.clear();
    outstanding_.clear();
    rebaseBlockedUntil_.clear();
    earlyCompleted_.clear();
    muteMemory_.clear();
}

}  // namespace telaviva
