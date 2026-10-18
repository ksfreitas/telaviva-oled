#include "pch.h"

#include "Host/AppController.h"
#include "Host/Dxva2PortProvider.h"
#include "Host/PlatformWindow.h"
#include "Host/Strings.h"
#include "Host/ThreadExecutor.h"

#include "Platform/AdapterCatalog.h"
#include "Platform/AdaptiveBrightness.h"
#include "Platform/AmbientCalibrationController.h"
#include "Platform/AppPaths.h"
#include "Platform/AudioOwner.h"
#include "Platform/CalibrationSession.h"
#include "Platform/CameraLifecycleDriver.h"
#include "Platform/CameraSelection.h"
#include "Platform/CoreBridge.h"
#include "Platform/CoreLifecycleApi.h"
#include "Platform/DdcController.h"
#include "Platform/Diagnostics.h"
#include "Platform/DeviceSelection.h"
#include "Platform/DisplayCatalog.h"
#include "Platform/EfficiencyController.h"
#include "Platform/InferenceFailureStore.h"
#include "Platform/InferenceHost.h"
#include "Platform/JsonCodec.h"
#include "Platform/KeyHook.h"
#include "Platform/KeyIntentRouter.h"
#include "Platform/KeyRoute.h"
#include "Platform/LaptopDetection.h"
#include "Platform/LaptopPanel.h"
#include "Platform/LaunchAtLogin.h"
#include "Platform/MediaCaptureBackend.h"
#include "Platform/ManualControls.h"
#include "Platform/PowerEvents.h"
#include "Platform/ProtectionRules.h"
#include "Platform/Scheduler.h"
#include "Platform/ScreensaverMonitor.h"
#include "Platform/SelftestFrame.h"
#include "Platform/SessionEvents.h"
#include "Platform/SettingsModel.h"
#include "Platform/SystemClock.h"
#include "Platform/SystemSleeper.h"
#include "Platform/UpdateChecker.h"
#include "Platform/VolumeActionTracker.h"
#include "Platform/Win32Efficiency.h"
#include "Platform/WinHttpGet.h"

#include <winrt/Windows.System.Threading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace telaviva::host {

namespace {

// Posted to the window when the Laptop becomes unavailable from a thread other than the UI thread
// (the snapshot thread, or the lid callback). The handler ends an active calibration on the UI thread.
constexpr UINT kLaptopUnavailableMessage = WM_APP + 0x31;

// Trailing delay for the protection brightness slider: one settings submission per pause in dragging.
constexpr std::chrono::milliseconds kProtectionDelay{150};

std::wstring toWide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string toUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
        nullptr);
    return out;
}

// BCP 47 name of the user's locale, for the core's compatibility field.
std::string userLocaleName()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) == 0) {
        return {};
    }
    return toUtf8(std::wstring_view{ name });
}

// TELAVIVA_LOG_LEVEL lowercased, for the diagnostics level. Empty when unset, too long or not ASCII.
std::string requestedLogLevel()
{
    wchar_t value[32]{};
    const DWORD length = GetEnvironmentVariableW(L"TELAVIVA_LOG_LEVEL", value, 32);
    if (length == 0 || length >= 32) {
        return {};
    }
    std::string text;
    for (DWORD index = 0; index < length; ++index) {
        const wchar_t c = value[index];
        if (c > 127) {
            return {};
        }
        const char lower = (c >= L'A' && c <= L'Z') ? static_cast<char>(c - L'A' + 'a') : static_cast<char>(c);
        text.push_back(lower);
    }
    return text;
}

// Short form of a monitor id for traces: its last eight characters.
std::string traceMonitor(const std::string& monitorId)
{
    return monitorId.size() > 8 ? monitorId.substr(monitorId.size() - 8) : monitorId;
}

std::string formatPercent(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", value);
    return buffer;
}

// Full path of this executable, which the launch-at-login command starts.
std::wstring executablePath()
{
    // GetModuleFileNameW truncates silently when the buffer is too small, so the buffer grows
    // until the path fits.
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// Hands commands to the core on behalf of DdcController.
class BridgeSubmitter final : public ICoreSubmitter {
public:
    explicit BridgeSubmitter(CoreBridge& bridge) : bridge_(bridge) {}

    int submit(const std::string& json) override { return bridge_.submit(json); }

private:
    CoreBridge& bridge_;
};

// Turns slider drafts into set_brightness or set_volume commands.
class CoreManualSink final : public IManualSink {
public:
    explicit CoreManualSink(CoreBridge& bridge) : bridge_(bridge) {}

    int submit(std::string_view monitorId, ManualFeature feature, double value,
        uint64_t connectionGeneration) override
    {
        const std::string id(monitorId);
        const std::string json = feature == ManualFeature::Brightness
            ? buildSetBrightness(id, value, connectionGeneration)
            : buildSetVolume(id, value, connectionGeneration);
        return bridge_.submit(json);
    }

private:
    CoreBridge& bridge_;
};

// Forwards DDC completions to the manual controls. The DDC worker may call it until DDC shuts
// down, so shutdown() closes the gate first; close() waits for a call in progress, so none
// reaches a controller that is being destroyed.
class ManualCompletionGate final : public IHardwareObserver {
public:
    void open(ManualControls* manual)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_ = manual;
    }

    void close() { open(nullptr); }

    void onHardwareCompleted(const HardwareCompletion& completion) override
    {
        std::optional<ManualFeature> feature;
        if (completion.feature == "brightness") {
            feature = ManualFeature::Brightness;
        } else if (completion.feature == "volume") {
            feature = ManualFeature::Volume;
        }
        if (!feature) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (manual_ != nullptr) {
                manual_->onHardwareCompleted(completion.monitorId, *feature, completion.ok, completion.observedPercent);
            }
        }
        // Outside the gate's lock: the volume hook takes the key lock.
        if (*feature == ManualFeature::Volume && volumeHook_) {
            volumeHook_(completion);
        }
    }

    // Set once in the constructor, before the DDC controller can complete anything.
    void setVolumeHook(std::function<void(const HardwareCompletion&)> hook) { volumeHook_ = std::move(hook); }

private:
    std::mutex mutex_;
    ManualControls* manual_ = nullptr;
    std::function<void(const HardwareCompletion&)> volumeHook_;
};

// Forwards the slider targets of the manual controls to the key router. Other features are not routed.
class ManualKeyBridge final : public IManualObserver {
public:
    std::function<void(const std::string&, uint64_t, double, uint64_t)> changed;
    std::function<void(const std::string&, uint64_t, double, uint64_t)> submitted;

    void onTargetChanged(const std::string& monitorId, uint64_t connectionGeneration, ManualFeature feature,
        double value, uint64_t revision) override
    {
        if (feature == ManualFeature::Volume && changed) {
            changed(monitorId, connectionGeneration, value, revision);
        }
    }

    void onTargetSubmitted(const std::string& monitorId, uint64_t connectionGeneration, ManualFeature feature,
        double value, uint64_t revision) override
    {
        if (feature == ManualFeature::Volume && submitted) {
            submitted(monitorId, connectionGeneration, value, revision);
        }
    }
};

// A channel with a hardware error reports a status outside this list. Only supported
// channels are checked, since an unsupported channel has no hardware to fail.
bool channelFailed(const ChannelSnapshot& channel)
{
    if (!channel.supported) {
        return false;
    }
    const std::string& status = channel.status;
    return !(status == "ready" || status == "pending" || status == "verifying_ownership" ||
        status == "read_required");
}

// The snapshot value as a 0..100 percent, computed from raw when the core sent no observed value.
double observedPercent(const ChannelSnapshot& channel)
{
    double value = 0.0;
    if (channel.observed) {
        value = *channel.observed;
    } else if (channel.raw && channel.max > channel.min) {
        value = (*channel.raw - channel.min) / (channel.max - channel.min) * 100.0;
    }
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 100.0);
}

// One monitor's curve as the calibration controller reads it. Points keep the order the core sent.
AmbientCurve curveFor(const SettingsSnapshot& settings, const std::string& monitorId)
{
    AmbientCurve curve;
    const auto found = settings.ambientCurves.find(monitorId);
    if (found != settings.ambientCurves.end()) {
        for (const AmbientCurvePointSnapshot& point : found->second) {
            curve.points.push_back(AmbientCurvePoint{ point.monitor, point.builtin });
        }
    }
    return curve;
}

