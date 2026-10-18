import SwiftUI

struct MonitorPopup: View {
    @ObservedObject var core: CoreBridge
    @ObservedObject var controls: ManualMonitorControls
    let openSettings: () -> Void
    let openAbout: () -> Void
    let refreshDisplays: () -> Void
    @State private var cardHeights: [String: CGFloat] = [:]

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label(AppText.string("app", locale: core.snapshot.settings?.locale), systemImage: "display.2")
                    .font(.headline)
                Spacer()
                Button { core.refresh(); refreshDisplays() } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(.borderless).help(AppText.string("refresh", locale: core.snapshot.settings?.locale))
            }
            Divider()
            if let monitors = core.snapshot.monitors, !monitors.isEmpty {
                ScrollView {
                    VStack(spacing: 12) {
                        ForEach(monitors) { MonitorControls(monitor: $0, core: core, controls: controls).reportPopupCardHeight(id: $0.id) }
                    }
                }
                .frame(height: PopupLayout.listHeight(
                    cardHeights: monitors.map { cardHeights[$0.id] ?? PopupLayout.estimatedCardHeight },
                    spacing: 12,
                    screenCap: PopupLayout.listCap(visibleScreenHeight: NSScreen.main?.visibleFrame.height ?? 800)))
                .onPreferenceChange(PopupCardHeightsKey.self) { cardHeights = $0 }
            } else {
                ContentUnavailableView(AppText.string("noDisplays", locale: core.snapshot.settings?.locale), systemImage: "display", description: Text(AppText.string("ddcUnavailable", locale: core.snapshot.settings?.locale)))
                    .frame(maxWidth: .infinity, minHeight: 210)
            }
            if core.snapshot.revision == nil {
                Label(AppText.string("coreUnavailable", locale: core.snapshot.settings?.locale), systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(.secondary)
            }
            HStack {
                Text(cameraStatus).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                Spacer()
                Button(AppText.string("settings", locale: core.snapshot.settings?.locale), action: openSettings)
                Button(AppText.string("menu.about", locale: core.snapshot.settings?.locale), action: openAbout)
                Button(AppText.string("quit", locale: core.snapshot.settings?.locale)) { NSApp.terminate(nil) }.keyboardShortcut("q")
            }
        }
        .padding(16)
        .frame(width: 360)
    }

    private var cameraStatus: String {
        "\(AppText.string("camera", locale: core.snapshot.settings?.locale)): \(AppText.cameraStatus(core.snapshot, locale: core.snapshot.settings?.locale))"
    }
}

