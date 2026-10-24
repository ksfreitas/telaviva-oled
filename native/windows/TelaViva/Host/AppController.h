#pragma once
// Wires the core bridge, display catalog, DDC, the controls driver and session events,
// and feeds the flyout. Owned by App on the UI thread.
#include "Host/FlyoutState.h"
#include "Host/SettingsHost.h"

#include <functional>
#include <memory>
#include <string>

namespace telaviva {
class CoreBridge;
}

namespace telaviva::host {

class PlatformWindow;

class AppController final : public IFlyoutActions, public ISettingsHost {
public:
    // onFlyoutState runs on the core's snapshot thread. onOpenSettings and onQuit
    // run on the UI thread when the flyout asks for them.
    AppController(PlatformWindow& window, std::function<void(FlyoutState)> onFlyoutState,
        std::function<void()> onOpenSettings, std::function<void()> onQuit);
    ~AppController() override;

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // Runs the first monitor refresh. Call once, after the sinks are wired.
    void start();

    // Owned by the controller; valid until the controller is destroyed.
    telaviva::CoreBridge* bridge();

    // Idempotent. Drains the core while DDC still runs so pending restores can finish,
    // then stops DDC. Call from the UI thread.
    void shutdown();

    // Called on the UI thread each time the flyout or the Settings window shows or hides. The
    // caller reports only transitions. Drives the Efficiency mode UI exemption.
    void onWindowVisibilityChanged(bool visible);

    // Called on the UI thread when the flyout hides. Writes a protection brightness value that is
    // still waiting for its trailing timer, so the last slider position is not lost.
    void onFlyoutHidden();

    // IFlyoutActions. UI thread only.
    void setBrightness(const std::string& monitorId, double value) override;
    void setVolume(const std::string& monitorId, double value) override;
    void setMonitorProtected(const std::string& monitorId, bool protectedOn) override;
    void setProtectionBrightness(const std::string& monitorId, double value) override;
    void setBuiltinBrightness(double value) override;
    void refresh() override;
    void openSettings() override;
    void quit() override;

    // ISettingsHost. currentSettings, latestSnapshot, cameras and keysState are safe from any thread;
    // refreshCameras blocks and runs off the UI thread; the rest run on the UI thread.
    telaviva::SettingsSnapshot currentSettings() override;
    KeysState keysState() override;
    telaviva::Snapshot latestSnapshot() override;
    std::vector<telaviva::CameraInfo> cameras() override;
    void refreshCameras() override;
    int submit(const std::string& json) override;
    telaviva::LaunchResult setLaunchAtLogin(bool enable) override;
    bool launchAtLoginEnabled() override;

    // ISettingsHost, Laptop and calibration. laptopStatus and curvePoints are safe from any thread;
    // the calibration methods run on the UI thread only.
    LaptopStatus laptopStatus() override;
    std::vector<CurvePointView> curvePoints(const std::string& monitorId) override;
    void beginCalibration(const std::string& monitorId, int point) override;
    CalibrationView calibrationView() override;
    void setCalibrationSlider(double value) override;
    bool confirmCalibration() override;
    bool removeCalibrationPoint(const std::string& monitorId, int point) override;
    void endCalibration(telaviva::CalibrationEndReason reason) override;

    // ISettingsHost, About page.
    telaviva::UpdateChecker& updateChecker() override;
    const std::string& appVersion() const override;

    // The Settings pages reach the controller through this interface.
    ISettingsHost* settingsHost();

    // ISettingsHost, Advanced page. Run on the UI thread only.
    std::vector<DeviceOption> inferenceDevices() override;
    InferenceReport inferenceReport() override;

private:
    struct Services;
    std::unique_ptr<Services> services_;
    // Built in the constructor from the core version. Declared after services_, so it is destroyed
    // first; it shares no state with the other services.
    std::string appVersion_;
    std::shared_ptr<telaviva::UpdateChecker> updateChecker_;
};

}  // namespace telaviva::host
