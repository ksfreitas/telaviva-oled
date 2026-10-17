import SwiftUI
import Combine
import ServiceManagement
import AVFoundation
import AppKit

struct SettingsView: View {
    @ObservedObject var core: CoreBridge
    var requestHardwareKeyPermission: () -> Void = {
        (NSApp.delegate as? MenuBarController)?.requestHardwareKeyPermission()
    }
    @ObservedObject var ambientAvailability: AmbientAvailability = AmbientAvailability()
    var calibration: AmbientCalibrationController? = nil
    @State private var locale = "auto"
    @State private var presence = false
    @State private var alwaysOnCamera = false
    @State private var screensaver = false
    @State private var battery = true
    @State private var login = false
    @State private var hardwareKeys = false
    @State private var brightnessKeyStep = 5.0
    @State private var volumeKeyStep = 5.0
    @State private var presenceMinBrightness = 20.0
    @State private var presentInterval = 10.0
    @State private var returnInterval = 1.0
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
    /// Monitors listed in the protection section, from the latest core snapshot.
    private var connectedMonitors: [CoreMonitor] { core.snapshot.monitors ?? [] }

    private func refreshCameras() {
        cameras = AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera, .external], mediaType: .video, position: .unspecified).devices
    }

    var body: some View {
        // Tabs read and write the drafts owned here; save() and load() stay in this view.
        TabView {
            generalTab
                .tabItem { Text(AppText.string("settings.tab.general", locale: locale)) }
            keyboardTab
                .tabItem { Text(AppText.string("settings.tab.keyboard", locale: locale)) }
            AmbientBrightnessView(core: core, availability: ambientAvailability, calibration: calibration)
                .tabItem { Text(AppText.string("settings.tab.ambient", locale: locale)) }
        }
        .onAppear { refreshCameras(); load() }
        .onReceive(NotificationCenter.default.publisher(for: .cameraDeviceConnected)) { _ in refreshCameras() }
        .onReceive(NotificationCenter.default.publisher(for: .cameraDeviceDisconnected)) { _ in refreshCameras() }
        .onChange(of: core.snapshot.revision) { _, _ in if !didLoad { load() } }
        .onChange(of: locale) { _, _ in save() }
        .onChange(of: presence) { _, _ in save() }
        .onChange(of: hardwareKeys) { _, _ in save() }
        .onChange(of: alwaysOnCamera) { _, _ in save() }
        .onChange(of: screensaver) { _, _ in save() }
        .onChange(of: battery) { _, _ in save() }
        .onChange(of: presentInterval) { _, _ in save() }
        .onChange(of: returnInterval) { _, _ in save() }
        .onChange(of: brightnessKeyStep) { _, _ in save() }
        .onChange(of: volumeKeyStep) { _, _ in save() }
        .onChange(of: presenceMinBrightness) { _, _ in save() }
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
                // Threshold below which the camera stays off; editable only while presence protection is on.
                VStack(alignment: .leading, spacing: 4) {
                    LabeledContent(AppText.string("presenceMinBrightness", locale: locale)) {
                        HStack { Slider(value: $presenceMinBrightness, in: 10...90, step: 1); Text("\(Int(presenceMinBrightness)) %").monospacedDigit() }
                    }
                    Text(AppText.string("presenceMinBrightnessHelp", locale: locale))
                        .font(.caption).foregroundStyle(.secondary)
                }
                .disabled(!presence)
                Toggle(AppText.string("alwaysOnCamera", locale: locale), isOn: $alwaysOnCamera)
                    .help(AppText.string("alwaysOnCameraHelp", locale: locale))
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
                if connectedMonitors.isEmpty {
                    Text(AppText.string("protection.monitors.none", locale: locale))
                        .foregroundStyle(.secondary)
                }
                ForEach(connectedMonitors) { monitor in
                    Toggle(monitor.name, isOn: Binding(
                        get: { monitor.protected ?? true },
                        set: { setMonitorProtection(monitor.id, enabled: $0) }
                    ))
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
        alwaysOnCamera = value.always_on_camera ?? false
        screensaver = value.screensaver_protection_enabled; battery = value.suspend_presence_on_battery
        login = value.launch_at_login; presentInterval = Double(value.presence_sampling_interval_s)
        hardwareKeys = value.hardware_keys_enabled ?? false
        brightnessKeyStep = Double(value.brightness_key_step ?? 5)
        volumeKeyStep = Double(value.volume_key_step ?? 5)
        presenceMinBrightness = value.presence_min_brightness_percent ?? 20
        returnInterval = Double(value.return_sampling_interval_s)
    }

    private func save() {
        guard didLoad else { return }
        var settings = core.snapshot.settings ?? CoreSettings()
        settings.locale = locale; settings.presence_protection_enabled = presence
        settings.always_on_camera = alwaysOnCamera
        settings.screensaver_protection_enabled = screensaver; settings.suspend_presence_on_battery = battery
        settings.launch_at_login = login; settings.presence_sampling_interval_s = presentInterval
        settings.return_sampling_interval_s = returnInterval
        settings.hardware_keys_enabled = hardwareKeys
        settings.brightness_key_step = Int(brightnessKeyStep.rounded())
        settings.volume_key_step = Int(volumeKeyStep.rounded())
        settings.presence_min_brightness_percent = presenceMinBrightness
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
