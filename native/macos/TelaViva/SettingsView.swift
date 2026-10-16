import SwiftUI
import ServiceManagement
import AVFoundation

struct SettingsView: View {
    @ObservedObject var core: CoreBridge
    @State private var locale = "auto"
    @State private var presence = false
    @State private var screensaver = false
    @State private var battery = true
    @State private var login = false
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
                Toggle(AppText.string("saver", locale: locale), isOn: $screensaver)
                Toggle(AppText.string("battery", locale: locale), isOn: $battery)
                LabeledContent(AppText.string("check", locale: locale)) {
                    HStack { Slider(value: $presentInterval, in: 5...300, step: 1); Text("\(Int(presentInterval)) s").monospacedDigit() }
                }
                LabeledContent(AppText.string("return", locale: locale)) {
                    HStack { Slider(value: $returnInterval, in: 0.25...5, step: 0.25); Text(String(format: "%.2g s", returnInterval)).monospacedDigit() }
                }
            }
            Section(AppText.string("availability", locale: locale)) {
                Picker(AppText.string("camera", locale: locale), selection: $selectedCameraID) {
                    Text(AppText.string("cameraPicker", locale: locale)).tag(String?.none)
                    ForEach(cameras, id: \.uniqueID) { device in Text(device.localizedName).tag(Optional(device.uniqueID)) }
                }
                .onChange(of: selectedCameraID) { _, _ in save() }
                Label("\(AppText.string("camera", locale: locale)): \(AppText.cameraStatus(core.snapshot, locale: locale))", systemImage: "camera")
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

    private func load() {
        guard let value = core.snapshot.settings else { return }
        didLoad = true
        locale = value.locale; presence = value.presence_protection_enabled
        screensaver = value.screensaver_protection_enabled; battery = value.suspend_presence_on_battery
        login = value.launch_at_login; presentInterval = Double(value.presence_sampling_interval_s)
        returnInterval = Double(value.return_sampling_interval_s)
        selectedCameraID = value.selected_camera_id
    }

    private func save() {
        guard didLoad else { return }
        var settings = core.snapshot.settings ?? CoreSettings()
        settings.locale = locale; settings.presence_protection_enabled = presence
        settings.screensaver_protection_enabled = screensaver; settings.suspend_presence_on_battery = battery
        settings.launch_at_login = login; settings.presence_sampling_interval_s = presentInterval
        settings.return_sampling_interval_s = returnInterval; settings.selected_camera_id = selectedCameraID
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
