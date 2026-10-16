import SwiftUI

struct MonitorPopup: View {
    @ObservedObject var core: CoreBridge
    let openSettings: () -> Void
    let refreshDisplays: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label(AppText.string("app", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages), systemImage: "display.2")
                    .font(.headline)
                Spacer()
                Button { core.refresh(); refreshDisplays() } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(.borderless).help(AppText.string("refresh", locale: core.snapshot.settings?.locale))
            }
            Divider()
            if let monitors = core.snapshot.monitors, !monitors.isEmpty {
                ScrollView {
                    VStack(spacing: 12) { ForEach(monitors) { MonitorControls(monitor: $0, core: core) } }
                }
            } else {
                ContentUnavailableView(AppText.string("noDisplays", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages), systemImage: "display", description: Text(AppText.string("ddcUnavailable", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages)))
                    .frame(maxWidth: .infinity, minHeight: 210)
            }
            if core.snapshot.revision == nil {
                Label(AppText.string("coreUnavailable", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages), systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(.secondary)
            }
            HStack {
                Text(cameraStatus).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                Spacer()
                Button(AppText.string("settings", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages), action: openSettings)
                Button(AppText.string("quit", locale: core.snapshot.settings?.locale)) { NSApp.terminate(nil) }.keyboardShortcut("q")
            }
        }
        .padding(16)
        .frame(width: 360, height: 420)
    }

    private var cameraStatus: String {
        "\(AppText.string("camera", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages)): \(AppText.cameraStatus(core.snapshot, locale: core.snapshot.settings?.locale))"
    }
}

private struct MonitorControls: View {
    let monitor: CoreMonitor
    let core: CoreBridge
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(monitor.name).font(.subheadline.weight(.semibold))
            control(AppText.string("brightness", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages), value: monitor.brightness, kind: "set_brightness")
            control(AppText.string("volume", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages), value: monitor.volume, kind: "set_volume")
            if core.snapshot.settings?.screensaver_protection_enabled == true {
                screensaverTarget
            }
        }
        .onAppear(perform: clampScreensaverTarget)
        .onChange(of: monitor.brightness?.value) { _, _ in clampScreensaverTarget() }
        .padding(12).background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))
    }
    private var screensaverTarget: some View {
        let maximum = protectionBrightnessMaximum
        let enabled = maximum.map { $0 > 0 } ?? false
        let target = min(core.snapshot.settings?.screensaver_targets[monitor.id] ?? 0, maximum ?? 0)
        return HStack(spacing: 8) {
            rowLabel(AppText.string("protectionBrightness", locale: core.snapshot.settings?.locale, messages: core.snapshot.messages))
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
    @ViewBuilder private func control(_ title: String, value: VcpValue?, kind: String) -> some View {
        if let value, value.supported, let current = value.value {
            HStack(spacing: 8) {
                rowLabel(title)
                Slider(value: Binding(get: { current }, set: { newValue in
                    core.submit(["type": kind, "monitor_id": monitor.id, "value": Int(newValue.rounded())])
                }), in: 0...100)
                Text("\(Int(current.rounded()))%").monospacedDigit().frame(width: 42, alignment: .trailing).font(.caption)
            }
        } else {
            Label("\(title) \(AppText.string("unavailable", locale: core.snapshot.settings?.locale))", systemImage: "minus.circle")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    private var protectionBrightnessMaximum: Double? {
        guard let brightness = monitor.brightness, brightness.supported,
              let value = brightness.value else { return nil }
        return min(max(value, 0), 100)
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
