import Foundation
import Combine

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
    /// Monitor brightness percent (1...100) being calibrated.
    var point: Int
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

    /// Starts a session for the monitor percent `point` (1...100), or re-measures it when already saved. Ignored,
    /// without a notice, when the point is out of range or would be a new point on a curve that is already full.
    func begin(monitorID: String, point: Int) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard (1...100).contains(point) else { return }
        let curve = hooks.curve(monitorID)
        guard curve.point(at: point) != nil || curve.points.count < AmbientCurve.maxPoints else { return }
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
        hooks.setMonitorBrightness(monitorID, Double(point))
        let range = allowedRange(for: point, monitorID: monitorID)
        let initial = hooks.curve(monitorID).point(at: point)?.builtin ?? captured!.builtinLevelPercent
        let slider = clamp(initial, to: range)
        guard backend.writeLevel(slider) else {
            notice = .builtInWriteFailed
            // The monitor was already moved by this call, so it is always restored.
            finish(restoreMonitor: true)
            return
        }
        active = ActiveCalibration(monitorID: monitorID, point: point, sliderValue: slider)
    }

    /// Range of MacBook levels bounded by the nearest saved neighbors of `point` (monitor percent): the nearest point
    /// below gives the lower bound and the nearest point above gives the upper bound, 0 and 100 when none exist.
    func allowedRange(for point: Int, monitorID: String) -> ClosedRange<Double> {
        dispatchPrecondition(condition: .onQueue(.main))
        // The stored points are sorted ascending by monitor percent.
        let points = hooks.curve(monitorID).points
        let lower = points.last { $0.monitor < Double(point) }?.builtin ?? 0
        let upper = points.first { $0.monitor > Double(point) }?.builtin ?? 100
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
        return abs(observed - Double(current.point)) <= 2
    }

    /// Stores the slider value for the active point only, inserting it or replacing the saved point with the same
    /// monitor percent. The session stays active.
    func confirm() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard let current = active else { return }
        var curve = hooks.curve(current.monitorID)
        curve.set(monitor: current.point, builtin: current.sliderValue)
        hooks.saveCurve(current.monitorID, curve)
    }

    /// Removes a saved point of the monitor without starting a session. Refused for the point being calibrated now.
    func removePoint(monitorID: String, point: Int) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !(active?.monitorID == monitorID && active?.point == point) else { return }
        var curve = hooks.curve(monitorID)
        curve.remove(monitor: point)
        hooks.saveCurve(monitorID, curve)
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
