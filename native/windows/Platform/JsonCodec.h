#pragma once
// JSON commands sent to the core and the snapshot read back from it. Builders
// return compact UTF-8 text; the parser throws std::invalid_argument on bad input.
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace telaviva {

struct MonitorInfo {
    struct Channel {
        bool supported = false;
        std::optional<double> raw;
        std::optional<double> max;
    };

    std::string id;
    std::string name;
    bool identityStable = false;
    std::string connectionPath;
    Channel brightness;
    Channel volume;
};

// One calibration point as the core sends it: monitor percent and the matching built-in percent.
struct AmbientCurvePointSnapshot {
    double monitor = 0.0;
    double builtin = 0.0;
};

// Keyed by monitor id; points keep the order the core sent.
using AmbientCurvesSnapshot = std::map<std::string, std::vector<AmbientCurvePointSnapshot>>;

struct SettingsSnapshot {
    bool presence_protection_enabled = false;
    bool hardware_keys_enabled = false;
    bool ambient_follow_enabled = false;
    std::string locale;
    double presence_sampling_interval_s = 0.0;
    int volume_key_step = 0;
    std::optional<std::string> selected_camera_id;
    // Fields the Settings UI edits. The defaults match the core for a snapshot that omits them.
    double return_sampling_interval_s = 1.0;
    bool suspend_presence_on_battery = true;
    double presence_min_brightness_percent = 20.0;
    bool always_on_camera = false;
    bool screensaver_protection_enabled = false;
    bool launch_at_login = false;
    int brightness_key_step = 5;
    std::map<std::string, double> screensaver_targets;
    std::set<std::string> protection_disabled;
    // Inference device selector: auto, cpu, ane, gpu or gpu:vvvv:dddd:n.
    std::string inference_device = "auto";
    AmbientCurvesSnapshot ambientCurves;
};

struct ChannelSnapshot {
    bool supported = false;
    std::optional<double> observed;
    std::optional<double> raw;
    std::optional<double> desired;
    double min = 0.0;
    double max = 0.0;
    std::string status;
};

struct MonitorSnapshot {
    std::string id;
    std::string name;
    std::string identityStatus;
    std::string connectionPath;
    uint64_t generation = 0;
    std::string displayState;
    std::string follow;
    bool protected_ = false;
    ChannelSnapshot brightness;
    ChannelSnapshot volume;
};

struct PendingAction {
    uint64_t id = 0;
    std::string monitorId;
    uint64_t generation = 0;
    std::string kind;
    std::optional<double> value;
    std::optional<double> target;
};

struct AmbientSnapshot {
    bool enabled = false;
    bool available = false;
    bool active = false;
    bool calibrating = false;
    std::optional<double> builtinPercent;
};

struct CameraSnapshot {
    std::string mode;
    std::string captureMode;
    std::string status;
    std::string displayStatus;
    std::optional<double> nextSampleInMs;
    uint64_t generation = 0;
    std::optional<std::string> presence;
};

struct PowerSnapshot {
    std::optional<bool> onBattery;
};

struct ScreensaverSnapshot {
    std::string state;
};

struct Snapshot {
    uint64_t revision = 0;
    std::string status;
    SettingsSnapshot settings;
    std::vector<MonitorSnapshot> monitors;
    std::vector<PendingAction> pendingActions;
    AmbientSnapshot ambient;
    CameraSnapshot camera;
    PowerSnapshot power;
    ScreensaverSnapshot screensaver;
};

// Returns text as a quoted, escaped JSON string literal.
std::string jsonQuote(std::string_view text);

std::string buildMonitorDiscovered(const MonitorInfo& monitor);
std::string buildMonitorDisconnected(const std::string& monitorId);
std::string buildMonitorDisplayState(const std::string& monitorId, uint64_t generation, const std::string& state);
std::string buildPower(std::optional<bool> onBattery);
std::string buildScreensaver(const std::string& state);
std::string buildPresenceObservation(const std::string& status, uint64_t capturedAtMs, uint64_t cameraGeneration,
                                     const std::optional<std::string>& cameraStatus);
std::string buildCameraStatus(const std::string& status, std::optional<uint64_t> generation);
std::string buildBuiltinDisplay(bool available, std::optional<double> brightness, bool calibrating);
std::string buildSetBrightness(const std::string& monitorId, double value,
                               std::optional<uint64_t> connectionGeneration);
std::string buildSetVolume(const std::string& monitorId, double value, std::optional<uint64_t> connectionGeneration);
std::string buildHardwareResult(uint64_t actionId, uint64_t generation, bool ok, std::optional<double> value,
                                const std::optional<std::string>& error);
// settingsJson must already be a JSON object; it is embedded unchanged.
std::string buildUpdateSettings(const std::string& settingsJson);

// Throws std::invalid_argument on malformed JSON or a non-object root.
Snapshot parseSnapshot(const std::string& json);

}  // namespace telaviva
