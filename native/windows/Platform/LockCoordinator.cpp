#include "LockCoordinator.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {

struct LockCoordinator::Impl {
    // Holds the mutex for one step. Timer ids cancelled during the step are cancelled
    // after the mutex is released, because cancel may wait for a running callback that
    // needs the mutex. The locker, submit and log are never called under the mutex.
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

    Impl(ISessionLocker& lockerRef, IScheduler& schedulerRef, SubmitResult submitRef, LogLine logRef,
         std::chrono::milliseconds window)
        : locker(lockerRef), scheduler(schedulerRef), submit(std::move(submitRef)), log(std::move(logRef)),
          confirmWindow(window)
    {
    }

    ISessionLocker& locker;
    IScheduler& scheduler;
    // Set at construction and read without the mutex.
    SubmitResult submit;
    LogLine log;
    std::chrono::milliseconds confirmWindow;
    // Timer callbacks lock this to reach the state without keeping it alive.
    std::weak_ptr<Impl> self;

    std::mutex mutex;
    // Filled under the mutex by cancelTimer. Only Guard drains it.
    std::vector<TimerId> toCancel;
    TimerId timerId = kInvalidTimerId;
    // Bumped on every cancel, so a timer that fires after its cancel does nothing.
    uint64_t timerSeq = 0;

    std::optional<uint64_t> lastHandledToken;
    std::optional<uint64_t> pendingToken;
    bool stopped = false;

    // Caller holds the mutex.
    void cancelTimer()
    {
        ++timerSeq;
        if (timerId != kInvalidTimerId) {
            toCancel.push_back(timerId);
        }
        timerId = kInvalidTimerId;
    }

    // Caller holds the mutex. Returns false when the confirm timer could not be armed.
    bool scheduleConfirmTimer(uint64_t token)
    {
        cancelTimer();
        const uint64_t seq = timerSeq;
        std::weak_ptr<Impl> owner = self;
        timerId = scheduler.scheduleOnce(confirmWindow, toleranceFor(confirmWindow), [owner, seq, token] {
            if (std::shared_ptr<Impl> alive = owner.lock()) {
                alive->onConfirmTimeout(seq, token);
            }
        });
        return timerId != kInvalidTimerId;
    }

    // Timer callbacks run on scheduler threads and take the mutex themselves. The timer that
    // fired has no id left to cancel, so timerId is cleared before the request is cleared.
    void onConfirmTimeout(uint64_t seq, uint64_t token)
    {
        {
            Guard guard(*this);
            if (stopped || seq != timerSeq) {
                return;
            }
            timerId = kInvalidTimerId;
            if (!pendingToken || *pendingToken != token) {
                return;
            }
            pendingToken.reset();
        }
        log("lock failed: not confirmed");
        submit(token, false, "not_confirmed");
    }
};

LockCoordinator::LockCoordinator(ISessionLocker& locker, IScheduler& scheduler, SubmitResult submit, LogLine log,
                                 std::chrono::milliseconds confirmWindow)
    : impl_(std::make_shared<Impl>(locker, scheduler, std::move(submit), std::move(log), confirmWindow))
{
    impl_->self = impl_;
}

LockCoordinator::~LockCoordinator()
{
    shutdown();
}

void LockCoordinator::observe(bool requested, std::optional<uint64_t> token)
{
    if (!requested || !token) {
        return;
    }
    const uint64_t value = *token;
    {
        Impl::Guard guard(*impl_);
        if (impl_->stopped || (impl_->lastHandledToken && *impl_->lastHandledToken == value)) {
            return;
        }
        impl_->lastHandledToken = value;
        // A newer token supersedes a pending one, so the older timer is cancelled.
        impl_->cancelTimer();
        impl_->pendingToken = value;
    }

    // The locker runs outside the mutex, because a lock event may arrive during the call.
    const bool started = impl_->locker.lockNow();

    {
        Impl::Guard guard(*impl_);
        // Shutdown cleared the request, or a lock event already confirmed it.
        if (!impl_->pendingToken || *impl_->pendingToken != value) {
            return;
        }
        // Wait for a lock event or the timer.
        if (started && impl_->scheduleConfirmTimer(value)) {
            return;
        }
        impl_->cancelTimer();
        impl_->pendingToken.reset();
    }

    if (!started) {
        impl_->log("lock failed: unavailable");
        impl_->submit(value, false, "unavailable");
    } else {
        impl_->log("lock failed: not confirmed");
        impl_->submit(value, false, "not_confirmed");
    }
}

void LockCoordinator::sessionLockChanged(bool locked)
{
    if (!locked) {
        return;
    }
    uint64_t value = 0;
    {
        Impl::Guard guard(*impl_);
        if (impl_->stopped || !impl_->pendingToken) {
            return;
        }
        value = *impl_->pendingToken;
        impl_->pendingToken.reset();
        impl_->cancelTimer();
    }
    impl_->log("lock confirmed");
    impl_->submit(value, true, "");
}

void LockCoordinator::shutdown()
{
    Impl::Guard guard(*impl_);
    impl_->stopped = true;
    impl_->pendingToken.reset();
    impl_->cancelTimer();
}

}  // namespace telaviva
