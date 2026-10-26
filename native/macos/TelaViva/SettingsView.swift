import SwiftUI
import Combine
import ServiceManagement
import AVFoundation
import AppKit

struct SettingsView: View {
    @ObservedObject var core: CoreBridge
    /// Tab selection shared with the menus; menus set it before showing the window.
    @ObservedObject var navigation: SettingsNavigation
    let updateChecker: UpdateChecker
    let installedVersion: String
    var requestHardwareKeyPermission: () -> Void = {
        (NSApp.delegate as? MenuBarController)?.requestHardwareKeyPermission()
    }
    var requestKeepAwakePermission: () -> Void = {
        (NSApp.delegate as? MenuBarController)?.requestKeepAwakePermission()
    }
    @ObservedObject var ambientAvailability: AmbientAvailability = AmbientAvailability()
    @ObservedObject var inferenceReport: InferenceDeviceSelection.Report
    var calibration: AmbientCalibrationController? = nil
    /// Nil leaves the built-in rows out. The Settings scene in App.swift passes `appDelegate.builtInControls`.
    var builtIn: BuiltInControls? = nil
    @State private var locale = "auto"
    @State private var presence = false
    @State private var alwaysOnCamera = true
    @State private var screensaver = false
    @State private var keepAwakeWithPresence = false
    @State private var battery = true
    @State private var login = false
    @State private var hardwareKeys = false
    @State private var brightnessKeyStep = 5.0
    @State private var volumeKeyStep = 5.0
    /// The threshold the slider shows: the monitored display's own value, synced from the snapshot, never the global draft.
    @State private var presenceMinBrightness = 20.0
    @State private var presentInterval = 10.0
    @State private var returnInterval = 1.0
    @State private var inferenceDevice = "auto"
    @State private var cameras: [AVCaptureDevice] = []
    @State private var didLoad = false
    @State private var showLoginItemError = false
    @State private var loginItemError = ""
    /// The camera the app uses now: the first remembered device that is connected, or nil for the system default.
    private var effectiveCameraID: String? {
        resolveCamera(history: core.snapshot.settings?.camera_history ?? [], connected: Set(cameras.map(\.uniqueID)))
    }
    /// The setter runs only for a user choice, never for a programmatic change, so a fallback is never persisted.
    private var cameraSelection: Binding<String?> {
        Binding(get: { effectiveCameraID }, set: { core.selectCamera($0) })
    }
    /// The monitored display the core resolved, or nil for Automatic. The setter runs only for a user choice.
    private var monitoredDisplaySelection: Binding<String?> {
        Binding(
            get: { core.snapshot.monitoredDisplay.automatic ? nil : core.snapshot.monitoredDisplay.id },
            set: { core.selectMonitoredDisplay($0) }
        )
    }
    /// Monitors listed in the protection section, from the latest core snapshot.
    private var connectedMonitors: [CoreMonitor] { core.snapshot.monitors ?? [] }
    /// The built-in row is listed only while the screen is protectable and available, like the popup card.
    private var showsBuiltInProtection: Bool {
        let bp = core.snapshot.builtinProtection
        return builtIn != nil && bp.protectable && bp.available
    }

