#pragma once
// Port of the macOS AmbientCalibrationController. One session maps a monitor's brightness percent
// to a Laptop panel level. Start: adaptive brightness is saved and turned off before anything else,
// then follow is paused, then the monitor and the panel are moved. End (done, cancel, quit, display
// off, lid closed): panel level, adaptive brightness, monitor, then the pause flag, then the record
// is deleted. A failed adaptive restore keeps the record for the next launch.
// Single-threaded: call from the UI thread only. Includes no windows.h.
#include "AdaptiveBrightness.h"
#include "CalibrationSession.h"
#include "SettingsModel.h"

#include <cstddef>
#include <map>
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

// Monitor brightness readouts and writes. The app maps these to the snapshot and ManualControls.
struct ICalibrationMonitors {
    virtual ~ICalibrationMonitors() = default;
    // nullopt when the monitor's brightness is unknown.
    virtual std::optional<double> observedPercent(const std::string& monitorId) = 0;
    virtual void setPercent(const std::string& monitorId, double percent) = 0;
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
    AutoBrightnessUnknown,
    AutoBrightnessNotChanged,
    RecoveryFailed,
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
    AmbientCalibrationController(ICalibrationPanel& panel, ICalibrationSettings& settings, IFollowPause& follow,
                                 ICalibrationMonitors& monitors, AdaptiveBrightness& adaptive,
                                 CalibrationSession& session);

    bool isActive() const { return active_.has_value(); }
    const std::optional<ActiveCalibration>& active() const { return active_; }
    std::optional<CalibrationNotice> notice() const { return notice_; }

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

    // Restores the session left by a previous run, through CalibrationSession::recover. False when it could not be
    // restored: the record is kept and the RecoveryFailed notice is raised.
    bool recoverPending();

private:
    struct Captured {
        std::string monitorId;
        double panelLevel = 0.0;
        std::optional<double> monitorOriginal;
        bool adaptiveHeld = false;
        SavedAdaptiveState adaptive;
    };

    AmbientCurve curveOf(const std::string& monitorId);
    // Turns adaptive brightness off and pauses follow. False when nothing was started.
    bool startSession(const std::string& monitorId);
    void finish();
    void restoreState(const Captured& captured);
    bool submitCurves(const AmbientCurveMap& curves);

    ICalibrationPanel& panel_;
    ICalibrationSettings& settings_;
    IFollowPause& follow_;
    ICalibrationMonitors& monitors_;
    AdaptiveBrightness& adaptive_;
    CalibrationSession& session_;
    std::optional<Captured> captured_;
    std::optional<ActiveCalibration> active_;
    std::optional<CalibrationNotice> notice_;
};

}  // namespace telaviva
