#include "KeepAwakeController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace telaviva {

namespace {

// Lease cap from the contract: sampling interval plus 15 s, never more than 315 s.
constexpr double kMaxLeaseSeconds = 315.0;
// Activity pulses run once per second while a lease is held.
constexpr std::chrono::milliseconds kPulseInterval{1000};
// Same leeway as the macOS expiry timer, so release lands close to the deadline.
constexpr std::chrono::milliseconds kExpiryTolerance{10};

// The core's observation_id is the capture timestamp. Tokens order by generation first.
struct Token {
    uint64_t generation;
    uint64_t observationId;
};

bool operator==(const Token& a, const Token& b)
{
    return a.generation == b.generation && a.observationId == b.observationId;
}

bool operator<(const Token& a, const Token& b)
{
    return a.generation == b.generation ? a.observationId < b.observationId : a.generation < b.generation;
}

}  // namespace

KeepAwakeSafety resolveKeepAwakeSafety(bool sessionActive, std::optional<bool> sessionLocked,
                                       std::optional<bool> screensaverActive, bool displayAsleep,
                                       bool systemSleeping)
{
    if (systemSleeping) {
        return KeepAwakeSafety::SystemSleeping;
    }
    if (!sessionActive || (sessionLocked && *sessionLocked)) {
        return KeepAwakeSafety::SessionInactive;
    }
    if (!sessionLocked) {
        return KeepAwakeSafety::SessionStateUnknown;
    }
    if (displayAsleep) {
        return KeepAwakeSafety::DisplayAsleep;
    }
    if (screensaverActive && *screensaverActive) {
        return KeepAwakeSafety::ScreensaverActive;
    }
    if (!screensaverActive) {
        return KeepAwakeSafety::ScreensaverStateUnknown;
    }
    return KeepAwakeSafety::Eligible;
}

bool keepAwakeCameraMonitoringAllowed(KeepAwakeSafety safety)
{
    switch (safety) {
    case KeepAwakeSafety::Eligible:
    case KeepAwakeSafety::ScreensaverActive:
    case KeepAwakeSafety::ScreensaverStateUnknown:
    case KeepAwakeSafety::DisplayAsleep:
    case KeepAwakeSafety::LidClosed:
        return true;
    case KeepAwakeSafety::SessionInactive:
    case KeepAwakeSafety::SessionStateUnknown:
    case KeepAwakeSafety::SystemSleeping:
        return false;
    }
    return false;
}

std::optional<bool> resolveKeepAwakeGuard(std::optional<bool> notified,
                                          const std::function<std::optional<bool>()>& live)
{
    if (notified && *notified) {
        return true;
    }
    return live();
}

const char* keepAwakeStatusName(KeepAwakeStatus status)
{
    switch (status) {
    case KeepAwakeStatus::Disabled:
        return "disabled";
    case KeepAwakeStatus::AwaitingPresence:
        return "awaiting";
    case KeepAwakeStatus::Active:
        return "active";
    case KeepAwakeStatus::PolicyBlocked:
        return "policy";
    case KeepAwakeStatus::Unavailable:
        return "unavailable";
    case KeepAwakeStatus::Suspended:
        return "suspended";
    }
    return "disabled";
}

struct KeepAwakeController::Impl {
    // Holds the mutex for one public call. Timer ids cancelled during the call are
    // cancelled here after the mutex is released, because cancel may wait for a
    // running callback that needs the mutex.
    class Guard {
    public:
        explicit Guard(Impl& owner) : impl_(owner), lock_(owner.mutex) {}

        ~Guard()
        {
            std::vector<TimerId> ids;
            ids.swap(impl_.toCancel);
            lock_.unlock();
            for (TimerId id : ids) {
                impl_.scheduler.cancel(id);
            }
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Impl& impl_;
        std::unique_lock<std::mutex> lock_;
    };

    Impl(IKeepAwakeBackend& backendRef, IScheduler& schedulerRef, const IClock& clockRef)
        : backend(backendRef), scheduler(schedulerRef), clock(clockRef)
    {
    }

    IKeepAwakeBackend& backend;
    IScheduler& scheduler;
    const IClock& clock;
    // Timer callbacks lock this to reach the state without keeping it alive.
    std::weak_ptr<Impl> self;