AmbientCurveMap curveMapFrom(const SettingsSnapshot& settings)
{
    AmbientCurveMap curves;
    for (const auto& entry : settings.ambientCurves) {
        curves[entry.first] = curveFor(settings, entry.first);
    }
    return curves;
}

std::optional<std::string> noticeKey(const std::optional<CalibrationNotice>& notice)
{
    if (!notice) {
        return std::nullopt;
    }
    switch (*notice) {
    case CalibrationNotice::BuiltInUnreadable: return std::string{ "builtInUnreadable" };
    case CalibrationNotice::BuiltInWriteFailed: return std::string{ "builtInWriteFailed" };
    case CalibrationNotice::BuiltInUnavailable: return std::string{ "builtInUnavailable" };
    case CalibrationNotice::AutoBrightnessUnknown: return std::string{ "autoBrightnessUnknown" };
    case CalibrationNotice::AutoBrightnessNotChanged: return std::string{ "autoBrightnessNotChanged" };
    case CalibrationNotice::RecoveryFailed: return std::string{ "recoveryFailed" };
    }
    return std::nullopt;
}

// The calibration seams. The controller runs on the UI thread; the snapshot copies are written on
// the snapshot thread, so the reads take the mutex that guards them.
class PanelAdapter final : public ICalibrationPanel {
public:
    explicit PanelAdapter(WmiPanel& panel) : panel_(panel) {}

    std::optional<double> panelPercent() override { return panel_.readPercent(); }
    bool setPanelPercent(double percent) override { return panel_.setPercent(percent); }

private:
    WmiPanel& panel_;
};

class FollowPauseAdapter final : public IFollowPause {
public:
    explicit FollowPauseAdapter(LaptopPanelFollower& follower) : follower_(follower) {}

    void setCalibrating(bool calibrating) override { follower_.onCalibrationChanged(calibrating); }
    bool paused() override { return !follower_.canCalibrate(); }

private:
    LaptopPanelFollower& follower_;
};

class MonitorsAdapter final : public ICalibrationMonitors {
public:
    MonitorsAdapter(std::mutex& mutex, const Snapshot& latest, ManualControls& manual)
        : mutex_(mutex), latest_(latest), manual_(manual) {}

    std::optional<double> observedPercent(const std::string& monitorId) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const MonitorSnapshot& monitor : latest_.monitors) {
            if (monitor.id == monitorId) {
                const ChannelSnapshot& channel = monitor.brightness;
                if (!channel.observed && !channel.raw) {
                    return std::nullopt;
                }
                return ::telaviva::host::observedPercent(channel);
            }
        }
        return std::nullopt;
    }

    void setPercent(const std::string& monitorId, double percent) override
    {
        manual_.setTarget(monitorId, ManualFeature::Brightness, percent);
    }

private:
    std::mutex& mutex_;
    const Snapshot& latest_;
    ManualControls& manual_;
};

class SettingsAdapter final : public ICalibrationSettings {
public:
    SettingsAdapter(std::mutex& mutex, const SettingsSnapshot& latest, CoreBridge& bridge)
        : mutex_(mutex), latest_(latest), bridge_(bridge) {}

    SettingsDraft currentDraft() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return draftFromSnapshotSettings(latest_);
    }

    AmbientCurveMap curves() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return curveMapFrom(latest_);
    }

    int submit(const std::string& json) override { return bridge_.submit(json); }

private:
    std::mutex& mutex_;
    const SettingsSnapshot& latest_;
    CoreBridge& bridge_;
};

// Target of kLaptopUnavailableMessage. The window handler holds it weakly; shutdown() resets it.
struct LaptopMessageTarget {
    std::function<void()> onUnavailable;
};

SliderState buildSlider(const MonitorSnapshot& monitor, ManualFeature feature, const ChannelSnapshot& channel,
    bool controllable, const ManualControls& manual, std::string_view reasonKey)
{
    SliderState slider;
    slider.available = channel.supported && controllable;
    // The user's draft wins while it exists; otherwise show what the hardware reported.
    slider.value = manual.draft(monitor.id, feature).value_or(observedPercent(channel));
    // A rejected draft has already fallen back to the observed value in value.
    slider.failed = manual.status(monitor.id, feature) == ManualStatus::Failed;
    if (!slider.available) {
        slider.reason = Str(reasonKey);
    }
    return slider;
}

// The display status when present, else the raw status, as on the Settings page and in AppText on macOS.
std::string cameraStatusKey(const Snapshot& snapshot)
{
    const auto& camera = snapshot.camera;
    if (!camera.displayStatus.empty()) {
        return camera.displayStatus;
    }
    if (!camera.status.empty()) {
        return camera.status;
    }
    return "unknown";
}

// Presence as the flyout and tray show it. Protection off reads as Off; a missing or unknown
// presence reads as absent, as the macOS status item does.
PresenceIcon presenceFor(const Snapshot& snapshot)
{
    if (!snapshot.settings.presence_protection_enabled) {
        return PresenceIcon::Off;
    }
    const auto& presence = snapshot.camera.presence;
    if (presence && *presence == "present") {
        return PresenceIcon::Detected;
    }
    if (presence && *presence == "insufficient_light") {
        return PresenceIcon::LowLight;
    }
    return PresenceIcon::Absent;
}

std::string_view presenceKey(PresenceIcon presence)
{
    switch (presence) {
    case PresenceIcon::Detected: return "flyout.presence.detected";
    case PresenceIcon::Absent: return "flyout.presence.absent";
    case PresenceIcon::LowLight: return "flyout.presence.lowlight";
    case PresenceIcon::Off: break;
    }
    return "flyout.presence.off";
}

// "Camera: <status>", the same line the Settings page shows.
std::wstring cameraLine(const std::string& key)
{
    return Str("camera") + L": " + Str("cameraStatus." + key);
}

MonitorCardState buildCard(const Snapshot& snapshot, const MonitorSnapshot& monitor, const ManualControls& manual)
{
    // The core marks monitors with a shared EDID as anything but "stable".
    const bool stable = monitor.identityStatus == "stable";
    const bool awake = monitor.displayState != "asleep";
    const bool anySupported = monitor.brightness.supported || monitor.volume.supported;

    MonitorCardState card;
    card.id = monitor.id;
    card.name = toWide(monitor.name);
    card.controllable = stable && awake && anySupported;
    // Protection controls. A followed monitor is driven by the ambient sensor, so the
    // protection cap ignores any leftover brightness draft for it, as on macOS.
    const SettingsSnapshot& settings = snapshot.settings;
    const bool followed = snapshot.ambient.active && monitor.follow == "eligible";
    card.followed = followed;
    const bool monitorProtected = isMonitorProtected(settings.protection_disabled, monitor.id);
    card.protectionToggleVisible =
        showProtectionToggle(settings.presence_protection_enabled, settings.screensaver_protection_enabled);
    card.protectionOn = monitorProtected;
    card.protectionRowVisible = showProtectionBrightnessRow(settings.screensaver_protection_enabled, monitorProtected);
    const std::optional<double> confirmed = monitor.brightness.supported
        ? std::optional<double>{ observedPercent(monitor.brightness) }
        : std::nullopt;
    const std::optional<double> draft = followed ? std::nullopt
                                                 : manual.draft(monitor.id, ManualFeature::Brightness);
    card.protectionCap = protectionBrightnessCap(confirmed, draft);
    card.protectionCapKnown = confirmed.has_value();
    const auto target = settings.screensaver_targets.find(monitor.id);
    card.protectionValue = target == settings.screensaver_targets.end() ? 0.0 : target->second;

    // Order matters: the first matching reason is the one the user sees.
    if (!stable) {
        card.status = Str("flyout.status.ambiguous");
    } else if (!awake) {
        card.status = Str("flyout.status.asleep");
    } else if (!anySupported) {
        card.status = Str("flyout.status.unsupported");
    } else if (channelFailed(monitor.brightness) || channelFailed(monitor.volume)) {
        card.status = Str("flyout.status.unavailable");
    } else {
        card.status = Str("flyout.status.ready");
    }

    card.brightness = buildSlider(monitor, ManualFeature::Brightness, monitor.brightness, card.controllable,
        manual, "flyout.reason.brightness_unavailable");
    card.volume = buildSlider(monitor, ManualFeature::Volume, monitor.volume, card.controllable, manual,
        "flyout.reason.volume_unavailable");
    return card;
}

