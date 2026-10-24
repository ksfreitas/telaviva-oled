#pragma once
// Port of the macOS AmbientCalibrationController. One session maps a monitor's brightness percent
// to a Laptop panel level. Start: adaptive brightness is saved and turned off before anything else,
// then follow is paused, then the monitor and the panel are moved. End (done, cancel, quit, display
// off, lid closed): panel level, adaptive brightness, monitor, then the pause flag, then the record
// is deleted. A failed adaptive restore keeps the record for the next launch.
//
// Adaptive brightness is taken through the shared AutoBrightnessOwner as the calibration holder. The
// owner keeps its own record (auto-brightness-hold.json). The calibration record (calibration-session.json)
// is the one this controller restores: recoverPending uses it, through CalibrationSession::recover and
// the owner's underlying AdaptiveBrightness, which is passed in for that purpose because the owner does not
// expose it. The two records hold the same saved values; the owner's record is written first, so the owner
// remains the single holder of the setting while calibration runs.
//
// A monitor missing at launch, or whose live restore was refused, keeps its part of the record. onSnapshot
// retries it on every publish and gives up after 90 s, deleting the record and raising MonitorRecoveryGaveUp.
//
// Threading: the UI thread drives begin, end, setSlider, confirm, removePoint and recoverPending. onSnapshot
// runs on the snapshot thread (not under settingsMutex or laptopMutex) and the give-up timer on a pool thread.
// They share only the pending monitor restore, its timer and the give-up notice, guarded by mutex_.
// Includes no windows.h.
#include "AdaptiveBrightness.h"
#include "AutoBrightnessOwner.h"
#include "CalibrationSession.h"
#include "Scheduler.h"
#include "SettingsModel.h"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

// The Laptop panel. The app adapts LaptopPanel or WmiPanel to it. Levels are whole percents.
struct ICalibrationPanel {
    virtual ~ICalibrationPanel() = default;
    // nullopt when the level cannot be read.
    virtual std::optional<double> panelPercent() = 0;
    virtual bool setPanelPercent(double percent) = 0;
};

// Pauses the follower while calibration runs.
struct IFollowPause {
    virtual ~IFollowPause() = default;
    virtual void setCalibrating(bool calibrating) = 0;
    // True while follow is paused or unavailable (lid closed, panel off, display asleep).
    virtual bool paused() = 0;
};

// Monitor brightness readouts and writes. The app maps these to the snapshot and the controls driver.
struct ICalibrationMonitors {
    virtual ~ICalibrationMonitors() = default;
    // nullopt when the monitor is not in the latest snapshot or its brightness is unknown.
    virtual std::optional<double> observedPercent(const std::string& monitorId) = 0;
    // False when the write was refused.
    virtual bool setPercent(const std::string& monitorId, double percent) = 0;
};

// One calibration point. `monitor` is the monitor brightness percent (1..100); `builtin` is the Laptop
// level (0..100) at which the monitor equals `monitor` percent.
struct AmbientCurvePoint {
    double monitor = 0.0;
    double builtin = 0.0;
    bool operator==(const AmbientCurvePoint& other) const = default;
};

// One monitor's curve: points kept ascending by monitor percent.
struct AmbientCurve {
    static constexpr std::size_t kMinPoints = 2;
    static constexpr std::size_t kMaxPoints = 32;

    std::vector<AmbientCurvePoint> points;

    bool isComplete() const;
    std::optional<AmbientCurvePoint> point(int monitor) const;
    // Replaces the point with that monitor percent, or inserts it in sorted order. Inserting does nothing at kMaxPoints.
    void set(int monitor, double builtin);
    void remove(int monitor);
};

using AmbientCurveMap = std::map<std::string, AmbientCurve>;

// Settings seam. The app submits through the core bridge.
struct ICalibrationSettings {
    virtual ~ICalibrationSettings() = default;
    virtual SettingsDraft currentDraft() = 0;
    // Every monitor's curve. update_settings replaces ambient_curves as a whole, so a save carries all of them.
    virtual AmbientCurveMap curves() = 0;
    // Sends an update_settings message. Returns the TV_ status: 0 accepts.
    virtual int submit(const std::string& json) = 0;
};

// The Laptop levels a point may take: between the nearest saved neighbors. Lower is never above upper.
struct SliderRange {
    double lower = 0.0;
    double upper = 100.0;
    bool operator==(const SliderRange& other) const = default;
};

struct ActiveCalibration {
    std::string monitorId;
    int point = 0;
    double sliderValue = 0.0;
    bool operator==(const ActiveCalibration& other) const = default;
};

// Reportable problems shown in the calibration panel. AutoBrightnessUnknown is raised when the adaptive
// setting cannot be read; AutoBrightnessNotChanged when a write is refused or a restore fails.
enum class CalibrationNotice {
    BuiltInUnreadable,
    BuiltInWriteFailed,
    BuiltInUnavailable,
    // Built-in protection holds the screen, so calibration is refused until protection releases it.
    ProtectionActive,
    AutoBrightnessUnknown,
    AutoBrightnessNotChanged,
    RecoveryFailed,
    // A monitor restore waited 90 s for the monitor, which did not return. The record was deleted.
    MonitorRecoveryGaveUp,
};

