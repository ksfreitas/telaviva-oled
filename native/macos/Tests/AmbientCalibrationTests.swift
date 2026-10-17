import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("AmbientCalibrationTests: \(message)\n".utf8))
        exit(1)
    }
}

private func levelText(_ value: Double) -> String {
    value.rounded() == value ? String(Int(value)) : String(value)
}

/// Ordered log shared by the fake backend and the hooks. Each write or save appends one entry.
private final class EventLog {
    var events: [String] = []
}

private final class FakeBackend: BuiltInDisplayBackend {
    let log: EventLog
    var level: Double?
    var auto: Bool?
    /// Level writes that report failure. The attempt is still logged.
    var failedLevels: Set<Double> = []
    var setAutoSucceeds = true

    init(log: EventLog, level: Double?, auto: Bool?) {
        self.log = log
        self.level = level
        self.auto = auto
    }

    func status() -> BuiltInDisplayStatus {
        BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)
    }

    func isAsleep() -> Bool { false }

    func readLevel() -> Double? { level }

    func writeLevel(_ percent: Double) -> Bool {
        log.events.append("writeLevel \(levelText(percent))")
        guard !failedLevels.contains(percent) else { return false }
        level = percent
        return true
    }

    func autoBrightnessEnabled() -> Bool? { auto }

    func setAutoBrightness(_ enabled: Bool) -> Bool {
        log.events.append("setAuto \(enabled)")
        guard setAutoSucceeds else { return false }
        auto = enabled
        return true
    }
}

/// One temp directory, one fake built-in display and in-memory monitor and curve state.
private final class Fixture {
    let log: EventLog
    let backend: FakeBackend
    let store: CalibrationSessionStore
    let directory: URL
    /// Monitor brightness readouts. A missing key means the monitor is not connected.
    /// Writes are logged but do not change these readouts.
    var monitors: [String: Double] = ["monitor-a": 62, "monitor-b": 30]
    var curves: [String: AmbientCurve] = [:]

    init(level: Double? = 44, auto: Bool? = true) {
        let log = EventLog()
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("ambient-calibration-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        self.log = log
        self.backend = FakeBackend(log: log, level: level, auto: auto)
        self.directory = directory
        self.store = CalibrationSessionStore(directory: directory)
    }

    var sessionURL: URL { directory.appendingPathComponent("calibration-session.json") }

    var hooks: CalibrationHooks {
        CalibrationHooks(
            monitorBrightness: { id in self.monitors[id] },
            setMonitorBrightness: { id, value in self.log.events.append("setMonitor \(id) \(levelText(value))") },
            curve: { id in self.curves[id] ?? AmbientCurve(p25: nil, p50: nil, p75: nil, p100: nil) },
            saveCurve: { id, curve in
                self.log.events.append("saveCurve \(id)")
                self.curves[id] = curve
            },
            setCalibrating: { enabled in self.log.events.append("setCalibrating \(enabled)") },
            now: { "2026-10-16T12:00:00Z" }
        )
    }

    func cleanUp() {
        try? FileManager.default.removeItem(at: directory)
    }
}

/// Session expected from the default fixture: level 44, automatic brightness on, monitor A at 62.
private func capturedSession(level: Double = 44, auto: Bool? = true, monitor: Double? = 62,
                             id: String = "monitor-a") -> CalibrationSession {
    CalibrationSession(version: 1, startedAt: "2026-10-16T12:00:00Z", builtinLevelPercent: level,
                       autoBrightnessWasEnabled: auto, monitorID: id, monitorBrightnessPercent: monitor)
}

@main
@MainActor
private struct AmbientCalibrationTests {
    static func main() {
        beginCapturesStateAndWritesFirstPoint()
        secondPointKeepsOriginalCapture()
        sliderRangeIsBoundedByNeighbors()
        confirmSavesOnlyTheChangedPoint()
        everyEndReasonRestoresInOrder()
        endSkipsAutomaticBrightnessWhenItWasOff()
        endSkipsMonitorWhenItsBrightnessWasUnknown()
        beginOnAnotherMonitorRestoresFirst()
        unreadableBuiltInLevelStartsNothing()
        unknownAutomaticBrightnessStillCalibrates()
        automaticBrightnessRefusalStillCalibrates()
        failedFirstWriteRestoresAndReports()
        recoveryRestoresLeftoverSession()
        recoveryWithDisconnectedMonitorSkipsMonitor()
        recoveryWithAutomaticBrightnessOffLeavesItOff()
        recoveryReportsUnreadableFile()
        recoveryWithoutFileDoesNothing()
        sessionJSONUsesSnakeCaseKeys()
        print("Ambient calibration tests passed")
    }

    private static func makeController(_ fixture: Fixture) -> AmbientCalibrationController {
        AmbientCalibrationController(backend: fixture.backend, store: fixture.store, hooks: fixture.hooks)
    }

    private static func beginCapturesStateAndWritesFirstPoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(controller.isActive, "first point starts a session")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p25, sliderValue: 62),
               "slider starts at the monitor's current brightness when no point is stored")
        expect(fixture.store.load() == .session(capturedSession()),
               "session file holds the captured state with version 1")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "writeLevel 25", "setMonitor monitor-a 62"],
               "first point runs in the specified order")
        expect(controller.notice == nil, "a clean begin reports no notice")
    }

    private static func secondPointKeepsOriginalCapture() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p25: nil, p50: 45, p75: nil, p100: nil)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        fixture.log.events.removeAll()
        controller.begin(monitorID: "monitor-a", point: .p50)
        expect(fixture.log.events == ["writeLevel 50", "setMonitor monitor-a 45"],
               "second point only moves the built-in level and shows a new slider")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p50, sliderValue: 45),
               "slider starts at the stored value for the point")
        expect(fixture.store.load() == .session(capturedSession()),
               "second point keeps the original capture at level 44")
    }

    private static func sliderRangeIsBoundedByNeighbors() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p25: 30, p50: nil, p75: 70, p100: nil)
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: .p50, monitorID: "monitor-a") == 30.0...70.0,
               "p50 is bounded by the calibrated neighbors p25 and p75")
        expect(controller.allowedRange(for: .p25, monitorID: "monitor-a") == 0.0...70.0,
               "p25 is bounded by 0 and the next calibrated point")
        expect(controller.allowedRange(for: .p100, monitorID: "monitor-a") == 70.0...100.0,
               "p100 is bounded by the previous calibrated point and 100")
        expect(controller.allowedRange(for: .p50, monitorID: "monitor-b") == 0.0...100.0,
               "an uncalibrated monitor has the full range")
        controller.begin(monitorID: "monitor-a", point: .p50)
        fixture.log.events.removeAll()
        controller.setSlider(10)
        expect(controller.active?.sliderValue == 30, "slider below the range is clamped to the lower neighbor")
        controller.setSlider(90)
        expect(controller.active?.sliderValue == 70, "slider above the range is clamped to the upper neighbor")
        controller.setSlider(50)
        expect(controller.active?.sliderValue == 50, "slider inside the range is kept")
        expect(fixture.log.events == ["setMonitor monitor-a 30", "setMonitor monitor-a 70", "setMonitor monitor-a 50"],
               "only clamped values reach the monitor")
    }

    private static func confirmSavesOnlyTheChangedPoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p25: 20, p50: nil, p75: 70, p100: nil)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p50)
        controller.setSlider(40)
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == AmbientCurve(p25: 20, p50: 40, p75: 70, p100: nil),
               "confirm stores the new point and preserves the others")
        expect(saved?[.p50] == 40, "the saved curve reads the new point through the subscript")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p50, sliderValue: 40),
               "session stays active after confirm")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func everyEndReasonRestoresInOrder() {
        for reason in CalibrationEndReason.allCases {
            let fixture = Fixture()
            defer { fixture.cleanUp() }
            let controller = makeController(fixture)
            controller.begin(monitorID: "monitor-a", point: .p25)
            fixture.log.events.removeAll()
            controller.end(reason)
            expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false"],
                   "\(reason) restores level, automatic brightness, monitor, then calibrating")
            expect(fixture.store.load() == .none, "\(reason) deletes the session file")
            expect(!controller.isActive && controller.active == nil, "\(reason) closes the session")
        }
    }

    private static func endSkipsAutomaticBrightnessWhenItWasOff() {
        let fixture = Fixture(auto: false)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        fixture.log.events.removeAll()
        controller.end(.done)
        expect(fixture.log.events == ["writeLevel 44", "setMonitor monitor-a 62", "setCalibrating false"],
               "automatic brightness that was off is not switched on")
    }

    private static func endSkipsMonitorWhenItsBrightnessWasUnknown() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.monitors["monitor-a"] = nil
        fixture.curves["monitor-a"] = AmbientCurve(p25: 20, p50: nil, p75: nil, p100: nil)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(fixture.store.load() == .session(capturedSession(monitor: nil)),
               "unknown monitor brightness is saved as nil")
        fixture.log.events.removeAll()
        controller.end(.done)
        expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setCalibrating false"],
               "a nil monitor brightness is never written back")
    }

    private static func beginOnAnotherMonitorRestoresFirst() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        fixture.log.events.removeAll()
        controller.begin(monitorID: "monitor-b", point: .p50)
        expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false",
                                      "setCalibrating true", "setAuto false", "writeLevel 50", "setMonitor monitor-b 30"],
               "monitor A is fully restored before monitor B begins")
        expect(fixture.store.load() == .session(capturedSession(monitor: 30, id: "monitor-b")),
               "the saved session now describes monitor B")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-b", point: .p50, sliderValue: 30),
               "active calibration moves to monitor B")
    }

    private static func unreadableBuiltInLevelStartsNothing() {
        let fixture = Fixture(level: nil)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(!controller.isActive && controller.active == nil, "an unreadable built-in level starts no session")
        expect(controller.notice == .builtInUnreadable, "the unreadable level is reported")
        expect(fixture.store.load() == .none, "no session file is written")
        let nonCalibrating = fixture.log.events.filter { !$0.hasPrefix("setCalibrating") }
        expect(nonCalibrating.isEmpty, "no backend or monitor writes happen")
        expect((fixture.log.events.last ?? "setCalibrating false") == "setCalibrating false",
               "calibrating is not left on")
    }

    private static func unknownAutomaticBrightnessStillCalibrates() {
        let fixture = Fixture(auto: nil)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(controller.isActive, "calibration starts when automatic brightness is unknown")
        expect(controller.notice == .autoBrightnessUnknown, "unknown automatic brightness is reported")
        expect(fixture.log.events.contains("writeLevel 25"), "the built-in level is still set")
        expect(fixture.store.load() == .session(capturedSession(auto: nil)),
               "the session records automatic brightness as unknown")
        fixture.log.events.removeAll()
        controller.end(.done)
        expect(!fixture.log.events.contains("setAuto true"), "unknown automatic brightness is never switched on at restore")
    }

    private static func automaticBrightnessRefusalStillCalibrates() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.backend.setAutoSucceeds = false
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(controller.isActive, "calibration starts when automatic brightness cannot be changed")
        expect(controller.notice == .autoBrightnessNotChanged, "a refused automatic brightness change is reported")
        expect(fixture.log.events.contains("writeLevel 25"), "the built-in level is still set")
        expect(fixture.store.load() == .session(capturedSession()),
               "the session still records automatic brightness as on")
    }

    private static func failedFirstWriteRestoresAndReports() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.backend.failedLevels = [25]
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(!controller.isActive && controller.active == nil, "a failed built-in write ends the session")
        expect(controller.notice == .builtInWriteFailed, "the failed write is reported")
        expect(fixture.store.load() == .none, "the session file is deleted")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "writeLevel 25",
                                      "writeLevel 44", "setAuto true", "setCalibrating false"],
               "the state changed before the failure is restored")
        expect(fixture.backend.level == 44 && fixture.backend.auto == true,
               "the built-in display is back at its captured state")
    }

    private static func recoveryRestoresLeftoverSession() {
        let fixture = Fixture(level: 30, auto: false)
        defer { fixture.cleanUp() }
        let saved = fixture.store.save(capturedSession())
        expect(saved, "leftover session is written")
        let controller = makeController(fixture)
        controller.recoverIfNeeded()
        expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false"],
               "recovery restores the saved state in order")
        expect(fixture.store.load() == .none, "recovery deletes the session file")
        expect(!controller.isActive, "recovery leaves no active session")
    }

    private static func recoveryWithDisconnectedMonitorSkipsMonitor() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.monitors["monitor-a"] = nil
        let saved = fixture.store.save(capturedSession())
        expect(saved, "leftover session is written")
        let controller = makeController(fixture)
        controller.recoverIfNeeded()
        expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setCalibrating false"],
               "a disconnected monitor is not written during recovery")
        expect(fixture.store.load() == .none, "recovery deletes the session file")
    }

    private static func recoveryWithAutomaticBrightnessOffLeavesItOff() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let saved = fixture.store.save(capturedSession(auto: false))
        expect(saved, "leftover session with automatic brightness off is written")
        let controller = makeController(fixture)
        controller.recoverIfNeeded()
        expect(fixture.log.events == ["writeLevel 44", "setMonitor monitor-a 62", "setCalibrating false"],
               "automatic brightness that was off is not switched on during recovery")
        expect(fixture.store.load() == .none, "recovery deletes the session file")
    }

    private static func recoveryReportsUnreadableFile() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let written = (try? Data("not a session".utf8).write(to: fixture.sessionURL)) != nil
        expect(written, "garbage bytes are written to the session path")
        let controller = makeController(fixture)
        controller.recoverIfNeeded()
        let restores = fixture.log.events.filter { !$0.hasPrefix("setCalibrating") }
        expect(restores.isEmpty, "an unreadable session restores no levels or monitors")
        expect(fixture.store.load() == .none, "the unreadable file is deleted")
        expect(controller.notice == .recoveryFailed, "the failed recovery is reported")
    }

    private static func recoveryWithoutFileDoesNothing() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.recoverIfNeeded()
        expect(fixture.log.events.isEmpty, "no leftover session means no events")
        expect(controller.notice == nil, "no leftover session reports nothing")
    }

    private static func sessionJSONUsesSnakeCaseKeys() {
        let session = capturedSession()
        guard let data = try? JSONEncoder().encode(session),
              let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            expect(false, "session encodes as a JSON object")
            return
        }
        let expectedKeys: Set<String> = ["version", "started_at", "builtin_level_percent",
                                         "auto_brightness_was_enabled", "monitor_id", "monitor_brightness_percent"]
        expect(Set(object.keys) == expectedKeys, "session JSON uses exactly the snake_case contract keys")
        let decoded = try? JSONDecoder().decode(CalibrationSession.self, from: data)
        expect(decoded == session, "session JSON round-trips")
        let contract = Data(#"{"version":1,"started_at":"2026-10-16T12:00:00Z","builtin_level_percent":44,"auto_brightness_was_enabled":true,"monitor_id":"monitor-a","monitor_brightness_percent":62}"#.utf8)
        expect((try? JSONDecoder().decode(CalibrationSession.self, from: contract)) == session,
               "the contract example decodes into the same session")
    }
}