FlyoutState buildFlyoutState(const Snapshot& snapshot, const ManualControls& manual)
{
    FlyoutState state;
    state.monitors.reserve(snapshot.monitors.size());
    for (const auto& monitor : snapshot.monitors) {
        state.monitors.push_back(buildCard(snapshot, monitor, manual));
    }

    state.presence = presenceFor(snapshot);
    state.presenceText = Str(presenceKey(state.presence));

    const std::string key = cameraStatusKey(snapshot);
    state.cameraText = cameraLine(key);
    // Denied, busy, unavailable and error get the warning, as on the Settings page.
    const bool attention = key == "permission" || key == "busy" || key == "unavailable" || key == "error";
    state.cameraNotice.visible = attention;
    state.cameraNotice.permission = key == "permission";
    state.cameraNotice.text = state.cameraText;
    return state;
}

// Shown when the core is not open: no monitors, and the flyout shows its notice.
FlyoutState unavailableState()
{
    FlyoutState state;
    state.coreAvailable = false;
    // No snapshot, so no camera status either; the footer reads unavailable and no warning is shown.
    state.cameraText = cameraLine("unavailable");
    return state;
}

}  // namespace

// Members are built in declaration order. The explicit teardown in ~AppController
// depends on it, so keep the order.
struct AppController::Services {
    AppPaths paths;
    Diagnostics diagnostics{ paths.logFile() };
    std::function<void(FlyoutState)> onFlyoutState;
    std::function<void()> onOpenSettings;
    std::function<void()> onQuit;
    Win32DisplayCatalog catalog;
    Dxva2PortProvider ports;
    SystemSleeper sleeper;
    ThreadpoolScheduler scheduler;
    SystemClock clock;
    std::unique_ptr<CoreBridge> bridge;
    std::unique_ptr<BridgeSubmitter> submitter;
    std::unique_ptr<CoreManualSink> manualSink;
    std::unique_ptr<ThreadExecutor> executor;
    // Declared before ddc, so it outlives the controller that calls it. Closed by shutdown().
    ManualCompletionGate manualGate;
    std::unique_ptr<DdcController> ddc;
    // Feeds slider targets and set_volume acceptances to the key router. The tracker and the connection
    // generation map are used under keyMutex, on the snapshot thread.
    ManualKeyBridge manualKeyBridge;
    VolumeActionTracker volumeActions;
    std::map<std::string, uint64_t> keyConnectionGenerations;
    std::unique_ptr<ManualControls> manual;

    // ---- Protection brightness slider. Ticks only store the newest value per monitor; one trailing
    // timer submits them with the latest settings. submitMutex is held across a submission, so shutdown
    // and a flush wait for it. Cancelling a timer happens outside every lock, since cancel waits for a
    // running callback that takes submitMutex. ----
    std::mutex protectionSubmitMutex;
    std::mutex protectionMutex;
    std::map<std::string, double> protectionPending;
    std::optional<TimerId> protectionTimer;
    uint64_t protectionToken = 0;  // bumped on every arm and flush, so a stale callback does nothing
    bool protectionAccepting = true;

    // ---- Laptop follow and ambient calibration (T117-T132, T164-T168). Built after manual and before
    // the session and power callbacks, which refer to them. Used by publish() and by the UI thread. ----
    Win32LaptopEnvironment laptopEnvironment;
    std::unique_ptr<WmiPanel> wmiPanel;
    std::unique_ptr<LaptopPanelFollower> laptopFollower;
    Win32PowerSettings powerSettings;
    std::unique_ptr<AdaptiveBrightness> adaptive;
    std::unique_ptr<CalibrationSession> calibrationSession;
    std::unique_ptr<PanelAdapter> calibrationPanel;
    std::unique_ptr<FollowPauseAdapter> calibrationFollow;
    std::unique_ptr<MonitorsAdapter> calibrationMonitors;
    std::unique_ptr<SettingsAdapter> calibrationSettings;
    std::unique_ptr<AmbientCalibrationController> calibration;
    // Gate for the follower, as for the screensaver: publish() runs on the snapshot thread, and
    // shutdown() clears laptopAccepting under laptopMutex before it stops the follower.
    std::mutex laptopMutex;
    bool laptopAccepting = true;
    std::optional<bool> laptopFollowSent;
    // Snapshot thread only, under laptopMutex: availability at the last publish, for the drop check.
    bool laptopAvailable = false;
    HWND uiWindow = nullptr;
    // Held weakly by the window handler for kLaptopUnavailableMessage. shutdown() resets it.
    std::shared_ptr<LaptopMessageTarget> laptopTarget;
    // Written by publish() under settingsMutex, read by laptopStatus().
    LaptopStatus latestLaptop;

    std::shared_ptr<SessionEvents> session;
    // Written on the core's snapshot thread and read by the Settings pages on the UI thread.
    // The camera cache is written by refreshCameras on a worker thread.
    std::mutex settingsMutex;
    SettingsSnapshot latestSettings;
    Snapshot latestSnapshot;
    std::vector<CameraInfo> cameraCache;
    // Declared before launchAtLogin, which refers to it.
    RunKeyRegistry runKey;
    std::unique_ptr<LaunchAtLogin> launchAtLogin;
    // Presence pipeline. Its callbacks submit through the bridge, so ~AppController
    // tears these down before the bridge.
    TvInferenceApi inference;
    DxgiAdapterCatalog adapters;
    InferenceFailureStore failures{ paths.root() };
    SystemPowerStatusReader powerReader;
    SystemScreensaverProbe screensaverProbe;
    std::unique_ptr<InferenceHost> analyzer;
    // The inference_device value last given to the analyzer. Only applyInferenceDevice touches it:
    // startup, before the snapshot sink is added, and then the snapshot thread alone.
    std::string appliedInferenceDevice = "auto";
    std::unique_ptr<MediaCaptureBackend> camera;
    std::unique_ptr<CoreLifecycleApi> lifecycle;
    std::unique_ptr<CameraLifecycleDriver> driver;
    // Held weakly by the window handler, so the handler drops out once this is reset.
    std::shared_ptr<PowerEvents> power;
    std::unique_ptr<ScreensaverMonitor> screensaver;
    // publish() runs on the bridge worker thread and calls setEnabled, while shutdown()
    // disables the monitor from the UI thread. The mutex serializes the two, and the flag
    // stops publish() from touching the monitor after shutdown began.
    std::mutex screensaverMutex;
    bool screensaverAccepting = true;
    bool screensaverEnabled = false;

    // ---- Efficiency mode (T101). Built and used on the UI thread only. ----
    Win32Efficiency efficiency;
    std::unique_ptr<EfficiencyController> efficiencyController;
    // Flyout and Settings windows shown now.
    uint32_t visibleWindows = 0;
    // The controller is not thread-safe, and the hook thread is exempted from publish() on the
    // snapshot thread. Every call into efficiencyController takes this lock. It is taken inside
    // keyMutex, never the other way round.
    std::mutex efficiencyMutex;

    // ---- Volume and mute keys (T112). Shutdown stops them before the bridge drains. ----
    // The hook callback runs on the hook thread: it reads keyRoute and pushes keyQueue through
    // keyPolicy, and signals keyWake only for a swallowed key. keyConsumer wakes on keyWake,
    // drives keyRouter, submits the intents it takes and updates audioOwner. Nothing else
    // touches the router from another thread.
    KeyRoutePublisher keyRoute;
    KeyEventQueue keyQueue;
    KeyHookPolicy keyPolicy{ keyRoute, keyQueue };
    std::unique_ptr<Win32KeyHook> keyHook;
    std::unique_ptr<KeyHookController> keyHookController;
    std::unique_ptr<KeyIntentRouter> keyRouter;
    std::unique_ptr<Win32AudioEndpoints> audioEndpoints;
    std::unique_ptr<PnpMonitorContainers> monitorContainers;
    std::unique_ptr<AudioOwner> audioOwner;
    HANDLE keyWake = nullptr;
    std::thread keyConsumer;
    std::atomic<bool> keyStop{ false };
    std::atomic<bool> keyPathLogged{ false };
    // Guards keyAccepting and the key fields below, and the router and hook controller calls.
    // Never held across a core call, so the core may call publish() while a command runs.
    std::mutex keyMutex;
    bool keyAccepting = true;
    bool keyHookWanted = false;
    std::optional<std::string> keyOwner;
    // Owner monitor the router was last configured with; empty when none. Read by the consumer for traces.
    std::string keyRouterOwner;
    std::vector<std::string> keyMonitorIds;
    bool keyMonitorsDirty = false;
    // Hook thread id last exempted from Efficiency mode; 0 when none.
    uint32_t keyExemptThread = 0;
    // Set in the constructor only when TELAVIVA_LOG_LEVEL asks for debug; empty otherwise.
    std::function<void(const std::string&)> trace;

