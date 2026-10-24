#pragma once
// Pure rules for the per-monitor protection controls on the flyout card. They mirror
// native/macos/TelaViva/MonitorPopup.swift (protectionToggle, screensaverTarget,
// protectionBrightnessMaximum and clampScreensaverTarget) and hold no UI or core state.
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace telaviva {

// The protection brightness is one fixed range for every screen, matching the core (1..50).
inline constexpr double kProtectionBrightnessMin = 1.0;
inline constexpr double kProtectionBrightnessMax = 50.0;

struct ProtectionClamp {
    double value = 0;
    // True when the stored value had to move, so the caller saves only then.
    bool changed = false;
};

// The toggle is useless when no protection mode runs, so it stays hidden until one is on.
inline bool showProtectionToggle(bool presenceProtectionEnabled, bool screensaverProtectionEnabled)
{
    return presenceProtectionEnabled || screensaverProtectionEnabled;
}

// Monitors are protected unless listed in protection_disabled, so an untouched monitor
// starts protected.
inline bool isMonitorProtected(const std::set<std::string>& protectionDisabled, const std::string& monitorId)
{
    return protectionDisabled.find(monitorId) == protectionDisabled.end();
}

// The brightness row only matters when a protected monitor is dimmed by presence or screensaver.
inline bool showProtectionBrightnessRow(bool presenceProtectionEnabled, bool screensaverProtectionEnabled,
                                        bool monitorProtected)
{
    return monitorProtected && (presenceProtectionEnabled || screensaverProtectionEnabled);
}

// A monitor without DDC brightness has no level to set, so its slider stays disabled.
inline bool protectionSliderEnabled(bool brightnessSupported)
{
    return brightnessSupported;
}

// The slider range is the fixed protection range, 1..50.
inline double protectionSliderMinimum()
{
    return kProtectionBrightnessMin;
}

inline double protectionSliderMaximum()
{
    return kProtectionBrightnessMax;
}

// Clamps the stored protection brightness into 1..50. Run it on show and on every
// brightness change. A non-finite stored value becomes 1 so it can be repaired. The result
// does not depend on any monitor's brightness.
inline ProtectionClamp clampProtectionBrightness(double stored)
{
    const double value = std::isfinite(stored)
        ? std::clamp(stored, kProtectionBrightnessMin, kProtectionBrightnessMax)
        : kProtectionBrightnessMin;
    return ProtectionClamp{value, value != stored};
}

// Returns protection_disabled after one monitor's toggle changes. std::set keeps the IDs
// sorted and unique, which is the payload shape the core expects. The 128-entry core limit
// is enforced by clampToCoreRanges when settings are saved, not here.
inline std::set<std::string> withMonitorProtection(std::set<std::string> protectionDisabled,
                                                   const std::string& monitorId,
                                                   bool monitorProtected)
{
    if (monitorProtected) {
        protectionDisabled.erase(monitorId);
    } else {
        protectionDisabled.insert(monitorId);
    }
    return protectionDisabled;
}

}  // namespace telaviva
