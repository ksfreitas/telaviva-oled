import SwiftUI
import Combine

/// Why following the built-in brightness is unavailable. `reason` is nil while it is available; MenuBarController sets it.
final class AmbientAvailability: ObservableObject {
    @Published var reason: BuiltInUnavailableReason?
}

/// Automatic brightness tab: the follow switch, why it is unavailable, and the external monitors with their follow state.
struct AmbientBrightnessView: View {
    @ObservedObject var core: CoreBridge
    @ObservedObject var availability: AmbientAvailability
    /// Nil hides the calibration controls. Each panel observes it directly.
    var calibration: AmbientCalibrationController? = nil

    init(core: CoreBridge, availability: AmbientAvailability, calibration: AmbientCalibrationController? = nil) {
        _core = ObservedObject(wrappedValue: core)
        _availability = ObservedObject(wrappedValue: availability)
        self.calibration = calibration
    }

    /// The same value SettingsView loads into its locale draft.
    private var locale: String? { core.snapshot.settings?.locale }
    /// The switch stays disabled until the core reports a usable built-in display.
    private var isAvailable: Bool { core.snapshot.ambient?.available == true }
    private var monitors: [CoreMonitor] { core.snapshot.monitors ?? [] }

    var body: some View {
        Form {
            Section {
                Toggle(AppText.string("ambient.toggle", locale: locale), isOn: Binding(
                    get: { core.snapshot.settings?.ambientFollowEnabled ?? false },
                    set: { newValue in
                        // Direct write, like the protection toggles: only the follow field changes.
                        guard var next = core.snapshot.settings else { return }
                        next.ambientFollowEnabled = newValue
                        core.updateSettings(next)
                    }
                ))
                // Stays usable to switch off when the setting is already on.
                .disabled(!isAvailable && !(core.snapshot.settings?.ambientFollowEnabled ?? false))
                Text(AppText.string("ambient.help", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                if !isAvailable {
                    Text(AppText.string("ambient.unavailable." + (availability.reason?.rawValue ?? "unknown"), locale: locale))
                        .foregroundStyle(.secondary)
                }
            }
            Section(AppText.string("ambient.monitors.title", locale: locale)) {
                if monitors.isEmpty {
                    Text(AppText.string("ambient.monitors.none", locale: locale))
                        .foregroundStyle(.secondary)
                }
                ForEach(monitors) { monitor in
                    HStack {
                        Text(monitor.name)
                        Spacer()
                        Text(AppText.string(statusKey(monitor.follow), locale: locale))
                            .foregroundStyle(.secondary)
                    }
                    if let calibration, isAvailable, monitor.follow != "unsupported", monitor.follow != "unstable" {
                        CalibrationPanel(calibration: calibration, core: core, monitor: monitor, locale: locale)
                    }
                }
                if let calibration {
                    CalibrationNoticeView(calibration: calibration, locale: locale)
                }
            }
        }
        .formStyle(.grouped)
        .padding(20)
        .onDisappear { calibration?.end(.leftTab) }
    }

    /// A missing or unknown value reads as not calibrated.
    private func statusKey(_ follow: String?) -> String {
        switch follow ?? "uncalibrated" {
        case "eligible": return "ambient.status.eligible"
        case "unsupported": return "ambient.status.unsupported"
        case "unstable": return "ambient.status.unstable"
        default: return "ambient.status.uncalibrated"
        }
    }
}

/// Calibration controls under one monitor: the four points, and the slider and actions while one is active.
private struct CalibrationPanel: View {
    @ObservedObject var calibration: AmbientCalibrationController
    let core: CoreBridge
    let monitor: CoreMonitor
    let locale: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(AppText.string("calibration.points.title", locale: locale))
                .font(.caption).foregroundStyle(.secondary)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 64), spacing: 8)], alignment: .leading, spacing: 8) {
                ForEach(CalibrationPoint.allCases, id: \.self) { point in
                    pointButton(point)
                }
            }
            if calibration.active?.monitorID == monitor.id, let active = calibration.active {
                Text(AppText.string("calibration.help", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
                if !calibration.anchorReached(monitorID: monitor.id) {
                    Text(AppText.string("calibration.waiting", locale: locale))
                        .font(.caption).foregroundStyle(.secondary)
                }
                sliderRow(active)
                HStack {
                    Button(AppText.string("calibration.confirm", locale: locale)) { calibration.confirm() }
                        .disabled(!calibration.anchorReached(monitorID: monitor.id))
                    Button(AppText.string("calibration.done", locale: locale)) { calibration.end(.done) }
                    Button(AppText.string("calibration.cancel", locale: locale)) { calibration.end(.cancelled) }
                }
            }
        }
    }

    private func pointButton(_ point: CalibrationPoint) -> some View {
        let title = "\(point.rawValue)%"
        let calibrated = core.snapshot.settings?.ambientCurves?[monitor.id]?[point] != nil
        let selected = calibration.active?.monitorID == monitor.id && calibration.active?.point == point
        let status = AppText.string(calibrated ? "calibration.point.calibrated" : "calibration.point.pending", locale: locale)
        return Button {
            calibration.begin(monitorID: monitor.id, point: point)
        } label: {
            HStack(spacing: 4) {
                if calibrated {
                    Image(systemName: "checkmark.circle.fill")
                }
                Text(title)
            }
        }
        .buttonStyle(.bordered)
        .tint(selected ? Color.accentColor : nil)
        .accessibilityLabel("\(title), \(status)")
    }

    private func sliderRow(_ active: ActiveCalibration) -> some View {
        let range = calibration.allowedRange(for: active.point, monitorID: monitor.id)
        return HStack {
            Slider(value: Binding(
                get: { calibration.active?.sliderValue ?? 0 },
                set: { calibration.setSlider($0) }
            ), in: range) {
                Text(AppText.string("calibration.slider", locale: locale))
            }
            // Zero-width range: nothing to adjust.
            .disabled(range.upperBound <= range.lowerBound)
            Text("\(Int(active.sliderValue.rounded()))%")
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }
}

/// The controller's last notice. Shown once below the monitors, since it is not per monitor.
private struct CalibrationNoticeView: View {
    @ObservedObject var calibration: AmbientCalibrationController
    let locale: String?

    var body: some View {
        if let notice = calibration.notice {
            Text(AppText.string(noticeKey(notice), locale: locale))
                .foregroundStyle(.secondary)
        }
    }

    private func noticeKey(_ notice: CalibrationNotice) -> String {
        switch notice {
        case .builtInUnreadable: return "calibration.notice.builtInUnreadable"
        case .builtInWriteFailed: return "calibration.notice.builtInWriteFailed"
        case .builtInUnavailable: return "calibration.notice.builtInUnavailable"
        case .autoBrightnessUnknown: return "calibration.notice.autoUnknown"
        case .autoBrightnessNotChanged: return "calibration.notice.autoNotChanged"
        case .recoveryFailed: return "calibration.notice.recoveryFailed"
        }
    }
}