    bool stopped = false;

    // Gives the analyzer the stored device when it differs from the last one applied. The value is
    // passed as a copy taken under settingsMutex, so setPreference runs without that lock held.
    void applyInferenceDevice(const std::string& value)
    {
        if (!analyzer || value == appliedInferenceDevice) {
            return;
        }
        appliedInferenceDevice = value;
        analyzer->setPreference(value);
    }

    // Runs on the core's snapshot thread after each accepted revision.
    void publish(const Snapshot& snapshot)
    {
        std::string inferenceDevice;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            latestSettings = snapshot.settings;
            latestSnapshot = snapshot;
            inferenceDevice = latestSettings.inference_device;
        }
        applyInferenceDevice(inferenceDevice);
        ddc->onSnapshot(snapshot);
        manual->apply(snapshot);
        driver->onSnapshot(snapshot);
        {
            std::lock_guard<std::mutex> lock(screensaverMutex);
            const bool enabled = snapshot.settings.screensaver_protection_enabled;
            if (screensaverAccepting && enabled != screensaverEnabled) {
                screensaverEnabled = enabled;
                screensaver->setEnabled(enabled);
            }
        }
        // Laptop follow. Gated like the screensaver: shutdown() clears laptopAccepting before it stops the follower.
        {
            std::lock_guard<std::mutex> lock(laptopMutex);
            if (laptopAccepting && laptopFollower) {
                const bool follow = snapshot.settings.ambient_follow_enabled;
                if (laptopFollowSent != follow) {
                    laptopFollowSent = follow;
                    laptopFollower->setFollowEnabled(follow);
                }
                const LaptopReason reason = laptopFollower->reason();
                const bool available = reason == LaptopReason::None;
                // The lid and display callbacks run on the UI thread, so a drop seen here is posted, not acted on.
                if (laptopAvailable && !available && uiWindow != nullptr) {
                    PostMessageW(uiWindow, kLaptopUnavailableMessage, 0, 0);
                }
                laptopAvailable = available;
                LaptopStatus status;
                status.available = available;
                status.reason = laptopReasonString(reason);
                status.following = snapshot.ambient.active;
                status.calibrating = snapshot.ambient.calibrating;
                status.panelPercent = snapshot.ambient.builtinPercent;
                std::lock_guard<std::mutex> settingsLock(settingsMutex);
                latestLaptop = std::move(status);
            }
        }
        // Volume keys. The hook follows the setting; the route and the monitor list follow the snapshot.
        std::vector<std::string> monitorIds;
        monitorIds.reserve(snapshot.monitors.size());
        for (const auto& monitor : snapshot.monitors) {
            monitorIds.push_back(monitor.id);
        }
        std::optional<std::string> owner;
        {
            std::lock_guard<std::mutex> lock(keyMutex);
            if (keyAccepting) {
                const bool enabled = keyWake != nullptr && snapshot.settings.hardware_keys_enabled;
                if (enabled != keyHookWanted) {
                    keyHookWanted = enabled;
                    keyHookController->setEnabled(enabled);
                }
                // The hook thread runs at normal quality, once per thread id, so a reinstall that
                // changes the thread is exempted too. Efficiency mode is never fought (controller).
                const uint32_t hookThread = keyHook->threadId();
                if (keyHook->isInstalled() && hookThread != 0 && hookThread != keyExemptThread) {
                    keyExemptThread = hookThread;
                    std::lock_guard<std::mutex> efficiencyLock(efficiencyMutex);
                    efficiencyController->registerExemptThread(hookThread);
                }
                if (monitorIds != keyMonitorIds) {
                    keyMonitorIds = monitorIds;
                    keyMonitorsDirty = true;
                }
                // Connection generations for the completion path. Our set_volume acceptances hold back
                // the router's rebase on readbacks until the matching completion arrives.
                keyConnectionGenerations.clear();
                for (const auto& monitor : snapshot.monitors) {
                    keyConnectionGenerations[monitor.id] = monitor.generation;
                }
                for (const AcceptedAction& accepted : volumeActions.onSnapshot(snapshot)) {
                    keyRouter->actionAccepted(accepted.monitorId, accepted.connectionGeneration,
                        accepted.actionGeneration);
                    if (trace) {
                        trace("keys: action accepted monitor=" + traceMonitor(accepted.monitorId) +
                            " actionGeneration=" + std::to_string(accepted.actionGeneration));
                    }
                }
                owner = keyOwner;
            }
        }
        routeKeys(snapshot, owner);

