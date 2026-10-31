#pragma once
// Settings draft edited by the Settings UI and the update_settings payloads built from it.
// The core validates on submit, but clamping here keeps one bad slider value from
// rejecting a whole save. Camera choice and ambient curves travel through other paths.
#include "JsonCodec.h"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace telaviva {

// Shared with the Settings UI. The core enforces the same limits.
inline constexpr double kPresenceMinBrightnessMin = 1.0;
inline constexpr double kPresenceMinBrightnessMax = 90.0;
inline constexpr double kLockDelayMin = 1.0;
inline constexpr double kLockDelayMax = 60.0;
inline constexpr double kLockDelayDefault = 5.0;

struct SettingsDraft {
    std::string locale = "auto";
    bool launchAtLogin = false;
    bool presenceProtectionEnabled = false;
    double presenceSamplingIntervalS = 10;
    double returnSamplingIntervalS = 0.5;
    bool suspendPresenceOnBattery = true;
    double presenceMinBrightnessPercent = 20;
    bool alwaysOnCamera = true;
    bool screensaverProtectionEnabled = false;
    bool keepAwakeWithPresence = false;
    bool lockOnAbsence = false;
    double lockDelayS = 5;
    bool hardwareKeysEnabled = false;
    int brightnessKeyStep = 5;
    int volumeKeyStep = 5;
    bool ambientFollowEnabled = false;
    bool builtinAutoBrightnessOffOnProtection = false;
    std::map<std::string, double> screensaverTargets;
    // Keyed by display id (a monitor id or "builtin"), like screensaverTargets.
    std::map<std::string, double> presenceMinBrightnessByDisplay;
    std::set<std::string> protectionDisabled;
    std::string inferenceDevice = "auto";
};

SettingsDraft draftFromSnapshotSettings(const SettingsSnapshot& settings);

// Clamps every value to the core's accepted range. NaN numeric values become the
// field default; NaN screensaver targets are dropped because no default is meaningful.
// Per-display presence thresholds clamp to 1..90 and NaN becomes the global default.
// The core raises a stored 0 to 1 on load, and the shell clamps to 1 as well.
// The lock delay clamps to 1..60 and NaN becomes 5.
// Locales other than auto, en-US and pt-BR become auto. Empty or over-512-byte keys
// are dropped. protection_disabled keeps at most 128 entries, the core's limit.
// An inference device other than auto, cpu, ane, gpu or a gpu:vvvv:dddd:n selector
// (lowercase hex, 1-3 digit ordinal, at most 32 bytes) becomes auto.
SettingsDraft clampToCoreRanges(SettingsDraft draft);

// Returns the complete update_settings message. Never includes selected_camera_id,
// camera_history, monitored_display_id, monitored_display_history, ambient_curves or
// schema_version, because the core rejects unknown keys or sends them by their own command.
std::string buildSettingsPayload(const SettingsDraft& draft);

// Returns the update_settings message that sets or clears (nullopt, empty or over 512 bytes) the camera.
std::string buildSelectCameraPayload(const std::optional<std::string>& cameraId);

// Returns the update_settings message that sets or clears (nullopt, empty or over 512 bytes) the
// monitored display. Automatic is nullopt. The reserved id "builtin" is accepted.
std::string buildSelectMonitoredDisplayPayload(const std::optional<std::string>& displayId);

}  // namespace telaviva
