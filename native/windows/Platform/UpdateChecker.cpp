#include "UpdateChecker.h"

#include <mutex>
#include <utility>

namespace telaviva {
namespace {

constexpr char kEndpoint[] = "https://api.github.com/repos/ksfreitas/telaviva-oled/releases/latest";
constexpr double kFreshSeconds = 6.0 * 3600.0;
constexpr double kRetrySeconds = 30.0;
// Far enough in the past that the first check is never throttled.
constexpr double kNeverAttempted = -1.0e300;

UpdateState failedState()
{
    UpdateState state;
    state.kind = UpdateKind::Failed;
    return state;
}

// Unknown statuses from the core are treated as failures.
UpdateState toState(const UpdateOutcome& outcome)
{
    UpdateState state;
    if (outcome.status == "update_available") {
        state.kind = UpdateKind::UpdateAvailable;
        state.remoteVersion = outcome.remoteVersion;
        state.releaseUrl = outcome.releaseUrl;
    } else if (outcome.status == "up_to_date") {
        state.kind = UpdateKind::UpToDate;
        state.remoteVersion = outcome.remoteVersion;
    } else if (outcome.status == "no_release") {
        state.kind = UpdateKind::NoRelease;
    } else if (outcome.status == "rate_limited") {
        state.kind = UpdateKind::RateLimited;
    } else {
        state.kind = UpdateKind::Failed;
    }
    return state;
}

// An observer that throws must not stop the job, so the exception is dropped.
void notifyChanged(const std::function<void()>& onChanged)
{
    if (!onChanged) {
        return;
    }
    try {
        onChanged();
    } catch (...) {
        // Dropped: the state is already settled.
    }
}

}  // namespace

struct UpdateChecker::Impl {
    std::string installedVersion;
    std::shared_ptr<IHttpGet> http;
    UpdateEvaluator evaluate;
    std::shared_ptr<IClock> clock;

    // Guards every field below. Never held while the observer or the runner is called.
    std::mutex mutex;
    UpdateState state;
    bool inFlight = false;
    double lastAttemptSteady = kNeverAttempted;
    double settledAtSteady = 0.0;
    std::function<void()> onChanged;
};

UpdateChecker::UpdateChecker(std::string installedVersion, std::shared_ptr<IHttpGet> http, UpdateEvaluator evaluate,
                             std::shared_ptr<IClock> clock, JobRunner runner)
    : impl_(std::make_shared<Impl>()), runner_(std::move(runner))
{
    impl_->installedVersion = std::move(installedVersion);
    impl_->http = std::move(http);
    impl_->evaluate = std::move(evaluate);
    impl_->clock = std::move(clock);
}

UpdateState UpdateChecker::state() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}

void UpdateChecker::checkIfNeeded()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->inFlight) {
        return;
    }
    const double age = impl_->clock->steadySeconds() - impl_->settledAtSteady;
    switch (impl_->state.kind) {
    case UpdateKind::UpToDate:
    case UpdateKind::UpdateAvailable:
    case UpdateKind::NoRelease:
        if (age < kFreshSeconds) {
            return;
        }
        break;
    case UpdateKind::RateLimited:
    case UpdateKind::Failed:
        if (age < kRetrySeconds) {
            return;
        }
        break;
    default:
        break;
    }
    start(lock);
}

void UpdateChecker::checkNow()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->inFlight) {
        return;
    }
    if (impl_->clock->steadySeconds() - impl_->lastAttemptSteady < kRetrySeconds) {
        return;
    }
    start(lock);
}

void UpdateChecker::start(std::unique_lock<std::mutex>& lock)
{
    impl_->inFlight = true;
    impl_->lastAttemptSteady = impl_->clock->steadySeconds();
    impl_->state = UpdateState{};
    impl_->state.kind = UpdateKind::Checking;
    std::function<void()> onChanged = impl_->onChanged;
    lock.unlock();

    notifyChanged(onChanged);
    // The job owns the shared state, so it stays valid if this checker is destroyed first.
    std::shared_ptr<Impl> impl = impl_;
    runner_([impl] { runJob(impl); });
}

void UpdateChecker::runJob(const std::shared_ptr<Impl>& impl)
{
    UpdateState next = failedState();
    try {
        const std::string userAgent = "TelaVivaOLED/" + impl->installedVersion;
        const HttpResult result = impl->http->get(kEndpoint, userAgent);
        if (result.transportOk) {
            // The body is never logged.
            const std::optional<UpdateOutcome> outcome =
                impl->evaluate(impl->installedVersion, result.status, result.body);
            if (outcome) {
                next = toState(*outcome);
            }
        }
    } catch (...) {
        next = failedState();
    }

    std::function<void()> onChanged;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->state = next;
        impl->settledAtSteady = impl->clock->steadySeconds();
        impl->inFlight = false;
        onChanged = impl->onChanged;
    }
    notifyChanged(onChanged);
}

void UpdateChecker::setOnChanged(std::function<void()> onChanged)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->onChanged = std::move(onChanged);
}

}  // namespace telaviva
