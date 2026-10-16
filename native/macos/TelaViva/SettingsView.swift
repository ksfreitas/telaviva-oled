import SwiftUI
import ServiceManagement
import AVFoundation
import AppKit

struct SettingsView: View {
    @ObservedObject var core: CoreBridge
    var requestHardwareKeyPermission: () -> Void = {
        (NSApp.delegate as? MenuBarController)?.requestHardwareKeyPermission()
    }
    @State private var locale = "auto"
    @State private var presence = false
    @State private var alwaysOnCamera = false
    @State private var screensaver = false
    @State private var battery = true
    @State private var login = false
    @State private var hardwareKeys = false
    @State private var presentInterval = 10.0
    @State private var returnInterval = 1.0
    @State private var selectedCameraID: String?
    @State private var didLoad = false
    @State private var showLoginItemError = false
    @State private var loginItemError = ""
    private var cameras: [AVCaptureDevice] { AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera, .external], mediaType: .video, position: .unspecified).devices }

    var body: some View {
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
            }
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
            Section(AppText.string("availability", locale: locale)) {
                Picker(AppText.string("camera", locale: locale), selection: $selectedCameraID) {
                    Text(AppText.string("cameraPicker", locale: locale)).tag(String?.none)
                    ForEach(cameras, id: \.uniqueID) { device in Text(device.localizedName).tag(Optional(device.uniqueID)) }
                }
                .onChange(of: selectedCameraID) { _, _ in save() }
                Label(AppText.cameraInlineStatus(core.snapshot, locale: locale), systemImage: "camera")
                Text(AppText.string("cameraHelp", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                Text(AppText.string("displayHelp", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding(20)
        .onAppear(perform: load)
        .onChange(of: core.snapshot.revision) { _, _ in if !didLoad { load() } }
        .onChange(of: locale) { _, _ in save() }
        .onChange(of: presence) { _, _ in save() }
        .onChange(of: hardwareKeys) { _, _ in save() }
        .onChange(of: alwaysOnCamera) { _, _ in save() }
        .onChange(of: screensaver) { _, _ in save() }
        .onChange(of: battery) { _, _ in save() }
        .onChange(of: presentInterval) { _, _ in save() }
        .onChange(of: returnInterval) { _, _ in save() }
        .alert(AppText.string("loginItemFailureTitle", locale: locale), isPresented: $showLoginItemError) {
            Button(AppText.string("ok", locale: locale), role: .cancel) { }
        } message: {
            Text(loginItemError)
        }
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
        returnInterval = Double(value.return_sampling_interval_s)
        selectedCameraID = value.selected_camera_id
    }

    private func save() {
        guard didLoad else { return }
        var settings = core.snapshot.settings ?? CoreSettings()
        settings.locale = locale; settings.presence_protection_enabled = presence
        settings.always_on_camera = alwaysOnCamera
        settings.screensaver_protection_enabled = screensaver; settings.suspend_presence_on_battery = battery
        settings.launch_at_login = login; settings.presence_sampling_interval_s = presentInterval
        settings.return_sampling_interval_s = returnInterval; settings.selected_camera_id = selectedCameraID
        settings.hardware_keys_enabled = hardwareKeys
        core.updateSettings(settings)
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
