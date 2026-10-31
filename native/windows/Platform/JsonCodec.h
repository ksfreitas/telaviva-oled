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

#include "ControlsTypes.h"

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
    // True for the display Windows marks as primary (MONITORINFOF_PRIMARY).
    bool primary = false;
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
    double return_sampling_interval_s = 0.5;
    bool suspend_presence_on_battery = true;
    double presence_min_brightness_percent = 20.0;
    bool always_on_camera = true;
    bool screensaver_protection_enabled = false;
    bool keep_awake_with_presence = false;
    bool lock_on_absence = false;
    double lock_delay_s = 5.0;
    bool launch_at_login = false;
    int brightness_key_step = 5;
    std::map<std::string, double> screensaver_targets;
    std::set<std::string> protection_disabled;
    // Inference device selector: auto, cpu, ane, gpu or gpu:vvvv:dddd:n.
    std::string inference_device = "auto";
    AmbientCurvesSnapshot ambientCurves;
    bool builtin_auto_brightness_off_on_protection = false;
    std::optional<std::string> monitored_display_id;
    // Read-only: the core owns the history and rejects writes to it.
    std::vector<std::string> camera_history;
    // Keyed by display id (a monitor id or "builtin"), as screensaver_targets is.
    std::map<std::string, double> presence_min_brightness_by_display;
};

struct ChannelSnapshot {
    bool supported = false;
    std::optional<double> observed;
    std::optional<double> raw;
    std::optional<double> desired;
    double min = 0.0;
    double max = 0.0;
    std::string status;
    // Normalized percentage 0 to 100, null when unknown. Controls events use it; observed stays raw.
    std::optional<double> normalized;
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

// The snapshot object builtin_protection. Defaults are the inert state for a shell that never reports protectable.
struct BuiltinProtectionSnapshot {
    bool protectable = false;
    bool protected_ = false;
    bool available = false;
    std::vector<std::string> reasons;
    std::optional<double> targetPercent;
    bool holdAutoBrightness = false;
    bool holding = false;
};

// The snapshot object monitored_display: the resolved display, and whether the history is empty.
struct MonitoredDisplaySnapshot {
    std::optional<std::string> id;
    bool automatic = false;
};

struct CameraSnapshot {
    std::string mode;
    std::string captureMode;
    std::string status;
    std::string displayStatus;
    std::optional<double> nextSampleInMs;
    std::optional<double> retryInMs;
    uint64_t generation = 0;
    std::optional<std::string> presence;
};

struct PowerSnapshot {
    std::optional<bool> onBattery;
};

struct ScreensaverSnapshot {
    std::string state;
};

// The snapshot object keep_awake. Defaults are the inert state for a snapshot that omits it.
struct KeepAwakeSnapshot {
    bool enabled = false;
    bool requested = false;
    uint64_t observationId = 0;
    uint64_t generation = 0;
    uint64_t validForMs = 0;
    std::string reason;
};

// The snapshot object lock. Defaults are the idle state for a snapshot that omits it.
struct LockSnapshot {
    bool enabled = false;
    std::string stage = "idle";
    bool requested = false;
    // Set only when the core sent a whole, non-negative number; nullopt otherwise.
    std::optional<uint64_t> token;
};

struct Snapshot {
    uint64_t revision = 0;
    std::string status;
    SettingsSnapshot settings;
    std::vector<MonitorSnapshot> monitors;
    std::vector<PendingAction> pendingActions;
    AmbientSnapshot ambient;
    BuiltinProtectionSnapshot builtinProtection;
    MonitoredDisplaySnapshot monitoredDisplay;
    CameraSnapshot camera;
    PowerSnapshot power;
    ScreensaverSnapshot screensaver;
    KeepAwakeSnapshot keepAwake;
    LockSnapshot lock;
    // Steady-clock seconds when parseSnapshot read this snapshot; keep-awake windows count down from it.
    double decodedAtSteadySeconds = 0.0;
};

// Returns text as a quoted, escaped JSON string literal.
std::string jsonQuote(std::string_view text);

std::string buildMonitorDiscovered(const MonitorInfo& monitor);
std::string buildMonitorDisconnected(const std::string& monitorId);
std::string buildMonitorDisplayState(const std::string& monitorId, uint64_t generation, const std::string& state);
std::string buildPower(std::optional<bool> onBattery);
std::string buildScreensaver(const std::string& state);
std::string buildKeepAwakeSession(bool active);
// error is omitted when empty.
std::string buildLockResult(uint64_t token, bool ok, const std::string& error);
std::string buildPresenceObservation(const std::string& status, uint64_t capturedAtMs, uint64_t cameraGeneration,
                                     const std::optional<std::string>& cameraStatus);
std::string buildCameraStatus(const std::string& status, std::optional<uint64_t> generation);
// protectable and holding default to false, which is what the legacy three-argument call sends.
std::string buildBuiltinDisplay(bool available, std::optional<double> brightness, bool calibrating,
                                bool protectable = false, bool holding = false);
std::string buildSetBrightness(const std::string& monitorId, double value,
                               std::optional<uint64_t> connectionGeneration);
std::string buildSetVolume(const std::string& monitorId, double value, std::optional<uint64_t> connectionGeneration);
std::string buildHardwareResult(uint64_t actionId, uint64_t generation, bool ok, std::optional<double> value,
                                const std::optional<std::string>& error);
// settingsJson must already be a JSON object; it is embedded unchanged.
std::string buildUpdateSettings(const std::string& settingsJson);

// Controls events for TvControls. Each returns one event object whose first number member is "now".
std::string buildControlsConfigure(double now, double brightnessStep, double volumeStep, bool keysEnabled,
                                   const std::optional<std::optional<std::string>>& volumeOwner);
std::string buildControlsObserve(double now, const Snapshot& snapshot);
std::string buildControlsSlider(double now, const std::string& monitorId, const std::string& feature, double value);
std::string buildControlsKey(double now, const std::string& key, bool down, bool repeat, bool fine,
                             const std::optional<KeyOwnerRef>& owner);
std::string buildControlsQueryDraft(double now, const std::string& monitorId, const std::string& feature);
std::string buildControlsMonitorProfile(double now, const std::string& monitorId, const std::string& manufacturer);
std::string buildControlsMonitorRemoved(double now, const std::string& monitorId);
std::string buildControlsDisplayPower(double now, bool on);
std::string buildControlsSubmitted(double now, uint64_t token, bool accepted);
std::string buildControlsAttemptDone(double now, uint64_t token, bool ok, const std::string& error,
                                     std::optional<double> value);
std::string buildControlsTick(double now);
std::string buildControlsShutdown(double now);

// Throws std::invalid_argument on malformed JSON, a non-object root, unknown effect types,
// or missing or mistyped fields.
Snapshot parseSnapshot(const std::string& json);
ControlsResult parseControlsResult(const std::string& json);

// Milliseconds left in a keep-awake window of validForMs that was read at decodedAtSteadySeconds.
// Zero when validForMs is zero or the window has run out; a clock that moved backwards counts as no elapsed time.
uint64_t keepAwakeRemainingMilliseconds(uint64_t validForMs, double decodedAtSteadySeconds, double nowSteadySeconds);

}  // namespace telaviva
