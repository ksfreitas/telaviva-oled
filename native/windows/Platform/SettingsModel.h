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

struct SettingsDraft {
    std::string locale = "auto";
    bool launchAtLogin = false;
    bool presenceProtectionEnabled = false;
    double presenceSamplingIntervalS = 10;
    double returnSamplingIntervalS = 1;
    bool suspendPresenceOnBattery = true;
    double presenceMinBrightnessPercent = 20;
    bool alwaysOnCamera = false;
    bool screensaverProtectionEnabled = false;
    bool hardwareKeysEnabled = false;
    int brightnessKeyStep = 5;
    int volumeKeyStep = 5;
    bool ambientFollowEnabled = false;
    std::map<std::string, double> screensaverTargets;
    std::set<std::string> protectionDisabled;
};

SettingsDraft draftFromSnapshotSettings(const SettingsSnapshot& settings);

// Clamps every value to the core's accepted range. NaN numeric values become the
// field default; NaN screensaver targets are dropped because no default is meaningful.
// Locales other than auto, en-US and pt-BR become auto. Empty or over-512-byte keys
// are dropped. protection_disabled keeps at most 128 entries, the core's limit.
SettingsDraft clampToCoreRanges(SettingsDraft draft);

// Returns the complete update_settings message. Never includes selected_camera_id,
// camera_history, ambient_curves or schema_version, because the core rejects unknown keys.
std::string buildSettingsPayload(const SettingsDraft& draft);

// Returns the update_settings message that sets or clears (nullopt, empty or over 512 bytes) the camera.
std::string buildSelectCameraPayload(const std::optional<std::string>& cameraId);

}  // namespace telaviva
