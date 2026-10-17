#pragma once

#include <string>
#include <vector>

namespace telaviva::host {

enum class PresenceIcon { Detected, Absent, LowLight, Off };

struct SliderState {
    bool available = false;
    double value = 0;  // 0..100, what the slider shows (draft or observed)
    std::wstring reason;  // localized text when unavailable, empty otherwise
    // The last apply was rejected; value then holds the confirmed or observed value.
    bool failed = false;
};

struct MonitorCardState {
    std::string id;
    std::wstring name;
    std::wstring status;  // localized short status
    bool controllable = false;
    // The ambient sensor drives this monitor: follow is on, the Laptop is available and the curve is complete.
    bool followed = false;
    SliderState brightness;
    SliderState volume;
    // Protection controls. Visibility comes from ProtectionRules; the values are the stored
    // target and its cap (0..100). The card clamps and saves the target.
    bool protectionToggleVisible = false;
    bool protectionOn = true;
    bool protectionRowVisible = false;
    double protectionValue = 0;
    double protectionCap = 0;
    // False when the brightness is unknown; the value is then shown but never saved, as on macOS.
    bool protectionCapKnown = false;
};

// Warning at the top of the flyout while the camera needs attention.
struct CameraNotice {
    bool visible = false;
    // Permission denial links to the Windows privacy setting.
    bool permission = false;
    std::wstring text;
};

struct FlyoutState {
    std::vector<MonitorCardState> monitors;
    PresenceIcon presence = PresenceIcon::Off;
    std::wstring presenceText;  // empty hides the presence row
    // Footer line, "Camera: <status>".
    std::wstring cameraText;
    CameraNotice cameraNotice;
    // False when the core is not open; the flyout then shows a notice instead of monitors.
    bool coreAvailable = true;
};

struct IFlyoutActions {
    virtual ~IFlyoutActions() = default;
    virtual void setBrightness(const std::string& monitorId, double value) = 0;
    virtual void setVolume(const std::string& monitorId, double value) = 0;
    virtual void setMonitorProtected(const std::string& monitorId, bool protectedOn) = 0;
    virtual void setProtectionBrightness(const std::string& monitorId, double value) = 0;
    virtual void refresh() = 0;
    virtual void openSettings() = 0;
    virtual void quit() = 0;
};

}  // namespace telaviva::host
