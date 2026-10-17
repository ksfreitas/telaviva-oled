import Foundation
import Combine

/// Calibration points on the monitor brightness axis, as percents. The five lowest are optional refinements.
enum CalibrationPoint: Int, CaseIterable {
    case p1 = 1, p5 = 5, p10 = 10, p15 = 15, p20 = 20, p25 = 25, p50 = 50, p75 = 75, p100 = 100
}

extension AmbientCurve {
    /// The stored values are MacBook levels (percent), not monitor brightness.
    subscript(point: CalibrationPoint) -> Double? {
        get {
            switch point {
            case .p1: return p1
            case .p5: return p5
            case .p10: return p10
            case .p15: return p15
            case .p20: return p20
            case .p25: return p25
            case .p50: return p50
            case .p75: return p75
            case .p100: return p100
            }
        }
        set {
            switch point {
            case .p1: p1 = newValue
            case .p5: p5 = newValue
            case .p10: p10 = newValue
            case .p15: p15 = newValue
            case .p20: p20 = newValue
            case .p25: p25 = newValue
            case .p50: p50 = newValue
            case .p75: p75 = newValue
            case .p100: p100 = newValue
            }
        }
    }
}

/// Captured state written to `calibration-session.json` for crash recovery (contracts.md).
struct CalibrationSession: Codable, Equatable {
    var version: Int
    var startedAt: String
    var builtinLevelPercent: Double
    var autoBrightnessWasEnabled: Bool?
    var monitorID: String
    var monitorBrightnessPercent: Double?

    enum CodingKeys: String, CodingKey {
        case version
        case startedAt = "started_at"
        case builtinLevelPercent = "builtin_level_percent"
        case autoBrightnessWasEnabled = "auto_brightness_was_enabled"
        case monitorID = "monitor_id"
        case monitorBrightnessPercent = "monitor_brightness_percent"
    }
}

/// Reads and writes the single calibration session file.
final class CalibrationSessionStore {
    enum Loaded: Equatable {
        case none
        case session(CalibrationSession)
        case unreadable
    }

    private let directory: URL
    private let fileURL: URL

    init(directory: URL) {
        self.directory = directory
        self.fileURL = directory.appendingPathComponent("calibration-session.json")
    }

    /// Writes the file atomically. Returns false when the directory or the file cannot be written.
    func save(_ session: CalibrationSession) -> Bool {
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(session)
            try data.write(to: fileURL, options: .atomic)
            return true
        } catch {
            return false
        }
    }

    func load() -> Loaded {
        guard FileManager.default.fileExists(atPath: fileURL.path) else { return .none }
        guard let data = try? Data(contentsOf: fileURL),
              let session = try? JSONDecoder().decode(CalibrationSession.self, from: data) else {
            return .unreadable
        }
        return .session(session)
    }

    func delete() {
        try? FileManager.default.removeItem(at: fileURL)
    }
}

/// Side effects the controller needs from the rest of the shell and the core.
struct CalibrationHooks {
    var monitorBrightness: (String) -> Double?
    var setMonitorBrightness: (String, Double) -> Void
    var curve: (String) -> AmbientCurve
    var saveCurve: (String, AmbientCurve) -> Void
    var setCalibrating: (Bool) -> Void
    var now: () -> String
}

/// Reportable problems shown in the calibration tab.
enum CalibrationNotice: Equatable {
    case builtInUnreadable
    case builtInWriteFailed
    /// The built-in screen is off or the lid is closed, so calibration cannot run.
    case builtInUnavailable
    case autoBrightnessUnknown
    case autoBrightnessNotChanged
    /// Also used when the session file cannot be saved at the start of a session.
    case recoveryFailed
}

enum CalibrationEndReason: CaseIterable {
    case done
    case cancelled
    case leftTab
    case windowClosed
    case appQuit
    case displaySleep
    case builtInUnavailable
}

struct ActiveCalibration: Equatable {
    var monitorID: String
    var point: CalibrationPoint
    var sliderValue: Double
}

/// Main-thread state machine for one calibration session. It deliberately uses a runtime
/// main-queue precondition, as ManualMonitorControls does, instead of @MainActor.
final class AmbientCalibrationController: ObservableObject {
    @Published private(set) var active: ActiveCalibration?
    @Published private(set) var notice: CalibrationNotice?

    var isActive: Bool { active != nil }

    private let backend: BuiltInDisplayBackend
    private let store: CalibrationSessionStore
    private let hooks: CalibrationHooks
    /// State captured when the session started. Nil when no session is running.
    private var captured: CalibrationSession?

    init(backend: BuiltInDisplayBackend, store: CalibrationSessionStore, hooks: CalibrationHooks) {
        self.backend = backend
        self.store = store
        self.hooks = hooks
    }