        setLanguage(snapshot.settings.locale);
        if (onFlyoutState) {
            onFlyoutState(buildFlyoutState(snapshot, *manual));
        }
    }

    // ---- Volume keys: helpers. Called from publish(), the audio owner callback and keyConsumer. ----

    // Pushes the owner's volume to the router and the route to the hook. Takes keyMutex only and
    // makes no core call, so it is safe from the snapshot thread and from audio callbacks.
    void routeKeys(const Snapshot& snapshot, const std::optional<std::string>& owner)
    {
        std::lock_guard<std::mutex> lock(keyMutex);
        if (!keyAccepting) {
            return;
        }
        const MonitorSnapshot* monitor = nullptr;
        if (owner) {
            for (const auto& candidate : snapshot.monitors) {
                if (candidate.id == *owner) {
                    monitor = &candidate;
                    break;
                }
            }
        }
        const bool hasOwner = monitor != nullptr;
        const bool volumeSupported = hasOwner && monitor->volume.supported;
        const bool displayAwake = hasOwner && monitor->displayState != "asleep";

        KeyRouteConfig config;
        config.enabled = snapshot.settings.hardware_keys_enabled;
        config.ownerMonitorId = hasOwner ? monitor->id : std::string{};
        config.step = static_cast<double>(snapshot.settings.volume_key_step);
        keyRouter->configure(config);
        keyRouterOwner = config.ownerMonitorId;

        if (hasOwner) {
            const ChannelSnapshot& volume = monitor->volume;
            VolumeObservation observation;
            observation.monitorId = monitor->id;
            observation.connectionGeneration = monitor->generation;
            observation.supported = volume.supported;
            observation.awake = displayAwake;
            // NaN tells the router the value is unknown, so it does not rebase to a guess.
            const bool known = volume.observed.has_value() || (volume.raw.has_value() && volume.max > volume.min);
            observation.percent = known ? observedPercent(volume) : std::numeric_limits<double>::quiet_NaN();
            observation.pending = std::any_of(snapshot.pendingActions.begin(), snapshot.pendingActions.end(),
                [&](const PendingAction& action) { return action.monitorId == monitor->id; });
            observation.failed = channelFailed(volume);
            keyRouter->observe(observation);
        }

        KeyRoute route;
        route.hasOwner = hasOwner;
        route.volumeSupported = volumeSupported;
        route.displayAwake = displayAwake;
        route.version = snapshot.revision;
        keyRoute.publish(route);
        if (keyWake != nullptr) {
            SetEvent(keyWake);
        }
    }

    // Audio owner callback. Runs on an audio system thread, or on the thread that calls refresh().
    void onKeyOwnerChanged(std::optional<std::string> owner)
    {
        Snapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            snapshot = latestSnapshot;
        }
        {
            std::lock_guard<std::mutex> lock(keyMutex);
            if (!keyAccepting) {
                return;
            }
            keyOwner = owner;
        }
        routeKeys(snapshot, owner);
    }

    // ---- Key router and manual controls. Each takes keyMutex and does nothing once keys are stopped.
    // Called from the manual controls without their lock held.

    void onManualTargetChanged(const std::string& monitorId, uint64_t generation, double value, uint64_t revision)
    {
        std::lock_guard<std::mutex> lock(keyMutex);
        if (!keyAccepting) {
            return;
        }
        keyRouter->manualControlChanged(monitorId, generation, value, revision);
    }

    void onManualTargetSubmitted(const std::string& monitorId, uint64_t generation, double value, uint64_t revision)
    {
        std::lock_guard<std::mutex> lock(keyMutex);
        if (!keyAccepting) {
            return;
        }
        keyRouter->manualControlAccepted(monitorId, generation, value, revision);
    }

    // DDC completion of a set_volume action, on the executor thread. Its generation is the action's,
    // so the connection generation comes from the latest snapshot.
    void onVolumeCompleted(const HardwareCompletion& completion)
    {
        std::lock_guard<std::mutex> lock(keyMutex);
        if (!keyAccepting) {
            return;
        }
        const auto found = keyConnectionGenerations.find(completion.monitorId);
        if (found == keyConnectionGenerations.end()) {
            return;
        }
        keyRouter->actionCompleted(completion.monitorId, found->second, completion.generation);
        if (trace) {
            trace("keys: action completed monitor=" + traceMonitor(completion.monitorId) +
                " actionGeneration=" + std::to_string(completion.generation) +
                " ok=" + (completion.ok ? "1" : "0"));
        }
    }

    // Device interface path for the audio owner. The snapshot carries the DisplayConfig
    // monitorDevicePath the catalog reported, so the lookup needs no catalog call.
    std::optional<std::wstring> monitorDevicePath(const std::string& monitorId)
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            for (const auto& monitor : latestSnapshot.monitors) {
                if (monitor.id == monitorId) {
                    path = monitor.connectionPath;
                    break;
                }
            }
        }
        if (path.empty()) {
            // Once per session: the owner is asked again on every monitor or default-output change.
            if (!keyPathLogged.exchange(true)) {
                diagnostics.log(LogLevel::Error,
                    "volume keys: a monitor has no device path, so its audio owner cannot be read");
            }
            return std::nullopt;
        }
        return toWide(path);
    }

    // Body of keyConsumer. Exits once keyStop is set and keyWake is signalled.
    void runKeyConsumer()
    {
        for (;;) {
            WaitForSingleObject(keyWake, INFINITE);
            if (keyStop.load()) {
                return;
            }

            // Called without keyMutex: the owner callback takes it.
            std::optional<std::vector<std::string>> monitorIds;
            {
                std::lock_guard<std::mutex> lock(keyMutex);
                if (keyMonitorsDirty) {
                    monitorIds = keyMonitorIds;
                    keyMonitorsDirty = false;
                }
            }
            if (monitorIds) {
                audioOwner->onMonitorsChanged(*monitorIds);
            }

            KeyQueueEntry entry{};
            while (keyQueue.pop(entry)) {
                telaviva::KeyEvent keyEvent;
                if (entry.vk == telaviva::kVkVolumeUp) {
                    keyEvent.key = telaviva::VolumeKey::Up;
                } else if (entry.vk == telaviva::kVkVolumeDown) {
                    keyEvent.key = telaviva::VolumeKey::Down;
                } else {
                    keyEvent.key = telaviva::VolumeKey::Mute;
                }
                keyEvent.phase = entry.isKeyDown ? telaviva::KeyPhase::Down : telaviva::KeyPhase::Up;
                keyEvent.isFineStep = entry.shiftDown;
                keyEvent.isRepeat = entry.isRepeat;
                if (trace) {
                    trace("keys: event vk=" + std::to_string(entry.vk) + " down=" + (entry.isKeyDown ? "1" : "0") +
                        " repeat=" + (entry.isRepeat ? "1" : "0") + " shift=" + (entry.shiftDown ? "1" : "0"));
                }
                std::lock_guard<std::mutex> lock(keyMutex);
                if (keyAccepting) {
                    const bool swallow = keyRouter->handleKey(keyEvent);
                    if (trace) {
                        trace("keys: router swallow=" + std::string(swallow ? "yes" : "no") + " owner=" +
                            (keyRouterOwner.empty() ? std::string("none") : traceMonitor(keyRouterOwner)));
                    }
                }
            }

            for (;;) {
                std::vector<telaviva::KeyIntent> intents;
                {
                    std::lock_guard<std::mutex> lock(keyMutex);
                    if (!keyAccepting) {
                        return;
                    }
                    intents = keyRouter->takeIntents();
                }
                if (intents.empty()) {
                    break;
                }
                for (const telaviva::KeyIntent& intent : intents) {
                    // Same per-monitor queue as the slider, so a key write and a slider drain never overlap.
                    // The router keeps its outstanding and expiry logic, so the intent is finished as taken.
                    if (trace) {
                        trace("keys: intent monitor=" + traceMonitor(intent.monitorId) + " target=" +
                            formatPercent(intent.targetVolume) + " generation=" + std::to_string(intent.generation));
                    }
                    // A key write is not reported to the router as a slider change, which would replace its own intent.
                    manual->setKeyTarget(intent.monitorId, ManualFeature::Volume, intent.targetVolume);
                    if (trace) {
                        trace("keys: setTarget returned, intent finished accepted");
                    }
                    std::lock_guard<std::mutex> lock(keyMutex);
                    keyRouter->intentFinished(intent, true);
                }
            }
        }
    }

    // Shutdown order for the volume keys: the hook (joined on destruction), then the consumer,
    // then the audio owner and its adapters. All of it runs before the bridge drains.
    void stopKeys()
    {
        {
            std::lock_guard<std::mutex> lock(keyMutex);
            keyAccepting = false;
            keyHookWanted = false;
            keyRouter->shutdown();
            // Routes no more keys: the hook is uninstalled before its thread is joined below.
            keyHookController->setEnabled(false);
        }
        keyHookController.reset();
        // Joins the hook thread, so no hook callback runs after this.
        keyHook.reset();
        keyStop = true;
        if (keyWake != nullptr) {
            SetEvent(keyWake);
        }
        if (keyConsumer.joinable()) {
            keyConsumer.join();
        }
        audioOwner.reset();
        monitorContainers.reset();
        audioEndpoints.reset();
        keyRouter.reset();
        if (keyWake != nullptr) {
            CloseHandle(keyWake);
            keyWake = nullptr;
        }
    }

    // ---- Protection brightness slider (see protectionMutex). UI thread for the queue and flush calls. ----

    // Records the newest value for the monitor and re-arms the one trailing timer.
    void queueProtectionBrightness(const std::string& monitorId, double value)
    {
        std::optional<TimerId> previous;
        {
            std::lock_guard<std::mutex> lock(protectionMutex);
            if (!protectionAccepting) {
                return;
            }
            protectionPending[monitorId] = value;
            previous = protectionTimer;
            protectionTimer.reset();
            const uint64_t token = ++protectionToken;
            const TimerId timer = scheduler.scheduleOnce(kProtectionDelay, toleranceFor(kProtectionDelay),
                [this, token] { onProtectionTimer(token); });
            if (timer != kInvalidTimerId) {
                protectionTimer = timer;
            }
        }
        if (previous.has_value()) {
            scheduler.cancel(*previous);
        }
    }

    // Writes every waiting value in one settings submission. Pass closing=true from shutdown, which
    // also stops later values from being queued. Runs on the UI thread or the scheduler thread.
    void flushProtection(bool closing)
    {
        std::optional<TimerId> timer;
        std::map<std::string, double> batch;
        {
            std::lock_guard<std::mutex> submitLock(protectionSubmitMutex);
            {
                std::lock_guard<std::mutex> lock(protectionMutex);
                batch.swap(protectionPending);
                timer = protectionTimer;
                protectionTimer.reset();
                ++protectionToken;
                if (closing) {
                    protectionAccepting = false;
                }
            }
            submitProtection(batch);
        }
        if (timer.has_value()) {
            scheduler.cancel(*timer);
        }
    }

    void onProtectionTimer(uint64_t token)
    {
        std::lock_guard<std::mutex> submitLock(protectionSubmitMutex);
        std::map<std::string, double> batch;
        {
            std::lock_guard<std::mutex> lock(protectionMutex);
            if (!protectionAccepting || token != protectionToken) {
                return;
            }
            batch.swap(protectionPending);
            protectionTimer.reset();
        }
        submitProtection(batch);
    }

    // Caller holds protectionSubmitMutex and not protectionMutex. The settings come from the latest copy.
    void submitProtection(const std::map<std::string, double>& batch)
    {
        if (batch.empty()) {
            return;
        }
        SettingsSnapshot current;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            current = latestSettings;
        }
        SettingsDraft draft = draftFromSnapshotSettings(current);
        for (const auto& [monitorId, value] : batch) {
            draft.screensaverTargets[monitorId] = value;
        }
        bridge->submit(buildSettingsPayload(draft));
    }
};