private struct MonitorControls: View {
    let monitor: CoreMonitor
    let core: CoreBridge
    @ObservedObject var controls: ManualMonitorControls

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text(monitor.name).font(.subheadline.weight(.semibold))
                Spacer()
                if core.snapshot.settings?.presence_protection_enabled == true || core.snapshot.settings?.screensaver_protection_enabled == true {
                    protectionToggle
                }
            }
            if !isFollowed {
                control(AppText.string("brightness", locale: core.snapshot.settings?.locale), value: monitor.brightness, feature: .brightness)
            }
            control(AppText.string("volume", locale: core.snapshot.settings?.locale), value: monitor.volume, feature: .volume)
            if core.snapshot.settings?.screensaver_protection_enabled == true, monitor.protected != false {
                screensaverTarget
            }
        }
        .onAppear(perform: clampScreensaverTarget)
        .onChange(of: monitor.brightness?.value) { _, _ in clampScreensaverTarget() }
        .padding(12).background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))
    }

    /// Followed monitors are driven by the ambient sensor, so the card hides the manual brightness control.
    private var isFollowed: Bool {
        core.snapshot.ambient?.active == true && monitor.follow == "eligible"
    }

    private var screensaverTarget: some View {
        let maximum = protectionBrightnessMaximum
        let enabled = maximum.map { $0 > 0 } ?? false
        let target = min(core.snapshot.settings?.screensaver_targets[monitor.id] ?? 0, maximum ?? 0)
        return HStack(spacing: 8) {
            rowLabel(AppText.string("protectionBrightness", locale: core.snapshot.settings?.locale))
            Slider(value: Binding(get: {
                min(core.snapshot.settings?.screensaver_targets[monitor.id] ?? 0, maximum ?? 0)
            }, set: { value in
                guard var settings = core.snapshot.settings else { return }
                settings.screensaver_targets[monitor.id] = min(max(value, 0), maximum ?? 0)
                core.updateSettings(settings)
            }), in: 0...(maximum.map { $0 > 0 ? $0 : 1 } ?? 1))
            .disabled(!enabled)
            Text("\(Int(target.rounded()))%").monospacedDigit().frame(width: 42, alignment: .trailing).font(.caption)
        }
    }

    private var protectionToggle: some View {
        let title = AppText.string("protection.monitor.toggle", locale: core.snapshot.settings?.locale)
        return Toggle(isOn: Binding(get: {
            monitor.protected ?? true
        }, set: { enabled in
            setProtection(enabled)
        })) {
            Text(title)
        }
        .toggleStyle(.switch).controlSize(.mini).labelsHidden()
        .help(title)
        .accessibilityLabel(title)
    }

    /// Writes the full `protection_disabled` set; the core treats a monitor as unprotected while its ID is listed.
    private func setProtection(_ enabled: Bool) {
        guard var settings = core.snapshot.settings else { return }
        var disabled = Set(settings.protectionDisabled ?? [])
        if enabled { disabled.remove(monitor.id) } else { disabled.insert(monitor.id) }
        settings.protectionDisabled = disabled.sorted()
        core.updateSettings(settings)
    }

    @ViewBuilder private func control(_ title: String, value: VcpValue?, feature: HardwareKeyFeature) -> some View {
        if let value, value.supported, let confirmed = value.value, monitor.generation != nil {
            let shown = controls.displayValue(monitor: monitor, feature: feature) ?? confirmed
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    rowLabel(title)
                    Slider(value: Binding(get: {
                        controls.displayValue(monitor: monitor, feature: feature) ?? confirmed
                    }, set: { newValue in
                        controls.setValue(newValue, monitor: monitor, feature: feature)
                    }), in: 0...100, onEditingChanged: { editing in
                        controls.setEditing(editing, monitor: monitor, feature: feature)
                    })
                    Text("\(Int(shown.rounded()))%").monospacedDigit().frame(width: 42, alignment: .trailing).font(.caption)
                }
                if controls.status(monitor: monitor, feature: feature) == .failed {
                    Text(AppText.string("controlFailed", locale: core.snapshot.settings?.locale))
                        .font(.system(size: 9)).foregroundStyle(Color.red)
                        .frame(maxWidth: .infinity, alignment: .trailing)
                }
            }
        } else {
            Label("\(title) \(AppText.string("unavailable", locale: core.snapshot.settings?.locale))", systemImage: "minus.circle")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var protectionBrightnessMaximum: Double? {
        guard let brightness = monitor.brightness, brightness.supported,
              let value = brightness.value else { return nil }
        let confirmed = min(max(value, 0), 100)
        // A followed monitor has no brightness control, so any leftover draft is ignored.
        guard !isFollowed, let draft = controls.displayValue(monitor: monitor, feature: .brightness) else { return confirmed }
        return min(confirmed, max(0, draft))
    }

    private func clampScreensaverTarget() {
        guard let maximum = protectionBrightnessMaximum,
              var settings = core.snapshot.settings else { return }
        let existing = settings.screensaver_targets[monitor.id] ?? 0
        let clamped = min(max(existing, 0), maximum)
        guard existing != clamped else { return }
        settings.screensaver_targets[monitor.id] = clamped
        core.updateSettings(settings)
    }

    private func rowLabel(_ title: String) -> some View {
        Text(title)
            .frame(width: 112, alignment: .trailing)
            .multilineTextAlignment(.trailing)
            .fixedSize(horizontal: false, vertical: true)
            .font(.caption)
    }
}
