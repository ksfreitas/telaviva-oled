#include "ControlsDriver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace telaviva {

namespace {

// Effects that answer an effect (submitted, attempt_done) can produce more effects. Deeper
// chains are dropped, so a misbehaving core cannot recurse without bound.
constexpr int kMaxEffectDepth = 64;

// Timer work decided under the mutex and carried out after it is released.
struct TimerPlan {
    TimerId cancel = kInvalidTimerId;
    bool schedule = false;
    uint64_t epoch = 0;
    std::chrono::milliseconds delay{0};
};

// Whole milliseconds until the absolute deadline, rounded up and never negative.
std::chrono::milliseconds delayUntil(double deadline, double nowSeconds)
{
    const double ms = std::ceil((deadline - nowSeconds) * 1000.0);
    return std::chrono::milliseconds(static_cast<long long>(std::max(0.0, ms)));
}

}  // namespace

ControlsDriver::ControlsDriver(IControlsApi& api, ICoreSubmitter& submitter, IAttemptRunner& runner,
                               IDdcExecutor& executor, IScheduler& scheduler, IClock& clock)
    : api_(api), submitter_(submitter), runner_(runner), executor_(executor), scheduler_(scheduler), clock_(clock)
{
}

ControlsDriver::~ControlsDriver()
{
    shutdown();
}

void ControlsDriver::setDraftObserver(DraftObserver observer)
{
    observer_ = std::move(observer);
}

void ControlsDriver::configure(double brightnessStep, double volumeStep, bool keysEnabled,
                               const std::optional<std::optional<std::string>>& volumeOwner)
{
    dispatch(buildControlsConfigure(now(), brightnessStep, volumeStep, keysEnabled, volumeOwner));
}

void ControlsDriver::observe(const Snapshot& snapshot)
{
    dispatch(buildControlsObserve(now(), snapshot));
}

std::optional<DraftView> ControlsDriver::slider(const std::string& monitorId, const std::string& feature,
                                                double value)
{
    return dispatch(buildControlsSlider(now(), monitorId, feature, value)).draft;
}

bool ControlsDriver::key(const std::string& key, bool down, bool repeat, bool fine,
                         const std::optional<KeyOwnerRef>& owner)
{
    return dispatch(buildControlsKey(now(), key, down, repeat, fine, owner)).consume.value_or(false);
}

DraftView ControlsDriver::queryDraft(const std::string& monitorId, const std::string& feature) const
{
    const ControlsResult result = send(buildControlsQueryDraft(now(), monitorId, feature));
    execute(result, 0);
    return result.draft.value_or(DraftView{});
}

void ControlsDriver::onMonitorProfile(const std::string& monitorId, const std::string& manufacturer)
{
    dispatch(buildControlsMonitorProfile(now(), monitorId, manufacturer));
}

void ControlsDriver::onMonitorRemoved(const std::string& monitorId)
{
    dispatch(buildControlsMonitorRemoved(now(), monitorId));
}

void ControlsDriver::displayPower(bool on)
{
    dispatch(buildControlsDisplayPower(now(), on));
}

void ControlsDriver::shutdown()
{
    TimerId cancelId = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        // The reply is ignored: no effects run once the driver has stopped.
        api_.step(buildControlsShutdown(now()));
        if (armed_) {
            cancelId = timerId_;
        }
        armed_ = false;
        timerId_ = kInvalidTimerId;
        // Any timer still being scheduled is superseded and cancelled by send when it returns.
        ++armEpoch_;
    }
    if (cancelId != kInvalidTimerId) {
        scheduler_.cancel(cancelId);
    }
}

ControlsResult ControlsDriver::dispatch(const std::string& eventJson)
{
    const ControlsResult result = send(eventJson);
    execute(result, 0);
    return result;
}

ControlsResult ControlsDriver::send(const std::string& eventJson) const
{
    TimerPlan plan;
    ControlsResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return result;
        }
        const std::string reply = api_.step(eventJson);
        if (reply.empty()) {
            return result;
        }
        try {
            result = parseControlsResult(reply);
        } catch (const std::invalid_argument&) {
            return ControlsResult{};
        }

        if (result.nextDeadline) {
            const double deadline = *result.nextDeadline;
            if (!armed_ || armedDeadline_ != deadline) {
                // The previous timer is cancelled outside the lock. An id still in flight is
                // not known yet; its callback is then stale and the new arming cancels it.
                plan.cancel = armed_ ? timerId_ : kInvalidTimerId;
                plan.schedule = true;
                plan.epoch = ++armEpoch_;
                plan.delay = delayUntil(deadline, clock_.steadySeconds());
                armed_ = true;
                armedDeadline_ = deadline;
                timerId_ = kInvalidTimerId;
            }
        } else if (armed_) {
            plan.cancel = timerId_;
            armed_ = false;
            timerId_ = kInvalidTimerId;
            ++armEpoch_;
        }
    }

    // The scheduler is never called with the mutex held.
    if (plan.cancel != kInvalidTimerId) {
        scheduler_.cancel(plan.cancel);
    }
    if (plan.schedule) {
        const uint64_t epoch = plan.epoch;
        const TimerId id = scheduler_.scheduleOnce(plan.delay, toleranceFor(plan.delay),
                                                   [this, epoch] { onTimer(epoch); });
        bool superseded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            superseded = epoch != armEpoch_;
            if (!superseded) {
                timerId_ = id;
            }
        }
        if (superseded) {
            scheduler_.cancel(id);
        }
    }
    return result;
}

void ControlsDriver::execute(const ControlsResult& result, int depth) const
{
    if (depth > kMaxEffectDepth) {
        return;
    }
    for (const ControlsEffect& effect : result.effects) {
        if (stoppedNow()) {
            return;
        }
        switch (effect.type) {
        case ControlsEffect::Type::SubmitCommand:
        case ControlsEffect::Type::SubmitResult: {
            // The submitter runs with the mutex released; its status answers the effect.
            const int status = submitter_.submit(effect.json);
            execute(send(buildControlsSubmitted(now(), effect.token, status == 0)), depth + 1);
            break;
        }
        case ControlsEffect::Type::RunAttempt:
            postAttempt(effect, depth);
            break;
        case ControlsEffect::Type::DraftChanged:
            if (observer_) {
                observer_(effect.monitorId, effect.feature);
            }
            break;
        }
    }
}

void ControlsDriver::postAttempt(const ControlsEffect& effect, int depth) const
{
    const uint64_t token = effect.token;
    executor_.post([this, token, monitorId = effect.monitorId, write = effect.write, code = effect.code,
                    value = effect.value, depth] {
        if (stoppedNow()) {
            return;
        }
        const uint32_t raw = static_cast<uint32_t>(std::max(0.0, value.value_or(0.0)));
        const AttemptOutcome outcome = runner_.run(AttemptRequest{monitorId, write, code, raw});
        if (stoppedNow()) {
            return;
        }
        std::string error;
        if (!outcome.ok) {
            error = outcome.error.empty() ? std::string("io") : outcome.error;
        }
        execute(send(buildControlsAttemptDone(now(), token, outcome.ok, error, outcome.value)), depth + 1);
    });
}

void ControlsDriver::onTimer(uint64_t epoch) const
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || epoch != armEpoch_) {
            return;
        }
        // Cleared before the tick runs, so the reply may arm a new timer and this callback is
        // never cancelled from inside itself.
        armed_ = false;
        timerId_ = kInvalidTimerId;
    }
    execute(send(buildControlsTick(now())), 0);
}

double ControlsDriver::now() const
{
    return clock_.steadySeconds();
}

bool ControlsDriver::stoppedNow() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

}  // namespace telaviva