AppController::AppController(PlatformWindow& window, std::function<void(FlyoutState)> onFlyoutState,
    std::function<void()> onOpenSettings, std::function<void()> onQuit)
    : services_(std::make_unique<Services>())
{
    Services& s = *services_;
    s.onFlyoutState = std::move(onFlyoutState);
    s.onOpenSettings = std::move(onOpenSettings);
    s.onQuit = std::move(onQuit);

    // TELAVIVA_LOG_LEVEL=debug or info raises the level for this run; any other value keeps Error.
    const std::string requestedLevel = requestedLogLevel();
    const bool traceDebug = requestedLevel == "debug";
    if (traceDebug) {
        s.diagnostics.setLevel(LogLevel::Debug);
    } else if (requestedLevel == "info") {
        s.diagnostics.setLevel(LogLevel::Info);
    }
    if (traceDebug || requestedLevel == "info") {
        s.diagnostics.log(LogLevel::Error, "log level: " + requestedLevel + " (TELAVIVA_LOG_LEVEL)");
    }

    if (!s.paths.ensureCreated()) {
        s.diagnostics.log(LogLevel::Error, "app data folder could not be created");
    }

    CoreBridgeOptions options;
    options.configPath = s.paths.settingsFile().wstring();
    options.osLocale = userLocaleName();
    s.bridge = std::make_unique<CoreBridge>(options);
    if (!s.bridge->isOpen()) {
        s.diagnostics.log(LogLevel::Error, "core did not open; monitor and setting changes will not apply");
    }

    s.submitter = std::make_unique<BridgeSubmitter>(*s.bridge);
    s.manualSink = std::make_unique<CoreManualSink>(*s.bridge);
    s.executor = std::make_unique<ThreadExecutor>();
    s.ddc = std::make_unique<DdcController>(s.catalog, s.ports, *s.submitter, *s.executor, s.sleeper, s.scheduler,
        &s.manualGate);
    s.manual = std::make_unique<ManualControls>(*s.manualSink, s.scheduler, s.clock);
    // Completions can arrive only after start(), which is after this line.
    s.manualGate.open(s.manual.get());
    // Slider targets reach the key router, and volume completions reach the router's acceptance tracking.
    // Both are wired before the key consumer thread starts.
    s.manual->setObserver(&s.manualKeyBridge);
    s.manualKeyBridge.changed = [&s](const std::string& monitorId, uint64_t generation, double value,
                                    uint64_t revision) { s.onManualTargetChanged(monitorId, generation, value, revision); };
    s.manualKeyBridge.submitted = [&s](const std::string& monitorId, uint64_t generation, double value,
                                      uint64_t revision) { s.onManualTargetSubmitted(monitorId, generation, value, revision); };
    s.manualGate.setVolumeHook([&s](const HardwareCompletion& completion) { s.onVolumeCompleted(completion); });
    // Trace lines reach the log only at Debug level. Otherwise the sinks stay empty and nothing is formatted.
    // Set before any traffic: nothing calls into these controllers until start() or the bridge sink below.
    if (traceDebug) {
        s.trace = [diagnostics = &s.diagnostics](const std::string& line) { diagnostics->log(LogLevel::Debug, line); };
        s.ddc->setTrace(s.trace);
        s.manual->setTrace(s.trace);
    }
    s.launchAtLogin = std::make_unique<LaunchAtLogin>(s.runKey, executablePath());

    // ---- Laptop follow and ambient calibration. The probe is run only by the follower, which polls
    // only after a laptop is detected, so a desktop never reads the panel. ----
    s.uiWindow = window.hwnd();
    s.wmiPanel = std::make_unique<WmiPanel>();
    s.laptopEnvironment.panelBrightnessProbe = [panel = s.wmiPanel.get()] {
        return panel->readPercent() ? BrightnessReadability::Ok : BrightnessReadability::NotReadable;
    };
    s.laptopFollower = std::make_unique<LaptopPanelFollower>(*s.wmiPanel, s.laptopEnvironment, *s.submitter,
        s.scheduler, s.clock);
    s.adaptive = std::make_unique<AdaptiveBrightness>(s.powerSettings);
    s.calibrationSession = std::make_unique<CalibrationSession>(s.paths.root());
    s.calibrationPanel = std::make_unique<PanelAdapter>(*s.wmiPanel);
    s.calibrationFollow = std::make_unique<FollowPauseAdapter>(*s.laptopFollower);
    s.calibrationMonitors = std::make_unique<MonitorsAdapter>(s.settingsMutex, s.latestSnapshot, *s.manual);
    s.calibrationSettings = std::make_unique<SettingsAdapter>(s.settingsMutex, s.latestSettings, *s.bridge);
    s.calibration = std::make_unique<AmbientCalibrationController>(*s.calibrationPanel, *s.calibrationSettings,
        *s.calibrationFollow, *s.calibrationMonitors, *s.adaptive, *s.calibrationSession);

    // The unavailable message is handled here on the UI thread. The window keeps the handler, so it holds
    // the target weakly; shutdown() resets the target before the calibration goes.
    s.laptopTarget = std::make_shared<LaptopMessageTarget>();
    s.laptopTarget->onUnavailable = [&s] {
        if (s.calibration->isActive()) {
            s.calibration->end(CalibrationEndReason::BuiltInUnavailable);
        }
    };
    window.addHandler([weak = std::weak_ptr<LaptopMessageTarget>(s.laptopTarget)](UINT message, WPARAM,
                          LPARAM) -> std::optional<LRESULT> {
        if (message != kLaptopUnavailableMessage) {
            return std::nullopt;
        }
        const auto target = weak.lock();
        if (!target || !target->onUnavailable) {
            return std::nullopt;
        }
        target->onUnavailable();
        return LRESULT{ 0 };
    });

    // The DDC controller is the only target of these callbacks, and it outlives the session object.
    DdcController* ddc = s.ddc.get();
    SessionEventCallbacks events;
    // The display going off ends a calibration: the panel and the monitor would be read while nothing is shown.
    events.onDisplayState = [ddc, &s](bool displayOn) {
        ddc->onDisplayState(displayOn);
        s.laptopEnvironment.recordDisplayStatus(displayOn);
        s.laptopFollower->onDisplayStatusChanged(displayOn);
        if (!displayOn && s.calibration->isActive()) {
            s.calibration->end(CalibrationEndReason::DisplaySleep);
        }
    };
    events.onResume = [ddc] { ddc->refresh(); };
    events.onTopologyChanged = [ddc] { ddc->refresh(); };
    s.session = std::make_shared<SessionEvents>(s.scheduler, std::move(events));

    // The window keeps its handlers until it is destroyed, which can come after this
    // controller, so the handler holds the session object weakly.
    window.addHandler([weak = std::weak_ptr<SessionEvents>(s.session)](UINT message, WPARAM wParam,
                          LPARAM lParam) -> std::optional<LRESULT> {
        const auto session = weak.lock();
        if (!session) {
            return std::nullopt;
        }
        return session->handleMessage(message, wParam, lParam);
    });
    SessionEvents::registerFor(window.hwnd());

    // Presence pipeline. The callbacks below submit through the raw bridge pointer, which
    // stays valid because ~AppController destroys them before the bridge.
    CoreBridge* bridge = s.bridge.get();

    // The runtime and model ship next to the executable (packaging/windows/build-app.ps1).
    // A missing file is logged, and InferenceHost is still built; it reports itself unavailable.
    const std::filesystem::path exeDir = std::filesystem::path(executablePath()).parent_path();
    const std::filesystem::path runtimePath = exeDir / L"onnxruntime.dll";
    const std::filesystem::path modelPath = exeDir / L"models" / L"face_detection_yunet_2023mar.onnx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(runtimePath, ec)) {
        s.diagnostics.log(LogLevel::Error, "ONNX Runtime library is missing; presence inference is unavailable");
    }
    if (!std::filesystem::is_regular_file(modelPath, ec)) {
        s.diagnostics.log(LogLevel::Error, "YuNet model is missing; presence inference is unavailable");
    }
    // The core version is the installed version for the About page and the inference failure record.
    appVersion_ = telaviva::CoreBridge::coreVersion();
    if (appVersion_.empty()) {
        appVersion_ = "unknown";
    }
    AccelerationOptions acceleration;
    // The self-test frame ships next to the executable too. Absent means no frame, so acceleration stays off.
    // A present but invalid file is logged once and also means no frame.
    const std::filesystem::path selftestPath = exeDir / L"selftest-face.bgra";
    if (std::filesystem::is_regular_file(selftestPath, ec)) {
        SelftestFrameLoadResult selftest = loadSelftestFrame(selftestPath);
        if (selftest.ok()) {
            acceleration.selfTestFrame = std::move(selftest.frame);
        } else {
            s.diagnostics.log(LogLevel::Error, "self-test frame is not usable, presence inference uses CPU only: " +
                                                   selftest.error);
        }
    }
    acceleration.adapters = &s.adapters;
    acceleration.failures = &s.failures;
    acceleration.clock = &s.clock;
    acceleration.appVersion = appVersion_;
    acceleration.diagnostics = &s.diagnostics;
    s.analyzer = std::make_unique<InferenceHost>(s.inference, s.scheduler, runtimePath.wstring(),
        modelPath.wstring(), std::move(acceleration));
    // Applies the stored device before the first frame. latestSettings is the last published value, or the
    // default "auto" when nothing was published yet. Later changes come from publish().
    {
        std::string stored;
        {
            std::lock_guard<std::mutex> lock(s.settingsMutex);
            stored = s.latestSettings.inference_device;
        }
        s.applyInferenceDevice(stored);
    }
    s.camera = std::make_unique<MediaCaptureBackend>(s.clock);
    s.lifecycle = std::make_unique<CoreLifecycleApi>();
    s.driver = std::make_unique<CameraLifecycleDriver>(*s.camera, *s.analyzer, *s.submitter, *s.lifecycle,
        s.clock, s.scheduler);

    // Update check for the About page. Self-contained: it owns its HTTP client, clock and job runner.
    updateChecker_ = std::make_shared<telaviva::UpdateChecker>(
        appVersion_,
        std::make_shared<WinHttpGet>(),
        [](const std::string& installed, int status, const std::string& body) {
            return CoreBridge::evaluateUpdate(installed, status, body);
        },
        std::make_shared<SystemClock>(),
        [](std::function<void()> job) {
            static_cast<void>(winrt::Windows::System::Threading::ThreadPool::RunAsync(
                [job = std::move(job)](auto const&) { job(); }));
        });

    PowerEventCallbacks powerCallbacks;
    powerCallbacks.onPower = [bridge](std::optional<bool> onBattery) { bridge->submit(buildPower(onBattery)); };
    // The lid callback runs on the UI thread. A lid closed during calibration goes through the window
    // message, the same path the snapshot thread uses for an unavailable Laptop.
    powerCallbacks.onLid = [&s](bool open) {
        s.laptopEnvironment.recordLidState(open);
        s.laptopFollower->onLidChanged(open);
        if (!open && s.calibration->isActive()) {
            PostMessageW(s.uiWindow, kLaptopUnavailableMessage, 0, 0);
        }
    };
    s.power = std::make_shared<PowerEvents>(s.powerReader, std::move(powerCallbacks));
    window.addHandler([weak = std::weak_ptr<PowerEvents>(s.power)](UINT message, WPARAM wParam,
                          LPARAM lParam) -> std::optional<LRESULT> {
        const auto power = weak.lock();
        if (!power) {
            return std::nullopt;
        }
        return power->handleMessage(message, wParam, lParam);
    });
    PowerEvents::registerFor(window.hwnd());
    // Reports the power source once at startup; later changes arrive as messages.
    s.power->publishCurrent();

    s.screensaver = std::make_unique<ScreensaverMonitor>(s.scheduler, s.screensaverProbe,
        [bridge](ScreensaverState state) { bridge->submit(buildScreensaver(screensaverStateString(state))); });

    // ---- Efficiency mode (T101). Requested once, here on the UI thread, which is the thread the
    // flyout and Settings windows are shown on.
    s.efficiencyController = std::make_unique<EfficiencyController>(s.efficiency, GetCurrentThreadId());
    s.efficiencyController->onStartup();
    s.diagnostics.log(LogLevel::Error, s.efficiencyController->stateString());

    // ---- Volume and mute keys (T112). The hook is installed only while the setting asks for it,
    // from publish(). COM is already initialised as single-threaded on this thread (main.cpp),
    // which the endpoint adapter accepts.
    s.keyWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s.keyWake == nullptr) {
        s.diagnostics.log(LogLevel::Error, "volume keys: wake event could not be created; keys stay with Windows");
    }
    s.keyRouter = std::make_unique<KeyIntentRouter>(s.clock);
    s.keyHook = std::make_unique<Win32KeyHook>([&s](std::string line) { s.diagnostics.log(LogLevel::Error, line); });
    s.keyHookController = std::make_unique<KeyHookController>(*s.keyHook, [&s](const KeyHookEvent& event) {
        // Hook thread: route read, latch and queue push, plus one signal for a swallowed key.
        const bool swallow = s.keyPolicy.onEvent(event);
        if (swallow) {
            SetEvent(s.keyWake);
        }
        return swallow;
    });
    s.audioEndpoints = std::make_unique<Win32AudioEndpoints>();
    s.monitorContainers = std::make_unique<PnpMonitorContainers>(
        [&s](const std::string& monitorId) { return s.monitorDevicePath(monitorId); });
    s.audioOwner = std::make_unique<AudioOwner>(*s.audioEndpoints, *s.monitorContainers,
        [&s](std::optional<std::string> owner) { s.onKeyOwnerChanged(std::move(owner)); });
    // Publishes the first owner. Later changes come from the endpoint callbacks and from publish().
    s.audioOwner->refresh();
    if (s.keyWake != nullptr) {
        s.keyConsumer = std::thread([&s] { s.runKeyConsumer(); });
    }

    // Crash recovery before the sink is added: no snapshot can enable the follower until a record
    // left by a previous run has been restored or kept.
    if (!s.calibration->recoverPending()) {
        s.diagnostics.log(LogLevel::Error, "calibration recovery failed; record kept for the next launch");
    }

    // Added last: the sink uses every part above.
    s.bridge->addSink([&s](const Snapshot& snapshot) { s.publish(snapshot); });
}

