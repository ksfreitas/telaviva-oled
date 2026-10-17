#pragma once
// Reports the screensaver state by polling SPI_GETSCREENSAVERRUNNING. Windows
// has no documented start or stop event for the screensaver, so the poll runs
// only while the protection setting is enabled. Only changes are published.
// This header does not depend on windows.h.
#include "Scheduler.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace telaviva {

struct IScreensaverProbe {
    virtual ~IScreensaverProbe() = default;
    // std::nullopt when the query fails.
    virtual std::optional<bool> isRunning() = 0;
};

class SystemScreensaverProbe final : public IScreensaverProbe {
public:
    std::optional<bool> isRunning() override;
};

enum class ScreensaverState { Unknown, Inactive, Active, Unavailable };

const char* screensaverStateString(ScreensaverState state);

// onState runs on a scheduler thread while enabled, and on the caller's thread
// from setEnabled. Call setEnabled and the destructor from one thread (the UI
// thread); the callback may call setEnabled but must not destroy the monitor.
class ScreensaverMonitor {
public:
    ScreensaverMonitor(IScheduler& scheduler, IScreensaverProbe& probe,
                       std::function<void(ScreensaverState)> onState,
                       std::chrono::milliseconds pollInterval = std::chrono::milliseconds(2000));
    // Cancels the timer and waits for a running tick, so no callback follows destruction.
    ~ScreensaverMonitor();

    ScreensaverMonitor(const ScreensaverMonitor&) = delete;
    ScreensaverMonitor& operator=(const ScreensaverMonitor&) = delete;

    // Enabling reads the probe at once. Disabling stops polling and publishes
    // Unknown once if a state had been published.
    void setEnabled(bool enabled);
    ScreensaverState state() const;

private:
    void enable();
    void disable();
    void tick(uint64_t generation);
    ScreensaverState readProbe();
    // Returns the state to publish, or nullopt when nothing changed. Caller holds mutex_.
    std::optional<ScreensaverState> decideLocked(ScreensaverState next);
    // Caller holds mutex_.
    void armLocked();

    IScheduler& scheduler_;
    IScreensaverProbe& probe_;
    std::function<void(ScreensaverState)> onState_;
    std::chrono::milliseconds pollInterval_;

    // Keeps published callbacks in decided order. Recursive so a callback may call setEnabled.
    std::recursive_mutex publishMutex_;
    mutable std::mutex mutex_;
    std::condition_variable tickDone_;
    bool enabled_ = false;
    bool destroyed_ = false;
    bool tickRunning_ = false;
    TimerId timer_ = kInvalidTimerId;
    // Bumped on every arm and disable, so a timer that raced a cancel is ignored.
    uint64_t armGeneration_ = 0;
    ScreensaverState state_ = ScreensaverState::Unknown;
    bool published_ = false;
};

}  // namespace telaviva
