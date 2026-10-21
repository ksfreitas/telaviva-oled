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

/// Calibration controls under one monitor: the saved points and the add row. The active calibration runs in a dialog sheet.
private struct CalibrationPanel: View {
    @ObservedObject var calibration: AmbientCalibrationController
    /// Observed so the saved points refresh when the core snapshot changes.
    @ObservedObject var core: CoreBridge
    let monitor: CoreMonitor
    let locale: String?
    /// The monitor percent the add row will calibrate next.
    @State var addPercent = 25

    /// The monitor's saved curve; nil when it has no points yet.
    private var savedCurve: AmbientCurve? { core.snapshot.settings?.ambientCurves?[monitor.id] }
    /// Saved points in ascending monitor order, as the core keeps them.
    private var points: [AmbientCurvePoint] { savedCurve?.points ?? [] }
    /// Add is refused for a percent that is already saved and once the curve is full.
    private var canAddPoint: Bool {
        savedCurve?.point(at: addPercent) == nil && points.count < AmbientCurve.maxPoints
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(AppText.string("calibration.points.title", locale: locale))
                .font(.caption).foregroundStyle(.secondary)
            ForEach(points, id: \.monitor) { point in
                pointRow(point)
            }
            addRow
            if points.count < AmbientCurve.minPoints {
                Text(AppText.string("calibration.minPoints", locale: locale))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .onAppear { addPercent = defaultAddPercent() }
        .onChange(of: points.count) { _, _ in
            // A saved point can take the value the add row shows; move to the next free default.
            if savedCurve?.point(at: addPercent) != nil { addPercent = defaultAddPercent() }
        }
        .sheet(isPresented: Binding(
            get: { calibration.active?.monitorID == monitor.id },
            set: { shown in
                // Escape or a close from the system cancels the active calibration.
                if !shown { calibration.end(.cancelled) }
            }
        )) {
            calibrationDialog
        }
    }

    /// The dialog for the active calibration: the target percent, the help, the slider, and Done or Cancel.
    /// It reads the controller state directly, so it renders nothing once the calibration ends.
    private var calibrationDialog: some View {
        Group {
            if let active = calibration.active {
                VStack(alignment: .leading, spacing: 16) {
                    Text(String(format: AppText.string("calibration.dialog.title", locale: locale), active.point))
                        .font(.headline)
                    Text(AppText.string("calibration.help", locale: locale))
                        .font(.caption).foregroundStyle(.secondary)
                    if !calibration.anchorReached(monitorID: monitor.id) {
                        Text(AppText.string("calibration.waiting", locale: locale))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    sliderRow(active)
                    HStack {
                        Spacer()
                        Button(AppText.string("calibration.cancel", locale: locale)) { calibration.end(.cancelled) }
                            .keyboardShortcut(.cancelAction)
                        Button(AppText.string("calibration.done", locale: locale)) {
                            calibration.confirm()
                            calibration.end(.done)
                        }
                        .keyboardShortcut(.defaultAction)
                        .disabled(!calibration.anchorReached(monitorID: monitor.id))
                    }
                }
                .padding(24)
                .frame(width: 420)
            } else {
                EmptyView()
            }
        }
    }

    /// One saved point: its monitor and MacBook percents, then Calibrate (re-measure) and Remove.
    private func pointRow(_ point: AmbientCurvePoint) -> some View {
        let monitorPercent = Int(point.monitor.rounded())
        let builtinPercent = Int(point.builtin.rounded())
        let selected = calibration.active?.monitorID == monitor.id && calibration.active?.point == monitorPercent
        let rowText = String(format: AppText.string("calibration.point.row", locale: locale), monitorPercent, builtinPercent)
        let recalibrateTitle = AppText.string("calibration.recalibrate", locale: locale)
        let removeTitle = AppText.string("calibration.remove", locale: locale)
        return HStack {
            Text(rowText)
                .foregroundStyle(selected ? Color.accentColor : Color.primary)
                .accessibilityLabel(rowText)
            Spacer()
            Button(recalibrateTitle) {
                calibration.begin(monitorID: monitor.id, point: monitorPercent)
            }
            .accessibilityLabel("\(recalibrateTitle) \(monitorPercent)%")
            Button(removeTitle) {
                calibration.removePoint(monitorID: monitor.id, point: monitorPercent)
            }
            .disabled(selected)
            .accessibilityLabel("\(removeTitle) \(monitorPercent)%")
        }
        .tint(selected ? Color.accentColor : nil)
    }

    /// The add row: a stepper for the monitor percent and the Add button.
    private var addRow: some View {
        let addTitle = AppText.string("calibration.add.button", locale: locale)
        return HStack {
            Stepper(value: $addPercent, in: 1...100) {
                Text(AppText.string("calibration.add.label", locale: locale))
            }
            Text("\(addPercent)%")
                .monospacedDigit()
                .foregroundStyle(.secondary)
            Button(addTitle) {
                calibration.begin(monitorID: monitor.id, point: addPercent)
            }
            .disabled(!canAddPoint)
            .accessibilityLabel("\(addTitle) \(addPercent)%")
        }
    }

    /// The first of 25, 50, 75 and 100 not saved yet, else the first whole percent not saved.
    private func defaultAddPercent() -> Int {
        if let preferred = [25, 50, 75, 100].first(where: { savedCurve?.point(at: $0) == nil }) {
            return preferred
        }
        return (1...100).first(where: { savedCurve?.point(at: $0) == nil }) ?? 25
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
        case .protectionActive: return "calibration.notice.protectionActive"
        case .autoBrightnessUnknown: return "calibration.notice.autoUnknown"
        case .autoBrightnessNotChanged: return "calibration.notice.autoNotChanged"
        case .recoveryFailed: return "calibration.notice.recoveryFailed"
        }
    }
}