AppController::~AppController()
{
    shutdown();

    Services& s = *services_;
    // Queued DDC work may still use the bridge and the controller, and every task now
    // returns early because shutdown stopped DDC. Drain it before anything else goes.
    s.executor.reset();
    // The monitor is already disabled, and publish() no longer touches it. Its destructor
    // waits for a running tick, which may submit through the bridge, so it goes before the bridge.
    s.screensaver.reset();
    // The calibration and the follower run on the UI thread, and a follower read may submit through
    // the bridge, so they go before it drains. The panel goes last, since both refer to it.
    s.calibration.reset();
    s.calibrationSettings.reset();
    s.calibrationMonitors.reset();
    s.calibrationFollow.reset();
    s.calibrationPanel.reset();
    s.laptopFollower.reset();
    s.wmiPanel.reset();
    s.calibrationSession.reset();
    s.adaptive.reset();
    // Joins the snapshot thread, which still calls ddc, manual and driver. The driver must
    // outlive this join, because publish() calls it on that thread.
    s.bridge.reset();
    // The driver is shut down, so later snapshots do nothing. It goes before the camera and
    // analyzer it refers to, and the lifecycle last.
    s.driver.reset();
    s.camera.reset();
    s.analyzer.reset();
    s.lifecycle.reset();
    s.manual.reset();
    s.ddc.reset();
    s.manualSink.reset();
    s.submitter.reset();
}

void AppController::start()
{
    Services& s = *services_;
    // A closed core never publishes a snapshot, so the flyout is told here instead.
    if (!s.bridge->isOpen() && s.onFlyoutState) {
        s.onFlyoutState(unavailableState());
    }
    s.ddc->refresh();
}

telaviva::CoreBridge* AppController::bridge()
{
    return services_->bridge.get();
}