    std::mutex mutex;
    // Filled under the mutex by cancel helpers. Only Guard drains it; timer callbacks clear it.
    std::vector<TimerId> toCancel;
    TimerId pulseId = kInvalidTimerId;
    // Bumped on every cancel, so a timer that fires after its cancel does nothing.
    uint64_t pulseSeq = 0;
    TimerId expiryId = kInvalidTimerId;
    uint64_t expirySeq = 0;

    bool enabled = false;
    bool sessionEligible = false;
    std::optional<Token> latestToken;
    std::optional<Token> revokedThrough;
    std::optional<uint64_t> requiredGeneration;
    uint64_t highestObservedGeneration = 0;
    bool sessionWasInactive = true;
    bool safetyVetoActive = false;
    bool hasReportedSessionState = false;
    std::optional<uint64_t> sessionVetoGeneration;
    bool wasEnabledAtVeto = false;
    // Core observation_id is the captured_at_ms timestamp for accepted positive observations.
    std::optional<uint64_t> requiredObservationAfterMilliseconds;
    uint64_t highestObservationTimestampMilliseconds = 0;
    std::optional<double> deadline;
    bool assertionsHeld = false;
    bool stopped = false;
    KeepAwakeStatus status = KeepAwakeStatus::Disabled;

    double now() const { return clock.steadySeconds(); }

    // Remaining time, rounded up so a timer never fires before the deadline.
    std::chrono::milliseconds delayUntil(double due) const
    {
        const double remainingMs = std::max(0.0, due - now()) * 1000.0;
        return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(std::ceil(remainingMs)));
    }

    void cancelPulse()
    {
        ++pulseSeq;
        if (pulseId != kInvalidTimerId) {
            toCancel.push_back(pulseId);
        }
        pulseId = kInvalidTimerId;
    }

    void cancelExpiry()
    {
        ++expirySeq;
        if (expiryId != kInvalidTimerId) {
            toCancel.push_back(expiryId);
        }
        expiryId = kInvalidTimerId;
    }

    void cancelWorkers()
    {
        cancelPulse();
        cancelExpiry();
    }

    // Returns false when the pulse chain could not be armed. The caller must then end the lease.
    bool schedulePulse()
    {
        cancelPulse();
        const uint64_t seq = pulseSeq;
        std::weak_ptr<Impl> owner = self;
        pulseId = scheduler.scheduleOnce(kPulseInterval, toleranceFor(kPulseInterval), [owner, seq] {
            if (std::shared_ptr<Impl> alive = owner.lock()) {
                alive->onPulse(seq);
            }
        });
        return pulseId != kInvalidTimerId;
    }

    // Returns false when no expiry watchdog could be armed. The lease is then revoked here,
    // so it is never held without an expiry timer.
    bool scheduleExpiry(double due)
    {
        cancelExpiry();
        const uint64_t seq = expirySeq;
        const std::chrono::milliseconds delay = delayUntil(due);
        std::weak_ptr<Impl> owner = self;
        expiryId = scheduler.scheduleOnce(delay, kExpiryTolerance, [owner, seq] {
            if (std::shared_ptr<Impl> alive = owner.lock()) {
                alive->onExpiry(seq);
            }
        });
        if (expiryId != kInvalidTimerId) {
            return true;
        }
        revokeCurrentToken();
        revokeLease();
        publish(KeepAwakeStatus::Unavailable);
        return false;
    }

