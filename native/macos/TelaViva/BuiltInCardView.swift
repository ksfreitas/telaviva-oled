import SwiftUI

/// The built-in screen's hooks for the popup card and Settings. Both closures run on main, the only queue that uses the backend.
struct BuiltInControls {
    let protection: BuiltInProtectionController
    let readLevel: () -> Double?
    let writeLevel: (Double) -> Bool
}

extension BuiltInProtectionController.Notice {
    /// Localization key of the notice line shown under the built-in card and in the Settings option.
    var textKey: String {
        switch self {
        case .autoBrightnessUnknown: return "builtin.notice.autoBrightnessUnknown"
        case .autoBrightnessNotChanged: return "builtin.notice.autoBrightnessNotChanged"
        case .restoreFailed: return "builtin.notice.restoreFailed"
        }
    }
}

/// The built-in screen's popup card: protection switch, protection brightness, brightness slider and notice. No volume.
/// The popup lists it only while the screen is protectable and available, so it is absent while the lid is closed.
struct BuiltInCardView: View {
    @ObservedObject var core: CoreBridge
    let builtIn: BuiltInControls
    @ObservedObject var protection: BuiltInProtectionController
    @State private var level = 0.0
    @State private var editing = false

    private var locale: String? { core.snapshot.settings?.locale }
    private var bp: CoreBuiltInProtection { core.snapshot.builtinProtection }
    /// Dimmed, held or unavailable: protection owns the brightness, or the screen cannot take changes.
    private var sliderLocked: Bool { bp.holding || !bp.reasons.isEmpty || !bp.available }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text(AppText.string("builtin.name", locale: locale)).font(.subheadline.weight(.semibold))
                Spacer()
                if core.snapshot.settings?.presence_protection_enabled == true || core.snapshot.settings?.screensaver_protection_enabled == true {
                    protectionToggle
                }
            }
            brightnessControl
            if core.snapshot.settings?.presence_protection_enabled == true || core.snapshot.settings?.screensaver_protection_enabled == true, bp.protected {
                protectionBrightness
            }
            BuiltInNoticeLabel(protection: protection, locale: locale)
        }
        // The level is read once on appear; later changes follow the polled level unless the user is dragging.
        .onAppear { level = builtIn.readLevel() ?? core.snapshot.ambient?.builtinPercent ?? 0 }
        .onChange(of: core.snapshot.ambient?.builtinPercent) { _, value in
            if !editing, let value { level = value }
        }
        .padding(12).background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))
    }

    private var brightnessControl: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 8) {
                rowLabel(AppText.string("brightness", locale: locale))
                Slider(value: Binding(get: {
                    level
                }, set: { value in
                    level = value
                    _ = builtIn.writeLevel(value)
                }), in: 0...100, onEditingChanged: { editing = $0 })
                .disabled(sliderLocked)
                Text("\(Int(level.rounded()))%").monospacedDigit().frame(width: 42, alignment: .trailing).font(.caption)
            }
            if sliderLocked {
                Text(AppText.string("builtin.slider.disabledWhileProtected", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var protectionToggle: some View {
        let title = AppText.string("protection.monitor.toggle", locale: locale)
        return Toggle(isOn: Binding(get: {
            bp.protected
        }, set: { enabled in
            setProtection(enabled)
        })) {
            Text(title)
        }
        .toggleStyle(.switch).controlSize(.mini).labelsHidden()
        .help(title)
        .accessibilityLabel(title)
    }

    /// Writes the full `protection_disabled` set, as the monitor rows do; the core treats the screen as unprotected while its ID is listed.
    private func setProtection(_ enabled: Bool) {
        guard var settings = core.snapshot.settings else { return }
        var disabled = Set(settings.protectionDisabled ?? [])
        if enabled { disabled.remove(BuiltInProtectionController.displayID) } else { disabled.insert(BuiltInProtectionController.displayID) }
        settings.protectionDisabled = disabled.sorted()
        core.updateSettings(settings)
    }

    /// Fixed 1...50 range, clamped like the monitor rows: protection brightness does not depend on the screen's current level.
    private var protectionBrightness: some View {
        let target = min(max(core.snapshot.settings?.screensaver_targets[BuiltInProtectionController.displayID] ?? 1, 1), 50)
        return HStack(spacing: 8) {
            rowLabel(AppText.string("protectionBrightness", locale: locale))
            Slider(value: Binding(get: {
                min(max(core.snapshot.settings?.screensaver_targets[BuiltInProtectionController.displayID] ?? 1, 1), 50)
            }, set: { value in
                guard var settings = core.snapshot.settings else { return }
                settings.screensaver_targets[BuiltInProtectionController.displayID] = min(max(value, 1), 50)
                core.updateSettings(settings)
            }), in: 1...50)
            Text("\(Int(target.rounded()))%").monospacedDigit().frame(width: 42, alignment: .trailing).font(.caption)
        }
    }

    private func rowLabel(_ title: String) -> some View {
        Text(title)
            .frame(width: 112, alignment: .trailing)
            .multilineTextAlignment(.trailing)
            .fixedSize(horizontal: false, vertical: true)
            .font(.caption)
    }
}

/// The built-in notice line. It observes the controller itself, so the Settings view need not observe it too.
struct BuiltInNoticeLabel: View {
    @ObservedObject var protection: BuiltInProtectionController
    let locale: String?

    var body: some View {
        if let notice = protection.notice {
            Label(AppText.string(notice.textKey, locale: locale), systemImage: "exclamationmark.triangle")
                .font(.caption).foregroundStyle(.orange)
        }
    }
}
