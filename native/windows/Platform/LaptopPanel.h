#pragma once
// Laptop follow for the built-in panel: the WMI panel adapter and the policy that
// polls it and reports builtin_display to the core. No windows.h here; the WMI
// calls live in LaptopPanel.cpp behind WmiPanel's pimpl.
#include "DdcController.h"
#include "IClock.h"
#include "ILaptopEnvironment.h"
#include "ILaptopPanel.h"
#include "LaptopDetection.h"
#include "Scheduler.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace telaviva {

// Owns the worker thread and the COM state. Defined in LaptopPanel.cpp.
struct WmiPanelWorker;

// ROOT\WMI brightness through WmiMonitorBrightness (read) and
// WmiMonitorBrightnessMethods::WmiSetBrightness (write). All COM work runs on one
// dedicated MTA worker thread that owns the cached IWbemServices connection. A call
// waits at most 2 s and returns nullopt or false on timeout, on failure, or while a
// previous call is still running. Never throws.
class WmiPanel final : public ILaptopPanel {
public:
    WmiPanel();
    // Stops the worker after any running call finishes (WMI calls have their own timeouts).
    ~WmiPanel() override;

    WmiPanel(const WmiPanel&) = delete;
    WmiPanel& operator=(const WmiPanel&) = delete;

    std::optional<double> readPercent() override;
    bool setPercent(double percent) override;

private:
    std::unique_ptr<WmiPanelWorker> impl_;
};

// Pure policy. Polls the panel every second with a 0.5 s leeway, but only while
// follow is enabled, the panel is available (lid open, display on, brightness
// readable) and no calibration runs. Reports builtin_display only on changes.
// Panel reads happen on the scheduler thread; the caller's thread never waits on WMI
// except for the synchronous detectLaptop check, which the app wiring runs on the UI thread.
//
// Lid and display state come from onLidChanged and onDisplayStatusChanged. The
// environment supplies the rest (DisplayConfig, battery, brightness readability).
class LaptopPanelFollower final {
public:
    LaptopPanelFollower(ILaptopPanel& panel, ILaptopEnvironment& environment, ICoreSubmitter& submitter,
                        IScheduler& scheduler, IClock& clock);
    // Cancels the poll and waits for a running read to finish.
    ~LaptopPanelFollower();

    LaptopPanelFollower(const LaptopPanelFollower&) = delete;
    LaptopPanelFollower& operator=(const LaptopPanelFollower&) = delete;

    // Called from every snapshot with the core's ambient_follow_enabled. Re-evaluates
    // only when the value changes (or on the first call).
    void setFollowEnabled(bool enabled);
    void onLidChanged(bool open);
    void onDisplayStatusChanged(bool on);
    void onCalibrationChanged(bool calibrating);

    // True only while follow is enabled and the panel is available. Calibration
    // must be refused otherwise.
    bool canCalibrate() const;

    // Shell-only reason from the last evaluation, for the UI.
    LaptopReason reason() const;

private:
    struct Report {
        bool available = false;
        std::optional<int> brightness;
        bool calibrating = false;

        bool operator==(const Report& other) const
        {
            return available == other.available && brightness == other.brightness && calibrating == other.calibrating;
        }
    };

    // Each of these expects mutex_ held. Scheduler cancels are returned to the caller
    // and run after unlocking, because a running callback may be waiting for mutex_.
    TimerId takeTimerLocked();
    void armLocked(std::chrono::milliseconds delay, std::chrono::milliseconds tolerance);
    TimerId reconcileLocked();
    LaptopAvailability evaluateLocked();
    bool monitoringLocked() const;
    void publishLocked(const Report& report);
    void applyReadLocked(std::optional<double> level);

    void onTick(uint64_t generation);
    void cancelTimer(TimerId id);

    ILaptopPanel& panel_;
    ILaptopEnvironment& environment_;
    ICoreSubmitter& submitter_;
    IScheduler& scheduler_;
    // Reserved for poll timestamps. The 1 s cadence comes from IScheduler.
    IClock& clock_;

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    bool destroyed_ = false;
    int readsInFlight_ = 0;

    bool followEnabled_ = false;
    bool evaluated_ = false;
    bool calibrating_ = false;
    std::optional<bool> lidOpen_;  // nullopt is treated as open by detectLaptop
    bool displayOn_ = true;
    bool available_ = false;
    LaptopReason reason_ = LaptopReason::None;

    std::optional<Report> submitted_;
    std::optional<int> lastLevel_;

    bool armed_ = false;
    TimerId timerId_ = kInvalidTimerId;
    // Bumped on every arm and take, so a tick that raced a change is ignored.
    uint64_t generation_ = 0;
};

}  // namespace telaviva