    private func refreshCameras() {
        cameras = AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera, .external], mediaType: .video, position: .unspecified).devices
    }

    private var settingsTabs: some View {
        // Tabs read and write the drafts owned here; save() and load() stay in this view.
        TabView(selection: $navigation.tab) {
            generalTab
                .tabItem { Text(AppText.string("settings.tab.general", locale: locale)) }
                .tag(SettingsTab.general)
            keyboardTab
                .tabItem { Text(AppText.string("settings.tab.keyboard", locale: locale)) }
                .tag(SettingsTab.keyboard)
            AmbientBrightnessView(core: core, availability: ambientAvailability, calibration: calibration)
                .tabItem { Text(AppText.string("settings.tab.ambient", locale: locale)) }
                .tag(SettingsTab.ambient)
            advancedTab
                .tabItem { Text(AppText.string("settings.tab.advanced", locale: locale)) }
                .tag(SettingsTab.advanced)
            AboutView(installedVersion: installedVersion, locale: locale, checker: updateChecker)
                .tabItem { Text(AppText.string("settings.tab.about", locale: locale)) }
                .tag(SettingsTab.about)
        }
    }

    private var settingsLifecycle: some View {
        settingsTabs
        .onAppear { refreshCameras(); load() }
        .onReceive(NotificationCenter.default.publisher(for: .cameraDeviceConnected)) { _ in refreshCameras() }
        .onReceive(NotificationCenter.default.publisher(for: .cameraDeviceDisconnected)) { _ in refreshCameras() }
        .onChange(of: core.snapshot.revision) { _, _ in if !didLoad { load() } }
        .onChange(of: core.snapshot.monitoredDisplay.id) { _, _ in syncPresenceMinBrightness() }
    }

    private var settingUpdates: some View {
        settingsLifecycle
        .onChange(of: locale) { _, _ in save() }
        .onChange(of: presence) { _, _ in save() }
        .onChange(of: hardwareKeys) { _, _ in save() }
        .onChange(of: alwaysOnCamera) { _, _ in save() }
        .onChange(of: screensaver) { _, _ in save() }
        .onChange(of: keepAwakeWithPresence) { _, _ in save() }
        .onChange(of: battery) { _, _ in save() }
        .onChange(of: presentInterval) { _, _ in save() }
        .onChange(of: returnInterval) { _, _ in save() }
        .onChange(of: brightnessKeyStep) { _, _ in save() }
        .onChange(of: volumeKeyStep) { _, _ in save() }
        .onChange(of: inferenceDevice) { _, _ in save() }
    }

    var body: some View {
        settingUpdates
        .alert(AppText.string("loginItemFailureTitle", locale: locale), isPresented: $showLoginItemError) {
            Button(AppText.string("ok", locale: locale), role: .cancel) { }
        } message: {
            Text(loginItemError)
        }
    }

    /// General tab: language and login, OLED protection, and camera availability.
    private var generalTab: some View {
        Form {
            Section(AppText.string("general", locale: locale)) {
                Picker(AppText.string("language", locale: locale), selection: $locale) {
                    Text(AppText.string("systemLanguage", locale: locale)).tag("auto")
                    Text(AppText.string("english", locale: locale)).tag("en-US")
                    Text(AppText.string("portuguese", locale: locale)).tag("pt-BR")
                }
                Toggle(AppText.string("launch", locale: locale), isOn: $login)
                    .onChange(of: login) { oldValue, enabled in
                        guard didLoad else { return }
                        let storedValue = core.snapshot.settings?.launch_at_login ?? oldValue
                        guard enabled != storedValue else { return }
                        if let error = LoginItemController.setEnabled(enabled) {
                            login = oldValue
                            loginItemError = "\(AppText.string("loginItemFailure", locale: locale)) (\(error.localizedDescription))"
                            showLoginItemError = true
                        } else {
                            save()
                        }
                    }
            }
            Section(AppText.string("protection", locale: locale)) {
                Toggle(AppText.string("presence", locale: locale), isOn: $presence)
                    .help(AppText.string("presenceHelp", locale: locale))
                Toggle(AppText.string("keepAwake.toggle", locale: locale), isOn: $keepAwakeWithPresence)
                    .help(AppText.string("keepAwake.help", locale: locale))
                Text(AppText.string("keepAwake.monitoring", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                Text(AppText.string(core.keepAwakeStatusKey, locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                if core.keepAwakeStatusKey == "keepAwake.status.permission" {
                    HStack {
                        Button(AppText.string("keepAwake.requestPermission", locale: locale), action: requestKeepAwakePermission)
                        Button(AppText.string("keepAwake.openPrivacy", locale: locale)) {
                            if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
                                NSWorkspace.shared.open(url)
                            }
                        }
                    }
                    .controlSize(.small)
                }
                // Threshold below which the camera stays off; editable only while presence protection is on.
                VStack(alignment: .leading, spacing: 4) {
                    LabeledContent(AppText.string("presenceMinBrightness", locale: locale)) {
                        HStack { Slider(value: presenceMinBrightnessSelection, in: 0...90, step: 1); Text("\(Int(presenceMinBrightness)) %").monospacedDigit() }
                    }
                    if let name = monitoredDisplayName {
                        Text(AppText.string("presenceMinBrightness.forDisplay", locale: locale)
                            .replacingOccurrences(of: "{0}", with: name))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Text(AppText.string("presenceMinBrightnessHelp", locale: locale))
                        .font(.caption).foregroundStyle(.secondary)
                }
                .disabled(!presence)
                Picker(AppText.string("monitored.title", locale: locale), selection: monitoredDisplaySelection) {
                    Text(AppText.string("monitored.automatic", locale: locale)).tag(String?.none)
                    ForEach(connectedMonitors) { monitor in Text(monitor.name).tag(Optional(monitor.id)) }
                    if core.snapshot.builtinProtection.protectable && core.snapshot.builtinProtection.available {
                        Text(AppText.string("builtin.name", locale: locale)).tag(Optional(BuiltInProtectionController.displayID))
                    }
                }
                Text(AppText.string("monitored.help", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                Toggle(AppText.string("saver", locale: locale), isOn: $screensaver)
                Toggle(AppText.string("battery", locale: locale), isOn: $battery)
                LabeledContent(AppText.string("check", locale: locale)) {
                    HStack { Slider(value: $presentInterval, in: 5...300, step: 1); Text("\(Int(presentInterval)) s").monospacedDigit() }
                }
                LabeledContent(AppText.string("return", locale: locale)) {
                    HStack { Slider(value: $returnInterval, in: 0.25...5, step: 0.25); Text(String(format: "%.2g s", returnInterval)).monospacedDigit() }
                }
                VStack(alignment: .leading, spacing: 4) {
                    Text(AppText.string("protection.monitors.title", locale: locale))
                        .font(.subheadline.weight(.semibold))
                    Text(AppText.string("protection.monitors.help", locale: locale))
                        .font(.caption).foregroundStyle(.secondary)
                }
                if connectedMonitors.isEmpty && !showsBuiltInProtection {
                    Text(AppText.string("protection.monitors.none", locale: locale))
                        .foregroundStyle(.secondary)
                }
                ForEach(connectedMonitors) { monitor in
                    Toggle(monitor.name, isOn: Binding(
                        get: { monitor.protected ?? true },
                        set: { setMonitorProtection(monitor.id, enabled: $0) }
                    ))
                }
                if let builtIn, showsBuiltInProtection {
                    Toggle(AppText.string("builtin.name", locale: locale), isOn: Binding(
                        get: { core.snapshot.builtinProtection.protected },
                        set: { setMonitorProtection(BuiltInProtectionController.displayID, enabled: $0) }
                    ))
                    .help(AppText.string("builtin.protection.toggle.help", locale: locale))
                    // The option only matters while protection dims the built-in screen, so it is locked while protection is off.
                    VStack(alignment: .leading, spacing: 4) {
                        Toggle(AppText.string("builtin.protection.option.title", locale: locale), isOn: Binding(
                            get: { core.snapshot.settings?.builtinAutoBrightnessOffOnProtection ?? false },
                            set: { setBuiltInAutoBrightnessOption($0) }
                        ))
                        .disabled(!core.snapshot.builtinProtection.protected)
                        Text(AppText.string("builtin.protection.option.help", locale: locale))
                            .font(.caption).foregroundStyle(.secondary)
                        BuiltInNoticeLabel(protection: builtIn.protection, locale: locale)
                    }
                }
            }
            Section(AppText.string("availability", locale: locale)) {
                Picker(AppText.string("camera", locale: locale), selection: cameraSelection) {
                    Text(AppText.string("cameraPicker", locale: locale)).tag(String?.none)
                    ForEach(cameras, id: \.uniqueID) { device in Text(device.localizedName).tag(Optional(device.uniqueID)) }
                }
                Label(AppText.cameraInlineStatus(core.snapshot, locale: locale), systemImage: "camera")
                Text(AppText.string("cameraHelp", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                Text(AppText.string("displayHelp", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }

    /// Advanced tab: the camera option (keeping the camera on between checks, with its warning), then the CoreML
    /// compute-unit preference and the current inference route.
    private var advancedTab: some View {
        Form {
            Section(AppText.string("camera", locale: locale)) {
                Toggle(AppText.string("alwaysOnCamera", locale: locale), isOn: $alwaysOnCamera)
                    .help(AppText.string("alwaysOnCameraHelp", locale: locale))
                Text(AppText.string("alwaysOnCameraWarning", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
            }
            InferenceSettingsView(selection: $inferenceDevice, report: inferenceReport, locale: locale)
        }
        .formStyle(.grouped)
        .padding(20)
    }

    /// Keyboard tab: hardware keys, step sizes, and the permissions they need.
    private var keyboardTab: some View {
        Form {
            Section(AppText.string("hardwareKeys.title", locale: locale)) {
                Toggle(AppText.string("hardwareKeys.toggle", locale: locale), isOn: Binding(
                    get: { hardwareKeys },
                    set: { enabled in
                        hardwareKeys = enabled
                        if enabled && didLoad { requestHardwareKeyPermission() }
                    }
                ))
                    .help(AppText.string("hardwareKeys.help", locale: locale))
                Text(AppText.string(core.hardwareKeyStatusKey, locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                LabeledContent(AppText.string("hardwareKeys.brightnessStep", locale: locale)) {
                    HStack { Slider(value: $brightnessKeyStep, in: 1...15, step: 1); Text("\(Int(brightnessKeyStep)) pt").monospacedDigit() }
                }
                .disabled(!hardwareKeys)
                LabeledContent(AppText.string("hardwareKeys.volumeStep", locale: locale)) {
                    HStack { Slider(value: $volumeKeyStep, in: 1...15, step: 1); Text("\(Int(volumeKeyStep)) pt").monospacedDigit() }
                }
                .disabled(!hardwareKeys)
                if hardwareKeys {
                    permissionRow(
                        title: AppText.string("hardwareKeys.accessibility", locale: locale),
                        granted: core.hardwareKeyPermissions.accessibility,
                        requestKey: "hardwareKeys.requestPermission",
                        request: { (NSApp.delegate as? MenuBarController)?.requestAccessibilityPermission() },
                        openKey: "hardwareKeys.openAccessibility",
                        openURL: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")
                    permissionRow(
                        title: AppText.string("hardwareKeys.inputMonitoring", locale: locale),
                        granted: core.hardwareKeyPermissions.inputMonitoring,
                        requestKey: "hardwareKeys.requestInputMonitoring",
                        request: { (NSApp.delegate as? MenuBarController)?.requestInputMonitoringPermission() },
                        openKey: "hardwareKeys.openInputMonitoring",
                        openURL: "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent")
                }
            }
        }
        .formStyle(.grouped)
        .padding(20)
    }

    /// One permission row: the state on top, and request and open-settings buttons disabled once granted.
    private func permissionRow(title: String, granted: Bool, requestKey: String, request: @escaping () -> Void,
                               openKey: String, openURL: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Label(title, systemImage: granted ? "checkmark.circle.fill" : "xmark.circle")
                    .foregroundStyle(granted ? Color.green : Color.secondary)
                Text(AppText.string(granted ? "hardwareKeys.granted" : "hardwareKeys.notGranted", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
            }
            HStack {
                Button(AppText.string(requestKey, locale: locale), action: request)
                    .disabled(granted)
                Button(AppText.string(openKey, locale: locale)) {
                    if let url = URL(string: openURL) { NSWorkspace.shared.open(url) }
                }
                .disabled(granted)
            }
            .controlSize(.small)
        }
    }

    private func load() {
        guard let value = core.snapshot.settings else { return }
        didLoad = true
        locale = value.locale; presence = value.presence_protection_enabled
        alwaysOnCamera = value.always_on_camera ?? true
        screensaver = value.screensaver_protection_enabled; battery = value.suspend_presence_on_battery
        keepAwakeWithPresence = value.keep_awake_with_presence ?? false
        login = value.launch_at_login; presentInterval = Double(value.presence_sampling_interval_s)
        hardwareKeys = value.hardware_keys_enabled ?? false
        brightnessKeyStep = Double(value.brightness_key_step ?? 5)
        volumeKeyStep = Double(value.volume_key_step ?? 5)
        presenceMinBrightness = storedPresenceMinBrightness(value)
        returnInterval = Double(value.return_sampling_interval_s)
        inferenceDevice = value.inference_device ?? "auto"
    }

    private func save() {
        guard didLoad else { return }
        var settings = core.snapshot.settings ?? CoreSettings()
        settings.locale = locale; settings.presence_protection_enabled = presence
        settings.always_on_camera = alwaysOnCamera
        settings.screensaver_protection_enabled = screensaver; settings.suspend_presence_on_battery = battery
        settings.keep_awake_with_presence = keepAwakeWithPresence
        settings.launch_at_login = login; settings.presence_sampling_interval_s = presentInterval
        settings.return_sampling_interval_s = returnInterval
        settings.hardware_keys_enabled = hardwareKeys
        settings.brightness_key_step = Int(brightnessKeyStep.rounded())
        settings.volume_key_step = Int(volumeKeyStep.rounded())
        // The presence threshold is not saved here: the shown value may belong to the monitored display, so
        // writePresenceMinBrightness sends it. save() keeps the stored threshold from the latest snapshot.
        settings.inference_device = inferenceDevice
        core.updateSettings(settings)
    }

    /// Writes one monitor's protection opt-out directly, with no draft: save() keeps this field from the latest snapshot.
    private func setMonitorProtection(_ monitorID: String, enabled: Bool) {
        guard var next = core.snapshot.settings else { return }
        var disabled = Set(next.protectionDisabled ?? [])
        if enabled { disabled.remove(monitorID) } else { disabled.insert(monitorID) }
        next.protectionDisabled = disabled.sorted()
        core.updateSettings(next)
    }

    /// The threshold the slider shows: the monitored display's own value, or the global one when no display resolves
    /// or the display has no entry yet.
    private func storedPresenceMinBrightness(_ settings: CoreSettings) -> Double {
        if let id = core.snapshot.monitoredDisplay.id, let value = settings.presenceMinBrightnessByDisplay?[id] {
            return value
        }
        return settings.presence_min_brightness_percent ?? 20
    }

    /// Re-reads the shown threshold from the latest snapshot, so a change of monitored display shows that display's value.
    private func syncPresenceMinBrightness() {
        guard let settings = core.snapshot.settings else { return }
        presenceMinBrightness = storedPresenceMinBrightness(settings)
    }

    /// The name of the monitored display for the threshold caption, or nil when none resolves.
    private var monitoredDisplayName: String? {
        guard let id = core.snapshot.monitoredDisplay.id else { return nil }
        if id == BuiltInProtectionController.displayID { return AppText.string("builtin.name", locale: locale) }
        return connectedMonitors.first(where: { $0.id == id })?.name
    }

    /// The slider's binding. SwiftUI calls the setter only for a user move, so a sync from the snapshot never writes.
    private var presenceMinBrightnessSelection: Binding<Double> {
        Binding(get: { presenceMinBrightness }, set: { writePresenceMinBrightness($0) })
    }

    /// Writes the threshold of the monitored display, or the global value when none resolves. Direct, like the protection
    /// toggles, so save() never sends the threshold and a per-display value cannot land in the global field.
    private func writePresenceMinBrightness(_ value: Double) {
        presenceMinBrightness = value
        guard var next = core.snapshot.settings else { return }
        if let id = core.snapshot.monitoredDisplay.id {
            var byDisplay = next.presenceMinBrightnessByDisplay ?? [:]
            byDisplay[id] = value
            next.presenceMinBrightnessByDisplay = byDisplay
        } else {
            next.presence_min_brightness_percent = value
        }
        core.updateSettings(next)
    }

    /// Writes the built-in automatic-brightness option directly, with no draft: save() keeps this field from the latest snapshot.
    private func setBuiltInAutoBrightnessOption(_ enabled: Bool) {
        guard var next = core.snapshot.settings else { return }
        next.builtinAutoBrightnessOffOnProtection = enabled
        core.updateSettings(next)
    }
}

private struct InferenceSettingsView: View {
    @Binding var selection: String
    @ObservedObject var report: InferenceDeviceSelection.Report
    let locale: String

    /// Only the section: the Advanced tab's Form supplies the grouping, formatting and padding.
    var body: some View {
        Section(AppText.string("inference.title", locale: locale)) {
            Picker(AppText.string("inference.title", locale: locale), selection: $selection) {
                ForEach(InferenceDeviceSelection.options) { option in
                    Text(AppText.string(option.labelKey, locale: locale)).tag(option.id)
                }
                if !InferenceDeviceSelection.options.contains(where: { $0.id == selection }) {
                    Text(AppText.inferenceUnavailableLabel(id: selection, locale: locale))
                        .tag(selection)
                }
            }
            Text(AppText.string("inference.help", locale: locale))
                .font(.caption).foregroundStyle(.secondary)
            Text(AppText.string("inference.discreteHelp", locale: locale))
                .font(.caption).foregroundStyle(.secondary)
            if report.decided {
                let active = AppText.inferenceActiveLabel(id: report.value.activeId,
                                                         fallback: report.value.activeLabel, locale: locale)
                Text(AppText.string("inference.inUse", locale: locale)
                    .replacingOccurrences(of: "{0}", with: active))
            } else {
                Text(AppText.string("inference.notDecided", locale: locale))
            }
            if let noticeKey = inferenceNoticeKey {
                Label(AppText.string(noticeKey, locale: locale), systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }
        }
    }

    private var inferenceNoticeKey: String? {
        switch report.value.state {
        case .unavailable: return "inference.notice.unavailable"
        case .failed: return "inference.notice.failed"
        case .ok, .autoCPU: return nil
        }
    }
}

enum LoginItemController {
    static func setEnabled(_ enabled: Bool) -> Error? {
        do {
            if enabled { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
            return nil
        } catch {
            NSLog("TelaViva: launch-at-login update failed: %@", error.localizedDescription)
            return error
        }
    }
}