enum class CalibrationEndReason {
    Done,
    Cancelled,
    LeftTab,
    WindowClosed,
    AppQuit,
    DisplaySleep,
    BuiltInUnavailable,
};

// The update_settings message for the draft with the full curve map. Empty when it cannot be built.
std::string buildCalibrationPayload(const SettingsDraft& draft, const AmbientCurveMap& curves);

// True when every curve fits the core: non-empty id, at most 32 points, monitor 1..100 strictly ascending,
// builtin 0..100 and finite.
bool isValidCurveMap(const AmbientCurveMap& curves);

class AmbientCalibrationController {
public:
    // owner takes adaptive brightness for the calibration holder. adaptive is the owner's underlying
    // AdaptiveBrightness, used by recoverPending. Every argument must outlive the controller.
    AmbientCalibrationController(ICalibrationPanel& panel, ICalibrationSettings& settings, IFollowPause& follow,
                                 ICalibrationMonitors& monitors, AutoBrightnessOwner& owner, AdaptiveBrightness& adaptive,
                                 CalibrationSession& session, IScheduler& scheduler);
    // Cancels the give-up timer, if one is armed.
    ~AmbientCalibrationController();

    AmbientCalibrationController(const AmbientCalibrationController&) = delete;
    AmbientCalibrationController& operator=(const AmbientCalibrationController&) = delete;

    bool isActive() const { return active_.has_value(); }
    const std::optional<ActiveCalibration>& active() const { return active_; }
    // The give-up notice wins over the session notice until the next begin.
    std::optional<CalibrationNotice> notice() const;

    // While it returns true, begin is refused with the ProtectionActive notice. Unset means nothing holds the screen.
    void setProtectionHolds(std::function<bool()> protectionHolds);

    // Starts a session for `point` (1..100), or re-measures it when the point is saved. Ignored, without a notice,
    // when the point is out of range or would be a new point on a full curve.
    void begin(const std::string& monitorId, int point);

    // Range of Laptop levels bounded by the nearest saved neighbors of `point`; 0 and 100 when none exist.
    SliderRange allowedRange(int point, const std::string& monitorId);

    // Moves the Laptop slider to the value, clamped to the allowed range.
    void setSlider(double value);

    // True once the monitor reports the active point's level within 2 points.
    bool anchorReached(const std::string& monitorId);

    // Saves the slider value for the active point (inserted or replacing the saved point with the same monitor
    // percent) and submits the curve. The session stays active. False when refused or rejected.
    bool confirm();

    // Removes a saved point without a session. Refused for the point being calibrated. False when refused or rejected.
    bool removePoint(const std::string& monitorId, int point);

    void end(CalibrationEndReason reason);

    // Restores the record left by a previous run. The panel level and the adaptive values are restored together. A
    // monitor missing from the snapshot keeps its part of the record until onSnapshot restores it or the 90 s give-up.
    // False when it could not be restored: the record is kept and the RecoveryFailed notice is raised.
    bool recoverPending();

    // Completes a pending monitor restore once the monitor reports again. Call on every snapshot publish.
    // Does nothing unless a restore is pending. A refused write keeps the restore for the next call.
    void onSnapshot();

private:
    struct Captured {
        std::string monitorId;
        double panelLevel = 0.0;
        std::optional<double> monitorOriginal;
    };

    struct PendingMonitor {
        std::string monitorId;
        double percent = 0.0;
    };

    AmbientCurve curveOf(const std::string& monitorId);
    // Turns adaptive brightness off and pauses follow. False when nothing was started. replacingPending: the new
    // record overwrites a monitor-only record, so it is written with rewrite rather than begin.
    bool startSession(const std::string& monitorId, bool replacingPending);
    void finish();
    void restoreState(const Captured& captured);
    bool submitCurves(const AmbientCurveMap& curves);
    // Keeps a monitor restore for onSnapshot and arms the give-up timer. Takes mutex_.
    void holdMonitorRestore(const std::string& monitorId, double percent);
    // Timer callback: gives up a pending monitor restore after the timeout. Takes mutex_.
    void giveUpMonitorRecovery();

    ICalibrationPanel& panel_;
    ICalibrationSettings& settings_;
    IFollowPause& follow_;
    ICalibrationMonitors& monitors_;
    AutoBrightnessOwner& owner_;
    AdaptiveBrightness& adaptive_;
    CalibrationSession& session_;
    IScheduler& scheduler_;
    std::function<bool()> protectionHolds_;
    std::optional<Captured> captured_;
    std::optional<ActiveCalibration> active_;
    std::optional<CalibrationNotice> notice_;

    // Guards the fields below. onSnapshot and the give-up timer run off the UI thread.
    mutable std::mutex mutex_;
    std::optional<PendingMonitor> pendingMonitor_;
    // Armed while a give-up is outstanding; one timer covers every pending restore until it fires.
    std::optional<TimerId> monitorTimer_;
    std::optional<CalibrationNotice> monitorNotice_;
};

}  // namespace telaviva
