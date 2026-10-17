#include "ScreensaverMonitor.h"

#include <windows.h>

#include <utility>

namespace telaviva {

const char* screensaverStateString(ScreensaverState state)
{
    switch (state) {
    case ScreensaverState::Unknown:
        return "unknown";
    case ScreensaverState::Inactive:
        return "inactive";
    case ScreensaverState::Active:
        return "active";
    case ScreensaverState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::optional<bool> SystemScreensaverProbe::isRunning()
{
    BOOL running = FALSE;
    if (SystemParametersInfoW(SPI_GETSCREENSAVERRUNNING, 0, &running, 0) == 0) {
        return std::nullopt;
    }
    return running != FALSE;
}

ScreensaverMonitor::ScreensaverMonitor(IScheduler& scheduler, IScreensaverProbe& probe,
                                       std::function<void(ScreensaverState)> onState,
                                       std::chrono::milliseconds pollInterval)
    : scheduler_(scheduler), probe_(probe), onState_(std::move(onState)), pollInterval_(pollInterval)
{
}

ScreensaverMonitor::~ScreensaverMonitor()
{
    TimerId pending = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        destroyed_ = true;
        enabled_ = false;
        pending = timer_;
        timer_ = kInvalidTimerId;
    }
    // Cancel outside the lock: a running tick takes the lock, and cancel may wait for it.
    if (pending != kInvalidTimerId) {
        scheduler_.cancel(pending);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    tickDone_.wait(lock, [this] { return !tickRunning_; });
}

ScreensaverState ScreensaverMonitor::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void ScreensaverMonitor::setEnabled(bool enabled)
{
    if (enabled) {
        enable();
    } else {
        disable();
    }
}

ScreensaverState ScreensaverMonitor::readProbe()
{
    const std::optional<bool> running = probe_.isRunning();
    if (!running.has_value()) {
        return ScreensaverState::Unavailable;
    }
    return *running ? ScreensaverState::Active : ScreensaverState::Inactive;
}

std::optional<ScreensaverState> ScreensaverMonitor::decideLocked(ScreensaverState next)
{
    if (published_ && next == state_) {
        return std::nullopt;
    }
    state_ = next;
    published_ = true;
    return next;
}

void ScreensaverMonitor::armLocked()
{
    ++armGeneration_;
    const uint64_t generation = armGeneration_;
    timer_ = scheduler_.scheduleOnce(pollInterval_, toleranceFor(pollInterval_),
                                     [this, generation] { tick(generation); });
}

void ScreensaverMonitor::enable()
{
    std::lock_guard<std::recursive_mutex> publishing(publishMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_ || destroyed_) {
            return;
        }
    }

    // The read happens before the lock so a slow probe never blocks a tick.
    const ScreensaverState reading = readProbe();
    std::optional<ScreensaverState> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = true;
        changed = decideLocked(reading);
        // A tick still running from an earlier enable re-arms itself; arm only if none is pending.
        if (timer_ == kInvalidTimerId) {
            armLocked();
        }
    }
    if (changed && onState_) {
        onState_(*changed);
    }
}

void ScreensaverMonitor::disable()
{
    TimerId pending = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false;
        ++armGeneration_;
        pending = timer_;
        timer_ = kInvalidTimerId;
    }
    // Outside the publish lock: a tick waiting for it must be able to finish first.
    if (pending != kInvalidTimerId) {
        scheduler_.cancel(pending);
    }

    std::lock_guard<std::recursive_mutex> publishing(publishMutex_);
    bool wasPublished = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasPublished = published_;
        published_ = false;
        state_ = ScreensaverState::Unknown;
    }
    if (wasPublished && onState_) {
        onState_(ScreensaverState::Unknown);
    }
}

void ScreensaverMonitor::tick(uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (destroyed_ || !enabled_ || generation != armGeneration_) {
            return;
        }
        timer_ = kInvalidTimerId;
        tickRunning_ = true;
    }

    {
        std::lock_guard<std::recursive_mutex> publishing(publishMutex_);
        const ScreensaverState reading = readProbe();
        std::optional<ScreensaverState> changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (enabled_ && !destroyed_) {
                changed = decideLocked(reading);
            }
        }
        if (changed && onState_) {
            onState_(*changed);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ && !destroyed_ && timer_ == kInvalidTimerId) {
        armLocked();
    }
    tickRunning_ = false;
    tickDone_.notify_all();
}

}  // namespace telaviva