    // Timer callbacks run on scheduler threads and take the lock themselves. They never
    // call scheduler.cancel. A timer that lost a race with a cancel finds a stale seq.
    void onPulse(uint64_t seq)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!stopped && seq == pulseSeq) {
            // This timer has fired, so there is no id left to cancel.
            pulseId = kInvalidTimerId;
            pulse();
        }
        toCancel.clear();
    }

    void onExpiry(uint64_t seq)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!stopped && seq == expirySeq) {
            expiryId = kInvalidTimerId;
            expireIfDue();
        }
        toCancel.clear();
    }

    void expireIfDue()
    {
        if (!latestToken || !deadline) {
            return;
        }
        if (*deadline > now()) {
            scheduleExpiry(*deadline);
            return;
        }
        revokeCurrentToken();
        revokeLease();
        publish(enabled ? KeepAwakeStatus::AwaitingPresence : KeepAwakeStatus::Disabled);
    }

    void publish(KeepAwakeStatus next)
    {
        status = next;
    }

    // Revoking the current token keeps it from returning. The floor only rises.
    void revokeCurrentToken()
    {
        if (!latestToken) {
            return;
        }
        if (!revokedThrough || *revokedThrough < *latestToken) {
            revokedThrough = latestToken;
        }
    }

    void requireNextGeneration()
    {
        const uint64_t observed =
            std::max(highestObservedGeneration, latestToken ? latestToken->generation : uint64_t{0});
        if (observed == std::numeric_limits<uint64_t>::max()) {
            return;
        }
        requiredGeneration = std::max(requiredGeneration.value_or(0), observed + 1);
    }

    void requireGenerationAfterResume(uint64_t baseline)
    {
        const uint64_t observed =
            std::max(baseline, latestToken ? latestToken->generation : uint64_t{0});
        const uint64_t limit = std::numeric_limits<uint64_t>::max();
        if (observed >= limit - 1) {
            return;
        }
        const uint64_t increment = wasEnabledAtVeto ? uint64_t{2} : uint64_t{1};
        requiredGeneration = std::max(requiredGeneration.value_or(0), observed + increment);
    }

    // Leaving a veto requires evidence captured after the transition, not just a newer generation.
    void leaveSafetyVeto()
    {
        if (!safetyVetoActive) {
            return;
        }
        const uint64_t wallNow = clock.epochMillis();
        requiredObservationAfterMilliseconds = std::max(
            std::max(requiredObservationAfterMilliseconds.value_or(0), wallNow),
            highestObservationTimestampMilliseconds);
        safetyVetoActive = false;
    }

    // Releases the request only if this controller holds it.
    void releaseHeld()
    {
        if (assertionsHeld) {
            assertionsHeld = false;
            backend.releaseRequest();
        }
    }

    void revokeLease()
    {
        cancelWorkers();
        deadline.reset();
        latestToken.reset();
        releaseHeld();
    }

    void stopEffects()
    {
        cancelPulse();
        releaseHeld();
    }

    void setSessionEligible(bool eligible)
    {
        if (stopped || sessionEligible == eligible) {
            return;
        }
        sessionEligible = eligible;
        if (!eligible) {
            wasEnabledAtVeto = enabled;
            sessionVetoGeneration = highestObservedGeneration;
            if (enabled) {
                requireNextGeneration();
            } else {
                requiredGeneration.reset();
            }
            sessionWasInactive = true;
            safetyVetoActive = true;
            hasReportedSessionState = true;
            revokeCurrentToken();
            revokeLease();
        } else if (sessionWasInactive) {
            if (enabled) {
                if (hasReportedSessionState) {
                    requireGenerationAfterResume(sessionVetoGeneration.value_or(highestObservedGeneration));
                } else {
                    requireNextGeneration();
                }
            } else {
                requiredGeneration.reset();
            }
            sessionVetoGeneration.reset();
            wasEnabledAtVeto = false;
            sessionWasInactive = false;
            hasReportedSessionState = true;
        }
        reconcile(false);
    }

    void update(bool nextEnabled, bool requested, uint64_t generation, uint64_t observationId,
                uint64_t validForMilliseconds)
    {
        if (stopped) {
            return;
        }
        enabled = nextEnabled;
        highestObservedGeneration = std::max(highestObservedGeneration, generation);
        highestObservationTimestampMilliseconds = std::max(highestObservationTimestampMilliseconds, observationId);
        const Token token{generation, observationId};
        if (!enabled || !requested || validForMilliseconds == 0) {
            revokeCurrentToken();
            revokeLease();
            reconcile(false);
            return;
        }

        const Token revokedFloor = revokedThrough.value_or(Token{0, 0});
        if (!sessionEligible || token.generation < highestObservedGeneration ||
            (requiredGeneration && token.generation < *requiredGeneration) ||
            (requiredObservationAfterMilliseconds && token.observationId <= *requiredObservationAfterMilliseconds) ||
            !(revokedFloor < token)) {
            reconcile(false);
            return;
        }

        const double leaseSeconds = std::min(static_cast<double>(validForMilliseconds) / 1000.0, kMaxLeaseSeconds);
        if (latestToken) {
            if (token < *latestToken) {
                reconcile(false);
                return;
            }
            if (token == *latestToken) {
                // The same token may only shorten its deadline, never extend it.
                if (!deadline) {
                    reconcile(false);
                    return;
                }
                const double shortened = std::min(*deadline, now() + leaseSeconds);
                deadline = shortened;
                if (!scheduleExpiry(shortened)) {
                    return;
                }
                reconcile(false);
                return;
            }
        }
        latestToken = token;
        deadline = now() + leaseSeconds;
        if (!scheduleExpiry(*deadline)) {
            return;
        }
        reconcile(true);
    }

    void safetyDidChange()
    {
        if (stopped) {
            return;
        }
        reconcile(false);
    }

    void shutdown()
    {
        if (stopped) {
            return;
        }
        stopped = true;
        cancelWorkers();
        revokeLease();
        publish(KeepAwakeStatus::Disabled);
    }

    // Caller holds the mutex. postImmediately forces an activity pulse at once.
    void reconcile(bool postImmediately)
    {
        if (!enabled) {
            revokeLease();
            publish(KeepAwakeStatus::Disabled);
            return;
        }
        if (!sessionEligible || backend.safetyState() != KeepAwakeSafety::Eligible) {
            safetyVetoActive = true;
            revokeCurrentToken();
            revokeLease();
            publish(KeepAwakeStatus::Suspended);
            return;
        }
        leaveSafetyVeto();
        if (!deadline || !(*deadline > now()) || !latestToken) {
            if (deadline) {
                revokeCurrentToken();
                revokeLease();
            }
            publish(KeepAwakeStatus::AwaitingPresence);
            return;
        }
        if (!backend.postingAllowed()) {
            stopEffects();
            publish(KeepAwakeStatus::PolicyBlocked);
            return;
        }
        if (postImmediately || !assertionsHeld) {
            pulse();
        }
    }

    // Caller holds the mutex. Re-checks every gate right before each OS effect.
    void pulse()
    {
        if (stopped) {
            return;
        }
        if (!enabled) {
            revokeLease();
            publish(KeepAwakeStatus::Disabled);
            return;
        }
        if (!sessionEligible || backend.safetyState() != KeepAwakeSafety::Eligible) {
            safetyVetoActive = true;
            revokeCurrentToken();
            revokeLease();
            publish(KeepAwakeStatus::Suspended);
            return;
        }
        leaveSafetyVeto();
        if (!deadline || !(*deadline > now()) || !latestToken) {
            if (deadline) {
                revokeCurrentToken();
            }
            revokeLease();
            publish(KeepAwakeStatus::AwaitingPresence);
            return;
        }
        if (!backend.postingAllowed()) {
            stopEffects();
            publish(KeepAwakeStatus::PolicyBlocked);
            return;
        }
        if (!assertionsHeld) {
            if (!backend.acquireRequest()) {
                stopEffects();
                publish(KeepAwakeStatus::Unavailable);
                return;
            }
            assertionsHeld = true;
        }
        if (backend.safetyState() != KeepAwakeSafety::Eligible || !deadline || !(*deadline > now())) {
            safetyVetoActive = true;
            revokeCurrentToken();
            revokeLease();
            publish(KeepAwakeStatus::Suspended);
            return;
        }
        if (!backend.postActivity()) {
            stopEffects();
            publish(KeepAwakeStatus::Unavailable);
            return;
        }
        if (!schedulePulse()) {
            // Without the pulse chain no further activity can be posted, so end the lease.
            revokeCurrentToken();
            revokeLease();
            publish(KeepAwakeStatus::Unavailable);
            return;
        }
        publish(KeepAwakeStatus::Active);
    }
};

KeepAwakeController::KeepAwakeController(IKeepAwakeBackend& backend, IScheduler& scheduler, const IClock& clock)
    : impl_(std::make_shared<Impl>(backend, scheduler, clock))
{
    impl_->self = impl_;
}

KeepAwakeController::~KeepAwakeController()
{
    shutdown();
}

void KeepAwakeController::setSessionEligible(bool eligible)
{
    Impl::Guard guard(*impl_);
    impl_->setSessionEligible(eligible);
}

void KeepAwakeController::update(bool enabled, bool requested, uint64_t generation, uint64_t observationId,
                                 uint64_t validForMilliseconds)
{
    Impl::Guard guard(*impl_);
    impl_->update(enabled, requested, generation, observationId, validForMilliseconds);
}

void KeepAwakeController::safetyDidChange()
{
    Impl::Guard guard(*impl_);
    impl_->safetyDidChange();
}

void KeepAwakeController::shutdown()
{
    Impl::Guard guard(*impl_);
    impl_->shutdown();
}

KeepAwakeStatus KeepAwakeController::status() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status;
}

}  // namespace telaviva