void AppController::shutdown()
{
    Services& s = *services_;
    if (s.stopped) {
        return;
    }
    s.stopped = true;

    // Calibration ends first, while the bridge, DDC and the follower still run: it restores adaptive
    // brightness, the monitor and the Laptop panel, then deletes the record.
    if (s.calibration->isActive()) {
        s.calibration->end(CalibrationEndReason::AppQuit);
    }
    // Drops the window's route to the calibration, so a message still queued does nothing.
    s.laptopTarget.reset();

    // Volume keys stop before anything else: the hook and its consumer submit through the
    // bridge and touch the audio owner, so they must be gone before the core drains.
    s.stopKeys();

    // Input stops first. The core then drains while DDC still runs, so pending restores
    // can reach the monitors. DDC stops last.
    SessionEvents::unregisterFor();
    s.session.reset();
    PowerEvents::unregisterFor();
    s.power.reset();
    {
        // Laptop follow stops under its gate, so publish() no longer touches the follower.
        // Disabling may publish builtin_display through the bridge, which is still open here.
        std::lock_guard<std::mutex> lock(s.laptopMutex);
        s.laptopAccepting = false;
        s.laptopFollower->setFollowEnabled(false);
    }
    {
        // Disabling publishes Unknown through the bridge, which is still open here.
        std::lock_guard<std::mutex> lock(s.screensaverMutex);
        s.screensaverAccepting = false;
        s.screensaver->setEnabled(false);
    }
    // Closes the camera and cancels its timers while the core still drains.
    s.driver->shutdown();
    // DDC completions stop reaching the manual controls before those controls shut down.
    s.manualGate.close();
    s.manual->shutdown();
    // A pending protection value is written while the bridge still accepts commands.
    s.flushProtection(true);
    s.bridge->shutdown(8s);
    s.ddc->shutdown();
}

// ---- Efficiency mode (T101) ----

void AppController::onWindowVisibilityChanged(bool visible)
{
    Services& s = *services_;
    std::lock_guard<std::mutex> lock(s.efficiencyMutex);
    if (visible) {
        ++s.visibleWindows;
    } else if (s.visibleWindows > 0) {
        --s.visibleWindows;
    }
    s.efficiencyController->onWindowVisibilityChanged(s.visibleWindows);
}

void AppController::setBrightness(const std::string& monitorId, double value)
{
    services_->manual->setTarget(monitorId, ManualFeature::Brightness, value);
}

void AppController::setVolume(const std::string& monitorId, double value)
{
    services_->manual->setTarget(monitorId, ManualFeature::Volume, value);
}

void AppController::setMonitorProtected(const std::string& monitorId, bool protectedOn)
{
    // The full settings go out, as the Settings page does, so only protection_disabled changes.
    SettingsDraft draft = draftFromSnapshotSettings(currentSettings());
    draft.protectionDisabled = telaviva::withMonitorProtection(std::move(draft.protectionDisabled), monitorId,
        protectedOn);
    submit(buildSettingsPayload(draft));
}

void AppController::setProtectionBrightness(const std::string& monitorId, double value)
{
    // Ticks only store the value; the trailing timer submits the settings once.
    services_->queueProtectionBrightness(monitorId, value);
}

void AppController::onFlyoutHidden()
{
    services_->flushProtection(false);
}

void AppController::refresh()
{
    Services& s = *services_;
    s.ddc->refresh();
    if (!s.onFlyoutState) {
        return;
    }
    // The core snapshot is read directly so the flyout shows current values without waiting
    // for the next accepted revision.
    if (!s.bridge->isOpen()) {
        s.onFlyoutState(unavailableState());
        return;
    }
    s.onFlyoutState(buildFlyoutState(s.bridge->snapshot(), *s.manual));
}

void AppController::openSettings()
{
    if (services_->onOpenSettings) {
        services_->onOpenSettings();
    }
}

void AppController::quit()
{
    if (services_->onQuit) {
        services_->onQuit();
    }
}

telaviva::SettingsSnapshot AppController::currentSettings()
{
    Services& s = *services_;
    std::lock_guard<std::mutex> lock(s.settingsMutex);
    return s.latestSettings;
}

KeysState AppController::keysState()
{
    Services& s = *services_;
    if (!currentSettings().hardware_keys_enabled) {
        return KeysState::Off;
    }
    std::lock_guard<std::mutex> lock(s.keyMutex);
    if (s.keyAccepting && s.keyHook && s.keyHook->isInstalled()) {
        return KeysState::Active;
    }
    return KeysState::Unavailable;
}

telaviva::Snapshot AppController::latestSnapshot()
{
    Services& s = *services_;
    std::lock_guard<std::mutex> lock(s.settingsMutex);
    return s.latestSnapshot;
}

std::vector<telaviva::CameraInfo> AppController::cameras()
{
    Services& s = *services_;
    std::lock_guard<std::mutex> lock(s.settingsMutex);
    return s.cameraCache;
}

void AppController::refreshCameras()
{
    Services& s = *services_;
    // Enumeration keeps no shared state, so it runs without the mutex; only the cache write locks.
    // The cache never holds two entries with one id; the first entry wins here (the backend already prefers the device name).
    std::vector<telaviva::CameraInfo> found = telaviva::dedupeCameras(s.camera->enumerate(), {});
    std::lock_guard<std::mutex> lock(s.settingsMutex);
    s.cameraCache = std::move(found);
}

int AppController::submit(const std::string& json)
{
    return services_->bridge->submit(json);
}

telaviva::LaunchResult AppController::setLaunchAtLogin(bool enable)
{
    return services_->launchAtLogin->setEnabled(enable);
}

bool AppController::launchAtLoginEnabled()
{
    return services_->launchAtLogin->isEnabled();
}

// ---- Laptop and calibration (T117-T132, T164-T168) ----

LaptopStatus AppController::laptopStatus()
{
    Services& s = *services_;
    std::lock_guard<std::mutex> lock(s.settingsMutex);
    return s.latestLaptop;
}

std::vector<CurvePointView> AppController::curvePoints(const std::string& monitorId)
{
    Services& s = *services_;
    std::lock_guard<std::mutex> lock(s.settingsMutex);
    std::vector<CurvePointView> points;
    const auto found = s.latestSettings.ambientCurves.find(monitorId);
    if (found != s.latestSettings.ambientCurves.end()) {
        for (const AmbientCurvePointSnapshot& point : found->second) {
            points.push_back(CurvePointView{ static_cast<int>(point.monitor), point.builtin });
        }
    }
    return points;
}

void AppController::beginCalibration(const std::string& monitorId, int point)
{
    Services& s = *services_;
    if (!s.laptopFollower->canCalibrate()) {
        return;
    }
    s.calibration->begin(monitorId, point);
}

CalibrationView AppController::calibrationView()
{
    Services& s = *services_;
    CalibrationView view;
    view.notice = noticeKey(s.calibration->notice());
    // A copy: the controller's optional is not held across the calls below.
    const std::optional<ActiveCalibration> active = s.calibration->active();
    if (!active) {
        return view;
    }
    view.active = true;
    view.monitorId = active->monitorId;
    view.point = active->point;
    view.sliderValue = active->sliderValue;
    const SliderRange range = s.calibration->allowedRange(active->point, active->monitorId);
    view.lower = range.lower;
    view.upper = range.upper;
    view.anchorReached = s.calibration->anchorReached(active->monitorId);
    return view;
}

void AppController::setCalibrationSlider(double value)
{
    services_->calibration->setSlider(value);
}

bool AppController::confirmCalibration()
{
    return services_->calibration->confirm();
}

bool AppController::removeCalibrationPoint(const std::string& monitorId, int point)
{
    return services_->calibration->removePoint(monitorId, point);
}

void AppController::endCalibration(CalibrationEndReason reason)
{
    services_->calibration->end(reason);
}

telaviva::UpdateChecker& AppController::updateChecker()
{
    return *updateChecker_;
}

const std::string& AppController::appVersion() const
{
    return appVersion_;
}

ISettingsHost* AppController::settingsHost()
{
    return this;
}

// ---- Advanced page: inference device ----

std::vector<DeviceOption> AppController::inferenceDevices()
{
    // The catalog is cheap and creates no device, so it is read each time the page is shown.
    return buildDeviceOptions(services_->adapters.adapters());
}

InferenceReport AppController::inferenceReport()
{
    Services& s = *services_;
    if (s.analyzer) {
        return s.analyzer->report();
    }
    // No analyzer yet: report the stored choice as not decided.
    InferenceReport report;
    report.requestedId = currentSettings().inference_device;
    return report;
}

}  // namespace telaviva::host