    func begin(monitorID: String, point: CalibrationPoint) {
        dispatchPrecondition(condition: .onQueue(.main))
        notice = nil
        // Checked live: the core snapshot can lag a display sleep or a closed lid.
        guard backend.status().available, !backend.isAsleep() else {
            notice = .builtInUnavailable
            return
        }
        if let session = captured, session.monitorID != monitorID {
            end(.cancelled)
        }
        if captured == nil {
            guard let level = backend.readLevel() else {
                notice = .builtInUnreadable
                return
            }
            let auto = backend.autoBrightnessEnabled()
            let session = CalibrationSession(version: 1, startedAt: hooks.now(), builtinLevelPercent: level,
                                             autoBrightnessWasEnabled: auto, monitorID: monitorID,
                                             monitorBrightnessPercent: hooks.monitorBrightness(monitorID))
            guard store.save(session) else {
                notice = .recoveryFailed
                return
            }
            captured = session
            hooks.setCalibrating(true)
            if auto == true {
                if !backend.setAutoBrightness(false) { notice = .autoBrightnessNotChanged }
            } else if auto == nil {
                notice = .autoBrightnessUnknown
            }
        }
        // The point is a monitor level: move the monitor first, then set the MacBook slider.
        hooks.setMonitorBrightness(monitorID, Double(point.rawValue))
        let range = allowedRange(for: point, monitorID: monitorID)
        let initial = hooks.curve(monitorID)[point] ?? captured!.builtinLevelPercent
        let slider = clamp(initial, to: range)
        guard backend.writeLevel(slider) else {
            notice = .builtInWriteFailed
            // The monitor was already moved by this call, so it is always restored.
            finish(restoreMonitor: true)
            return
        }
        active = ActiveCalibration(monitorID: monitorID, point: point, sliderValue: slider)
    }

    /// Range of MacBook levels bounded by the nearest calibrated neighbors: 0 below and 100 above when none exist.
    func allowedRange(for point: CalibrationPoint, monitorID: String) -> ClosedRange<Double> {
        dispatchPrecondition(condition: .onQueue(.main))
        let curve = hooks.curve(monitorID)
        let lowerPoints = CalibrationPoint.allCases.filter { $0.rawValue < point.rawValue }
        let upperPoints = CalibrationPoint.allCases.filter { $0.rawValue > point.rawValue }
        let lower = lowerPoints.reversed().compactMap { curve[$0] }.first ?? 0
        let upper = upperPoints.compactMap { curve[$0] }.first ?? 100
        return min(lower, upper)...upper
    }

    func setSlider(_ value: Double) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard var current = active else { return }
        let clamped = clamp(value, to: allowedRange(for: current.point, monitorID: current.monitorID))
        guard backend.writeLevel(clamped) else {
            notice = .builtInWriteFailed
            finish(restoreMonitor: true)
            return
        }
        current.sliderValue = clamped
        active = current
    }

    /// True once the monitor reports the active point's level, within 2 points. Save waits for it.
    func anchorReached(monitorID: String) -> Bool {
        dispatchPrecondition(condition: .onQueue(.main))
        guard let current = active, current.monitorID == monitorID,
              let observed = hooks.monitorBrightness(monitorID) else {
            return false
        }
        return abs(observed - Double(current.point.rawValue)) <= 2
    }

    /// Stores the slider value for the active point only. The session stays active.
    func confirm() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard let current = active else { return }
        var curve = hooks.curve(current.monitorID)
        curve[current.point] = current.sliderValue
        hooks.saveCurve(current.monitorID, curve)
    }

    func end(_ reason: CalibrationEndReason) {
        dispatchPrecondition(condition: .onQueue(.main))
        if reason == .builtInUnavailable, captured != nil {
            notice = .builtInUnavailable
        }
        finish(restoreMonitor: true)
    }

    /// Restores a session left behind by a crash or force quit, then deletes its file.
    func recoverIfNeeded() {
        dispatchPrecondition(condition: .onQueue(.main))
        switch store.load() {
        case .none:
            return
        case .unreadable:
            store.delete()
            notice = .recoveryFailed
        case .session(let session):
            restore(session, restoreMonitor: hooks.monitorBrightness(session.monitorID) != nil)
        }
    }

    private func finish(restoreMonitor: Bool) {
        guard let session = captured else { return }
        restore(session, restoreMonitor: restoreMonitor)
        captured = nil
        active = nil
    }

    /// Order matters: built-in level, automatic brightness, monitor, then the calibrating flag.
    /// A failing step does not stop the following steps.
    private func restore(_ session: CalibrationSession, restoreMonitor: Bool) {
        _ = backend.writeLevel(session.builtinLevelPercent)
        if session.autoBrightnessWasEnabled == true {
            _ = backend.setAutoBrightness(true)
        }
        if restoreMonitor, let original = session.monitorBrightnessPercent {
            hooks.setMonitorBrightness(session.monitorID, original)
        }
        hooks.setCalibrating(false)
        store.delete()
    }

    private func clamp(_ value: Double, to range: ClosedRange<Double>) -> Double {
        min(max(value, range.lowerBound), range.upperBound)
    }
}
