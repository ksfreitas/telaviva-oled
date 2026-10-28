#pragma once
// What the Settings pages need from the app: the current settings and snapshot, the
// core submit path, the camera list and launch at login. AppController implements it.
#include "Platform/AmbientCalibrationController.h"
#include "Platform/CameraSelection.h"
#include "Platform/DeviceSelection.h"
#include "Platform/JsonCodec.h"
#include "Platform/LaunchAtLogin.h"
#include "Platform/UpdateChecker.h"

#include <optional>
#include <string>
#include <vector>

namespace telaviva::host {

// The Keyboard page status line. Active means the hook is installed; Unavailable means keys are
// enabled but the hook is not installed (or its last install failed).
enum class KeysState { Off, Active, Unavailable };

// The Laptop status line. Safe from any thread; written by publish() from the snapshot.
struct LaptopStatus {
    bool available = false;
    std::string reason;  // laptopReasonString; the wire value, empty when none
    bool following = false;  // the core reports the ambient follow as active
    bool calibrating = false;
    std::optional<double> panelPercent;  // the core's builtin_percent
};

// One saved calibration point of a monitor. monitor is the monitor percent, builtin the Laptop level.
struct CurvePointView {
    int monitor = 0;
    double builtin = 0.0;
};

// The calibration panel. notice is a stable key: builtInUnreadable, builtInWriteFailed, builtInUnavailable,
// autoBrightnessUnknown, autoBrightnessNotChanged or recoveryFailed.
struct CalibrationView {
    bool active = false;
    std::string monitorId;
    int point = 0;
    double sliderValue = 0.0;
    double lower = 0.0;
    double upper = 100.0;
    bool anchorReached = false;
    std::optional<std::string> notice;
};

struct ISettingsHost {
    virtual ~ISettingsHost() = default;

    virtual telaviva::SettingsSnapshot currentSettings() = 0;
    // Volume-key routing state. Safe from any thread.
    virtual KeysState keysState() = 0;
    // Copy of the last accepted core snapshot. Safe from any thread.
    virtual telaviva::Snapshot latestSnapshot() = 0;
    // Cached camera list, replaced by refreshCameras(). Safe from any thread.
    virtual std::vector<telaviva::CameraInfo> cameras() = 0;
    // Enumerates cameras and replaces the cached list. Blocks on WinRT calls: never call it on the UI thread.
    virtual void refreshCameras() = 0;
    // Sends an update_settings command; returns the TV_ status code of the core call.
    virtual int submit(const std::string& json) = 0;
    virtual telaviva::LaunchResult setLaunchAtLogin(bool enable) = 0;
    virtual bool launchAtLoginEnabled() = 0;

    // Laptop and calibration. laptopStatus and curvePoints are safe from any thread; the rest
    // run on the UI thread only.
    virtual LaptopStatus laptopStatus() = 0;
    virtual std::vector<CurvePointView> curvePoints(const std::string& monitorId) = 0;
    // No-op while the Laptop cannot be calibrated.
    virtual void beginCalibration(const std::string& monitorId, int point) = 0;
    virtual CalibrationView calibrationView() = 0;
    virtual void setCalibrationSlider(double value) = 0;
    virtual bool confirmCalibration() = 0;
    virtual bool removeCalibrationPoint(const std::string& monitorId, int point) = 0;
    virtual void endCalibration(telaviva::CalibrationEndReason reason) = 0;

    // Update check for the About page. Its state is guarded internally; the About page is the only observer.
    virtual telaviva::UpdateChecker& updateChecker() = 0;
    // The installed version from the core. Safe from any thread.
    virtual const std::string& appVersion() const = 0;

    // Inference device, for the Advanced page. Defaults keep existing implementations compiling.
    // inferenceDevices builds the option list from the live adapter catalog, without creating a device.
    virtual std::vector<DeviceOption> inferenceDevices() { return {}; }
    // What the inference host is using or was refused, from InferenceHost::report().
    virtual InferenceReport inferenceReport() { return {}; }

    // Keep awake with presence status name: disabled, awaiting, active, policy, unavailable or suspended.
    // Defaults keep existing implementations compiling. Safe from any thread.
    virtual std::string keepAwakeStatus() { return "disabled"; }
};

}  // namespace telaviva::host
