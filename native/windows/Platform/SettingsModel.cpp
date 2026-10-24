#include "SettingsModel.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace telaviva {
namespace {

// Mirrors the limits in crates/telaviva-core/src/settings.rs validate().
constexpr double kPresenceIntervalMin = 5.0;
constexpr double kPresenceIntervalMax = 300.0;
constexpr double kReturnIntervalMin = 0.25;
constexpr double kReturnIntervalMax = 5.0;
constexpr double kMinBrightnessMin = 0.0;
constexpr double kMinBrightnessMax = 90.0;
constexpr int kKeyStepMin = 1;
constexpr int kKeyStepMax = 15;
constexpr double kTargetMin = 0.0;
constexpr double kTargetMax = 100.0;
constexpr std::size_t kMaxIdBytes = 512;
constexpr std::size_t kMaxProtectionEntries = 128;
constexpr std::size_t kMaxInferenceDeviceBytes = 32;

// Clamps a finite or infinite value into the range. NaN has no ordering, so it takes the default.
double clampNumber(double value, double min, double max, double fallback)
{
    if (std::isnan(value)) {
        return fallback;
    }
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    // Folds -0.0 into 0 so the payload never carries a negative zero.
    return value == 0.0 ? 0.0 : value;
}

int clampStep(int step)
{
    if (step < kKeyStepMin) {
        return kKeyStepMin;
    }
    if (step > kKeyStepMax) {
        return kKeyStepMax;
    }
    return step;
}

bool validId(std::string_view id)
{
    return !id.empty() && id.size() <= kMaxIdBytes;
}

// Lowercase hex digits only, as the core's grammar requires.
bool lowerHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool lowerHex4(std::string_view text)
{
    if (text.size() != 4) {
        return false;
    }
    for (const char c : text) {
        if (!lowerHexDigit(c)) {
            return false;
        }
    }
    return true;
}

// One to three decimal digits for the adapter ordinal.
bool ordinalText(std::string_view text)
{
    if (text.empty() || text.size() > 3) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// Accepts auto, cpu, ane, gpu, or gpu:vvvv:dddd:n with lowercase hex ids and a 1-3 digit ordinal.
bool validInferenceDevice(std::string_view device)
{
    if (device.size() > kMaxInferenceDeviceBytes) {
        return false;
    }
    if (device == "auto" || device == "cpu" || device == "ane" || device == "gpu") {
        return true;
    }
    constexpr std::string_view prefix = "gpu:";
    if (device.substr(0, prefix.size()) != prefix) {
        return false;
    }
    // The part after "gpu:" is vvvv:dddd:n, which is 11 to 13 bytes long.
    const std::string_view rest = device.substr(prefix.size());
    if (rest.size() < 11 || rest.size() > 13) {
        return false;
    }
    if (rest[4] != ':' || rest[9] != ':') {
        return false;
    }
    return lowerHex4(rest.substr(0, 4)) && lowerHex4(rest.substr(5, 4)) && ordinalText(rest.substr(10));
}

// Shortest round-trip form, so 10.0 is written as 10. Callers pass finite values only.
std::string numberText(double value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

std::string boolText(bool value)
{
    return value ? "true" : "false";
}

// Writes a display-id to number object in the map's sorted order. Callers clamp the values first.
std::string numberMapText(const std::map<std::string, double>& values)
{
    std::string out = "{";
    bool first = true;
    for (const auto& [id, value] : values) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += jsonQuote(id);
        out.push_back(':');
        out += numberText(value);
    }
    out.push_back('}');
    return out;
}

// Keeps comma placement in one place, like the writer in JsonCodec.cpp.
class MemberWriter {
public:
    MemberWriter() : out_("{") {}

    void add(std::string_view key, const std::string& rawValue)
    {
        if (!first_) {
            out_.push_back(',');
        }
        first_ = false;
        out_ += jsonQuote(key);
        out_.push_back(':');
        out_ += rawValue;
    }

    std::string finish()
    {
        out_.push_back('}');
        return std::move(out_);
    }

private:
    std::string out_;
    bool first_ = true;
};

}  // namespace

SettingsDraft draftFromSnapshotSettings(const SettingsSnapshot& settings)
{
    SettingsDraft draft;
    draft.locale = settings.locale;
    draft.launchAtLogin = settings.launch_at_login;
    draft.presenceProtectionEnabled = settings.presence_protection_enabled;
    draft.presenceSamplingIntervalS = settings.presence_sampling_interval_s;
    draft.returnSamplingIntervalS = settings.return_sampling_interval_s;
    draft.suspendPresenceOnBattery = settings.suspend_presence_on_battery;
    draft.presenceMinBrightnessPercent = settings.presence_min_brightness_percent;
    draft.alwaysOnCamera = settings.always_on_camera;
    draft.screensaverProtectionEnabled = settings.screensaver_protection_enabled;
    draft.hardwareKeysEnabled = settings.hardware_keys_enabled;
    draft.brightnessKeyStep = settings.brightness_key_step;
    draft.volumeKeyStep = settings.volume_key_step;
    draft.ambientFollowEnabled = settings.ambient_follow_enabled;
    draft.builtinAutoBrightnessOffOnProtection = settings.builtin_auto_brightness_off_on_protection;
    draft.screensaverTargets = settings.screensaver_targets;
    draft.presenceMinBrightnessByDisplay = settings.presence_min_brightness_by_display;
    draft.protectionDisabled = settings.protection_disabled;
    draft.inferenceDevice = settings.inference_device;
    return draft;
}

SettingsDraft clampToCoreRanges(SettingsDraft draft)
{
    if (draft.locale != "auto" && draft.locale != "en-US" && draft.locale != "pt-BR") {
        draft.locale = "auto";
    }
    draft.presenceSamplingIntervalS =
        clampNumber(draft.presenceSamplingIntervalS, kPresenceIntervalMin, kPresenceIntervalMax, 10.0);
    draft.returnSamplingIntervalS =
        clampNumber(draft.returnSamplingIntervalS, kReturnIntervalMin, kReturnIntervalMax, 1.0);
    draft.presenceMinBrightnessPercent =
        clampNumber(draft.presenceMinBrightnessPercent, kMinBrightnessMin, kMinBrightnessMax, 20.0);
    draft.brightnessKeyStep = clampStep(draft.brightnessKeyStep);
    draft.volumeKeyStep = clampStep(draft.volumeKeyStep);

    std::map<std::string, double> targets;
    for (const auto& [id, value] : draft.screensaverTargets) {
        if (validId(id) && !std::isnan(value)) {
            targets[id] = clampNumber(value, kTargetMin, kTargetMax, kTargetMin);
        }
    }
    draft.screensaverTargets = std::move(targets);

    std::map<std::string, double> presenceByDisplay;
    for (const auto& [id, value] : draft.presenceMinBrightnessByDisplay) {
        if (validId(id)) {
            presenceByDisplay[id] = clampNumber(value, kMinBrightnessMin, kMinBrightnessMax, 20.0);
        }
    }
    draft.presenceMinBrightnessByDisplay = std::move(presenceByDisplay);

    std::set<std::string> disabled;
    for (const auto& id : draft.protectionDisabled) {
        if (validId(id) && disabled.size() < kMaxProtectionEntries) {
            disabled.insert(id);
        }
    }
    draft.protectionDisabled = std::move(disabled);

    if (!validInferenceDevice(draft.inferenceDevice)) {
        draft.inferenceDevice = "auto";
    }
    return draft;
}

std::string buildSettingsPayload(const SettingsDraft& input)
{
    const SettingsDraft draft = clampToCoreRanges(input);

    const std::string targets = numberMapText(draft.screensaverTargets);
    const std::string presenceByDisplay = numberMapText(draft.presenceMinBrightnessByDisplay);

    std::string disabled = "[";
    bool firstDisabled = true;
    for (const auto& id : draft.protectionDisabled) {
        if (!firstDisabled) {
            disabled.push_back(',');
        }
        firstDisabled = false;
        disabled += jsonQuote(id);
    }
    disabled.push_back(']');

    // Member order follows the core's field list; the core does not depend on it.
    MemberWriter settings;
    settings.add("locale", jsonQuote(draft.locale));
    settings.add("presence_sampling_interval_s", numberText(draft.presenceSamplingIntervalS));
    settings.add("return_sampling_interval_s", numberText(draft.returnSamplingIntervalS));
    settings.add("suspend_presence_on_battery", boolText(draft.suspendPresenceOnBattery));
    settings.add("presence_protection_enabled", boolText(draft.presenceProtectionEnabled));
    settings.add("presence_min_brightness_percent", numberText(draft.presenceMinBrightnessPercent));
    settings.add("always_on_camera", boolText(draft.alwaysOnCamera));
    settings.add("screensaver_protection_enabled", boolText(draft.screensaverProtectionEnabled));
    settings.add("launch_at_login", boolText(draft.launchAtLogin));
    settings.add("hardware_keys_enabled", boolText(draft.hardwareKeysEnabled));
    settings.add("brightness_key_step", std::to_string(draft.brightnessKeyStep));
    settings.add("volume_key_step", std::to_string(draft.volumeKeyStep));
    settings.add("inference_device", jsonQuote(draft.inferenceDevice));
    settings.add("screensaver_targets", targets);
    settings.add("ambient_follow_enabled", boolText(draft.ambientFollowEnabled));
    settings.add("protection_disabled", disabled);
    settings.add("builtin_auto_brightness_off_on_protection", boolText(draft.builtinAutoBrightnessOffOnProtection));
    settings.add("presence_min_brightness_by_display", presenceByDisplay);

    return buildUpdateSettings(settings.finish());
}

std::string buildSelectCameraPayload(const std::optional<std::string>& cameraId)
{
    // Only this payload carries the camera, so an unrelated save never re-sends a fallback.
    const std::string value = (cameraId && validId(*cameraId)) ? jsonQuote(*cameraId) : std::string("null");
    return buildUpdateSettings("{\"selected_camera_id\":" + value + "}");
}

std::string buildSelectMonitoredDisplayPayload(const std::optional<std::string>& displayId)
{
    // Only this payload carries the monitored display, so an unrelated save never re-sends a stale choice.
    const std::string value = (displayId && validId(*displayId)) ? jsonQuote(*displayId) : std::string("null");
    return buildUpdateSettings("{\"monitored_display_id\":" + value + "}");
}

}  // namespace telaviva
