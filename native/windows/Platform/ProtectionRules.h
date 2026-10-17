#pragma once
// Pure rules for the per-monitor protection controls on the flyout card. They mirror
// native/macos/TelaViva/MonitorPopup.swift (protectionToggle, screensaverTarget,
// protectionBrightnessMaximum and clampScreensaverTarget) and hold no UI or core state.
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>

namespace telaviva {

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

// The brightness row only matters when screensaver protection dims the monitor.
inline bool showProtectionBrightnessRow(bool screensaverProtectionEnabled, bool monitorProtected)
{
    return screensaverProtectionEnabled && monitorProtected;
}

// Returns the cap for the protection brightness slider. confirmed is the core's last
// reported brightness (0..100); nullopt means the control is unsupported or has no value,
// which yields 0 so the slider stays disabled. outstandingDraft is the pending local slider
// value, or nullopt when none is pending. Callers pass nullopt for a followed monitor, since
// the ambient sensor drives its brightness and a leftover draft must not lower the cap.
inline double protectionBrightnessCap(std::optional<double> confirmed, std::optional<double> outstandingDraft)
{
    if (!confirmed || !std::isfinite(*confirmed)) {
        return 0;
    }
    const double base = std::clamp(*confirmed, 0.0, 100.0);
    if (!outstandingDraft) {
        return base;
    }
    return std::min(base, std::max(0.0, *outstandingDraft));
}

// A cap of 0 leaves nothing to dim to, so the slider is disabled and shows 0.
inline bool protectionSliderEnabled(double cap)
{
    return cap > 0;
}

// The slider range is 0..cap. A disabled slider still needs a non-empty range, so it uses 1.
inline double protectionSliderMaximum(double cap)
{
    return cap > 0 ? cap : 1;
}

// Clamps the stored protection brightness into 0..cap. Run it on show and on every
// brightness change. A non-finite stored value is treated as 0 so it can be repaired.
inline ProtectionClamp clampProtectionBrightness(double stored, double cap)
{
    const double finite = std::isfinite(stored) ? stored : 0.0;
    const double value = std::clamp(finite, 0.0, std::max(0.0, cap));
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
