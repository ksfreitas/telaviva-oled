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
    /// Reported by `status()`. Defaults to an available built-in screen.
    var statusValue = BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)
    /// Reported by `isAsleep()`. Defaults to awake.
    var asleep = false

    init(log: EventLog, level: Double?, auto: Bool?) {
        self.log = log
        self.level = level
        self.auto = auto
    }

    func status() -> BuiltInDisplayStatus { statusValue }

    func isAsleep() -> Bool { asleep }

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
        initialSliderFallsBackToTheCapturedBuiltInLevel()
        lowPointsStartASessionAtTheirMonitorLevel()
        p1StartsASessionAtMonitorOne()
        sliderRangeIsBoundedByNeighbors()
        sliderRangeIsBoundedByLowPointNeighbors()
        sliderRangeIsBoundedByP1Neighbors()
        confirmSavesOnlyTheChangedPoint()
        confirmSavesALowPointOnly()
        confirmSavesOnlyTheP1Point()
        anchorReachedFollowsTheMonitorLevel()
        anchorReachedUsesTheLowPointLevel()
        anchorReachedUsesTheP1Level()
        everyEndReasonRestoresInOrder()
        endSkipsAutomaticBrightnessWhenItWasOff()
        endSkipsMonitorWhenItsBrightnessWasUnknown()
        beginOnAnotherMonitorRestoresFirst()
        unreadableBuiltInLevelStartsNothing()
        beginIsRefusedWhileTheBuiltInScreenIsUnavailable()
        beginIsRefusedWhileTheBuiltInScreenIsAsleep()
        unknownAutomaticBrightnessStillCalibrates()
        automaticBrightnessRefusalStillCalibrates()
        failedFirstWriteRestoresAndReports()
        failedSliderWriteEndsTheSessionAndRestores()
        endBuiltInUnavailableReportsTheNoticeWhenASessionWasRunning()
        endBuiltInUnavailableWithoutASessionSetsNoNotice()
        otherEndReasonsLeaveNoNotice()
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
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p25, sliderValue: 44),
               "the MacBook slider starts at the captured built-in level when no point is stored")
        expect(fixture.store.load() == .session(capturedSession()),
               "session file holds the captured state with version 1")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 25", "writeLevel 44"],
               "first point moves the monitor to the point, then writes the MacBook slider, in the specified order")
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
        expect(fixture.log.events == ["setMonitor monitor-a 50", "writeLevel 45"],
               "second point moves the monitor to the new point and sets the MacBook to the stored value")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p50, sliderValue: 45),
               "the MacBook slider starts at the stored value for the point")
        expect(fixture.store.load() == .session(capturedSession()),
               "second point keeps the original capture at level 44")
    }

    private static func initialSliderFallsBackToTheCapturedBuiltInLevel() {
        let fixture = Fixture(level: 37)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p75)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p75, sliderValue: 37),
               "with no stored point the slider starts at the captured built-in level, not the monitor brightness")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 75", "writeLevel 37"],
               "the captured built-in level is written as the initial slider")
    }

    private static func lowPointsStartASessionAtTheirMonitorLevel() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // A stored p10 of 8 caps the p5 range at 8, so the captured built-in level 44 is clamped to it.
        fixture.curves["monitor-a"] = AmbientCurve(p10: 8)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p5)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p5, sliderValue: 8),
               "the p5 slider starts at the captured built-in level clamped to the range 0...8")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 5", "writeLevel 8"],
               "p5 moves the monitor to 5, then writes the clamped MacBook level")
        fixture.log.events.removeAll()
        // p20 is bounded below by p10 and has no upper neighbor, so the captured level 44 is inside its range.
        controller.begin(monitorID: "monitor-a", point: .p20)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p20, sliderValue: 44),
               "the p20 slider starts at the captured built-in level when it is inside the range")
        expect(fixture.log.events == ["setMonitor monitor-a 20", "writeLevel 44"],
               "p20 moves the monitor to 20, then writes the MacBook level")
        expect(fixture.store.load() == .session(capturedSession()), "the session keeps the original capture for p20")
    }

    private static func p1StartsASessionAtMonitorOne() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p1)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p1, sliderValue: 44),
               "the p1 slider starts at the captured built-in level when no point is stored")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 1", "writeLevel 44"],
               "p1 moves the monitor to 1, then writes the MacBook level")
        expect(fixture.store.load() == .session(capturedSession()), "the session keeps the original capture for p1")
    }

    private static func sliderRangeIsBoundedByNeighbors() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p25: 30, p50: nil, p75: 70, p100: nil)
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: .p50, monitorID: "monitor-a") == 30.0...70.0,
               "p50 is bounded by the calibrated MacBook levels of p25 and p75")
        expect(controller.allowedRange(for: .p25, monitorID: "monitor-a") == 0.0...70.0,
               "p25 is bounded by 0 and the next calibrated MacBook level")
        expect(controller.allowedRange(for: .p100, monitorID: "monitor-a") == 70.0...100.0,
               "p100 is bounded by the previous calibrated MacBook level and 100")
        expect(controller.allowedRange(for: .p50, monitorID: "monitor-b") == 0.0...100.0,
               "an uncalibrated monitor has the full MacBook range")
        controller.begin(monitorID: "monitor-a", point: .p50)
        fixture.log.events.removeAll()
        controller.setSlider(10)
        expect(controller.active?.sliderValue == 30, "slider below the range is clamped to the lower neighbor")
        controller.setSlider(90)
        expect(controller.active?.sliderValue == 70, "slider above the range is clamped to the upper neighbor")
        controller.setSlider(50)
        expect(controller.active?.sliderValue == 50, "slider inside the range is kept")
        expect(fixture.log.events == ["writeLevel 30", "writeLevel 70", "writeLevel 50"],
               "only clamped values reach the MacBook, and the monitor is not moved by the slider")
    }

    private static func sliderRangeIsBoundedByLowPointNeighbors() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p10: 8, p20: 16)
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: .p15, monitorID: "monitor-a") == 8.0...16.0,
               "p15 is bounded by the calibrated MacBook levels of p10 and p20")
        expect(controller.allowedRange(for: .p5, monitorID: "monitor-a") == 0.0...8.0,
               "p5 is bounded by 0 and the next calibrated MacBook level, p10")
        fixture.curves["monitor-a"] = AmbientCurve(p20: 16, p50: 45)
        expect(controller.allowedRange(for: .p25, monitorID: "monitor-a") == 16.0...45.0,
               "p25 is bounded by the calibrated MacBook levels of p20 and p50")
    }

    private static func sliderRangeIsBoundedByP1Neighbors() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p1: 2, p10: 12)
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: .p5, monitorID: "monitor-a") == 2.0...12.0,
               "p5 is bounded by the calibrated MacBook levels of p1 and p10")
        fixture.curves["monitor-a"] = AmbientCurve(p5: 6)
        expect(controller.allowedRange(for: .p1, monitorID: "monitor-a") == 0.0...6.0,
               "p1 is bounded by 0 and the next calibrated MacBook level, p5")
        fixture.curves["monitor-a"] = AmbientCurve(p10: 9)
        expect(controller.allowedRange(for: .p1, monitorID: "monitor-a") == 0.0...9.0,
               "p1 is bounded by 0 and the nearest calibrated MacBook level above it when p5 is absent")
    }

    private static func confirmSavesOnlyTheChangedPoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = AmbientCurve(p25: 20, p50: nil, p75: 70, p100: nil)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p50)
        fixture.log.events.removeAll()
        controller.setSlider(40)
        expect(fixture.log.events == ["writeLevel 40"], "the slider writes the MacBook level and not the monitor")
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == AmbientCurve(p25: 20, p50: 40, p75: 70, p100: nil),
               "confirm stores the MacBook level for the point and preserves the others")
        expect(saved?[.p50] == 40, "the saved curve reads the new point through the subscript")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p50, sliderValue: 40),
               "session stays active after confirm")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func confirmSavesALowPointOnly() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // The stored p5 and p20 bound the p10 range at 2...16, and the other points must survive the save.
        fixture.curves["monitor-a"] = AmbientCurve(p5: 2, p20: 16, p25: 20, p75: 70)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p10)
        fixture.log.events.removeAll()
        controller.setSlider(12)
        expect(fixture.log.events == ["writeLevel 12"], "the slider writes the MacBook level and not the monitor")
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == AmbientCurve(p5: 2, p10: 12, p20: 16, p25: 20, p50: nil, p75: 70, p100: nil),
               "confirm stores the MacBook level for p10 and preserves every other point")
        expect(saved?[.p10] == 12, "the saved curve reads the new low point through the subscript")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p10, sliderValue: 12),
               "session stays active after confirm at p10")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func confirmSavesOnlyTheP1Point() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // The stored p5 bounds the p1 range at 0...6, and the other points must survive the save.
        fixture.curves["monitor-a"] = AmbientCurve(p5: 6, p10: 9, p25: 20)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p1)
        fixture.log.events.removeAll()
        controller.setSlider(1)
        expect(fixture.log.events == ["writeLevel 1"], "the slider writes the MacBook level and not the monitor")
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == AmbientCurve(p1: 1, p5: 6, p10: 9, p25: 20),
               "confirm stores the MacBook level for p1 and preserves every other point")
        expect(saved?[.p1] == 1, "the saved curve reads the new point through the subscript")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: .p1, sliderValue: 1),
               "session stays active after confirm at p1")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func anchorReachedFollowsTheMonitorLevel() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        expect(!controller.anchorReached(monitorID: "monitor-a"), "no anchor is reached before a session starts")
        controller.begin(monitorID: "monitor-a", point: .p50)
        expect(!controller.anchorReached(monitorID: "monitor-a"), "a monitor still at 62 has not reached 50")
        // The fake hooks only log writes, so the test moves the readouts to stand in for the monitor.
        fixture.monitors["monitor-a"] = 51
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout 1 point from the level is reached")
        fixture.monitors["monitor-a"] = 49
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout 1 point below the level is reached")
        fixture.monitors["monitor-a"] = 53
        expect(!controller.anchorReached(monitorID: "monitor-a"), "a readout 3 points from the level is not reached")
        fixture.monitors["monitor-a"] = nil
        expect(!controller.anchorReached(monitorID: "monitor-a"), "an unknown monitor brightness is not reached")
        fixture.monitors["monitor-a"] = 50
        fixture.monitors["monitor-b"] = 50
        expect(!controller.anchorReached(monitorID: "monitor-b"), "a monitor other than the active one is never reached")
        controller.end(.done)
        expect(!controller.anchorReached(monitorID: "monitor-a"), "no anchor is reached after the session ends")
    }

    private static func anchorReachedUsesTheLowPointLevel() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p5)
        // The fake hooks only log writes, so the test moves the readouts to stand in for the monitor.
        fixture.monitors["monitor-a"] = 5
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout at 5 reaches the p5 anchor")
        fixture.monitors["monitor-a"] = 6.5
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout 1.5 points above 5 reaches the p5 anchor")
        fixture.monitors["monitor-a"] = 10
        expect(!controller.anchorReached(monitorID: "monitor-a"), "a readout at 10 does not reach the p5 anchor")
    }

    private static func anchorReachedUsesTheP1Level() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p1)
        // The fake hooks only log writes, so the test moves the readouts to stand in for the monitor.
        fixture.monitors["monitor-a"] = 1
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout at 1 reaches the p1 anchor")
        fixture.monitors["monitor-a"] = 2.5
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout 1.5 points above 1 reaches the p1 anchor")
        fixture.monitors["monitor-a"] = 5
        expect(!controller.anchorReached(monitorID: "monitor-a"), "a readout at 5 does not reach the p1 anchor")
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
                                      "setCalibrating true", "setAuto false", "setMonitor monitor-b 50", "writeLevel 44"],
               "monitor A is fully restored before monitor B begins")
        expect(fixture.store.load() == .session(capturedSession(monitor: 30, id: "monitor-b")),
               "the saved session now describes monitor B")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-b", point: .p50, sliderValue: 44),
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

    private static func beginIsRefusedWhileTheBuiltInScreenIsUnavailable() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.backend.statusValue = BuiltInDisplayStatus(available: false, reason: .displayOffline, displayID: nil)
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(controller.notice == .builtInUnavailable, "a begin with the built-in screen unavailable reports the notice")
        expect(!controller.isActive && controller.active == nil, "no session starts")
        expect(fixture.log.events.isEmpty, "no backend write, monitor write or setCalibrating call happens")
        expect(fixture.store.load() == .none, "no session file is written")
    }

    private static func beginIsRefusedWhileTheBuiltInScreenIsAsleep() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.backend.asleep = true
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(controller.notice == .builtInUnavailable, "a begin with the built-in screen asleep reports the notice")
        expect(!controller.isActive && controller.active == nil, "no session starts while the screen is asleep")
        expect(fixture.log.events.isEmpty, "no backend write, monitor write or setCalibrating call happens while asleep")
        expect(fixture.store.load() == .none, "no session file is written while the screen is asleep")
    }

    private static func unknownAutomaticBrightnessStillCalibrates() {
        let fixture = Fixture(auto: nil)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(controller.isActive, "calibration starts when automatic brightness is unknown")
        expect(controller.notice == .autoBrightnessUnknown, "unknown automatic brightness is reported")
        expect(fixture.log.events.contains("writeLevel 44"), "the MacBook slider is still written")
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
        expect(fixture.log.events.contains("writeLevel 44"), "the MacBook slider is still written")
        expect(fixture.store.load() == .session(capturedSession()),
               "the session still records automatic brightness as on")
    }

    private static func failedFirstWriteRestoresAndReports() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // A stored point makes the first MacBook write 20, so the failing write is not the captured level.
        fixture.curves["monitor-a"] = AmbientCurve(p25: 20, p50: nil, p75: nil, p100: nil)
        fixture.backend.failedLevels = [20]
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        expect(!controller.isActive && controller.active == nil, "a failed built-in write ends the session")
        expect(controller.notice == .builtInWriteFailed, "the failed write is reported")
        expect(fixture.store.load() == .none, "the session file is deleted")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 25", "writeLevel 20",
                                      "writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false"],
               "the state changed before the failure is restored, including the monitor")
        expect(fixture.backend.level == 44 && fixture.backend.auto == true,
               "the built-in display is back at its captured state")
    }

    private static func failedSliderWriteEndsTheSessionAndRestores() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p50)
        fixture.backend.failedLevels = [30]
        fixture.log.events.removeAll()
        controller.setSlider(30)
        expect(fixture.log.events == ["writeLevel 30", "writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false"],
               "a failed slider write restores the state in order, including the monitor")
        expect(!controller.isActive && controller.active == nil, "a failed slider write ends the session")
        expect(controller.notice == .builtInWriteFailed, "the failed slider write is reported")
        expect(fixture.store.load() == .none, "the session file is deleted after a failed slider write")
        expect(fixture.backend.level == 44 && fixture.backend.auto == true,
               "the built-in display is back at its captured state after a failed slider write")
    }

    private static func endBuiltInUnavailableReportsTheNoticeWhenASessionWasRunning() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: .p25)
        fixture.log.events.removeAll()
        controller.end(.builtInUnavailable)
        expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false"],
               "an unavailable built-in screen restores the state in order")
        expect(controller.notice == .builtInUnavailable, "a session stopped by the unavailable screen reports the notice")
        expect(!controller.isActive && controller.active == nil, "the session is closed")
        expect(fixture.store.load() == .none, "the session file is deleted")
    }

    private static func endBuiltInUnavailableWithoutASessionSetsNoNotice() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.end(.builtInUnavailable)
        expect(controller.notice == nil, "without a running session the unavailable end sets no notice")
        expect(fixture.log.events.isEmpty, "without a running session nothing is restored")
    }

    private static func otherEndReasonsLeaveNoNotice() {
        for reason in CalibrationEndReason.allCases where reason != .builtInUnavailable {
            let fixture = Fixture()
            defer { fixture.cleanUp() }
            let controller = makeController(fixture)
            controller.begin(monitorID: "monitor-a", point: .p25)
            controller.end(reason)
            expect(controller.notice == nil, "\(reason) reports no notice")
        }
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
