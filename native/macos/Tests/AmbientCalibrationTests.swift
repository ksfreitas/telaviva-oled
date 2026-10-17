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

/// Builds a curve from (monitor percent, MacBook percent) pairs, in the order given.
private func makeCurve(_ pairs: [(Int, Double)]) -> AmbientCurve {
    AmbientCurve(points: pairs.map { AmbientCurvePoint(monitor: Double($0.0), builtin: $0.1) })
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
            curve: { id in self.curves[id] ?? AmbientCurve() },
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
        monitorOnePointStartsASessionAtMonitorOne()
        sliderRangeIsBoundedByNeighbors()
        sliderRangeIsBoundedByLowPointNeighbors()
        sliderRangeIsBoundedByMonitorOneNeighbors()
        allowedRangeWithoutNeighborsIsTheFullRange()
        allowedRangeWithEqualNeighborsIsZeroWidth()
        curveSetKeepsAscendingOrder()
        addKeepsAscendingOrder()
        confirmSavesOnlyTheChangedPoint()
        confirmOnAnExistingPointReplacesItsValue()
        confirmSavesALowPointOnly()
        confirmSavesOnlyTheMonitorOnePoint()
        removePointDeletesThePoint()
        removePointIsRefusedForTheActivePoint()
        removingBelowTwoPointsMakesItIncomplete()
        beginIgnoresPointsOutsideOneToHundred()
        beginOnANewPointIsIgnoredAtThirtyTwoPoints()
        anchorReachedFollowsTheMonitorLevel()
        anchorReachedUsesTheLowPointLevel()
        anchorReachedUsesTheMonitorOneLevel()
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
        controller.begin(monitorID: "monitor-a", point: 25)
        expect(controller.isActive, "first point starts a session")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 25, sliderValue: 44),
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
        fixture.curves["monitor-a"] = makeCurve([(50, 45)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 25)
        fixture.log.events.removeAll()
        controller.begin(monitorID: "monitor-a", point: 50)
        expect(fixture.log.events == ["setMonitor monitor-a 50", "writeLevel 45"],
               "second point moves the monitor to the new point and sets the MacBook to the stored value")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 50, sliderValue: 45),
               "the MacBook slider starts at the stored value for the point")
        expect(fixture.store.load() == .session(capturedSession()),
               "second point keeps the original capture at level 44")
    }

    private static func initialSliderFallsBackToTheCapturedBuiltInLevel() {
        let fixture = Fixture(level: 37)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 75)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 75, sliderValue: 37),
               "with no stored point the slider starts at the captured built-in level, not the monitor brightness")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 75", "writeLevel 37"],
               "the captured built-in level is written as the initial slider")
    }

    private static func lowPointsStartASessionAtTheirMonitorLevel() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // A stored 10% point at 8 caps the 5% range at 8, so the captured built-in level 44 is clamped to it.
        fixture.curves["monitor-a"] = makeCurve([(10, 8)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 5)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 5, sliderValue: 8),
               "the 5% slider starts at the captured built-in level clamped to the range 0...8")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 5", "writeLevel 8"],
               "5% moves the monitor to 5, then writes the clamped MacBook level")
        fixture.log.events.removeAll()
        // 20% is bounded below by the 10% point and has no upper neighbor, so the captured level 44 is inside its range.
        controller.begin(monitorID: "monitor-a", point: 20)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 20, sliderValue: 44),
               "the 20% slider starts at the captured built-in level when it is inside the range")
        expect(fixture.log.events == ["setMonitor monitor-a 20", "writeLevel 44"],
               "20% moves the monitor to 20, then writes the MacBook level")
        expect(fixture.store.load() == .session(capturedSession()), "the session keeps the original capture for 20%")
    }

    private static func monitorOnePointStartsASessionAtMonitorOne() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 1)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 1, sliderValue: 44),
               "the 1% slider starts at the captured built-in level when no point is stored")
        expect(fixture.log.events == ["setCalibrating true", "setAuto false", "setMonitor monitor-a 1", "writeLevel 44"],
               "1% moves the monitor to 1, then writes the MacBook level")
        expect(fixture.store.load() == .session(capturedSession()), "the session keeps the original capture for 1%")
    }

    private static func sliderRangeIsBoundedByNeighbors() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 30), (75, 70)])
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: 50, monitorID: "monitor-a") == 30.0...70.0,
               "50 is bounded by the calibrated MacBook levels of 25 and 75")
        expect(controller.allowedRange(for: 25, monitorID: "monitor-a") == 0.0...70.0,
               "25 is bounded by 0 and the next calibrated MacBook level")
        expect(controller.allowedRange(for: 100, monitorID: "monitor-a") == 70.0...100.0,
               "100 is bounded by the previous calibrated MacBook level and 100")
        expect(controller.allowedRange(for: 50, monitorID: "monitor-b") == 0.0...100.0,
               "an uncalibrated monitor has the full MacBook range")
        controller.begin(monitorID: "monitor-a", point: 50)
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
        fixture.curves["monitor-a"] = makeCurve([(10, 8), (20, 16)])
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: 15, monitorID: "monitor-a") == 8.0...16.0,
               "15 is bounded by the calibrated MacBook levels of 10 and 20")
        expect(controller.allowedRange(for: 5, monitorID: "monitor-a") == 0.0...8.0,
               "5 is bounded by 0 and the next calibrated MacBook level, the one at 10")
        fixture.curves["monitor-a"] = makeCurve([(20, 16), (50, 45)])
        expect(controller.allowedRange(for: 25, monitorID: "monitor-a") == 16.0...45.0,
               "25 is bounded by the calibrated MacBook levels of 20 and 50")
    }

    private static func sliderRangeIsBoundedByMonitorOneNeighbors() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(1, 2), (10, 12)])
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: 5, monitorID: "monitor-a") == 2.0...12.0,
               "5 is bounded by the calibrated MacBook levels of 1 and 10")
        fixture.curves["monitor-a"] = makeCurve([(5, 6)])
        expect(controller.allowedRange(for: 1, monitorID: "monitor-a") == 0.0...6.0,
               "1 is bounded by 0 and the next calibrated MacBook level, the one at 5")
        fixture.curves["monitor-a"] = makeCurve([(10, 9)])
        expect(controller.allowedRange(for: 1, monitorID: "monitor-a") == 0.0...9.0,
               "1 is bounded by 0 and the nearest calibrated MacBook level above it when 5 is absent")
    }

    private static func allowedRangeWithoutNeighborsIsTheFullRange() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: 1, monitorID: "monitor-a") == 0.0...100.0,
               "with no saved points the range at 1 is 0...100")
        expect(controller.allowedRange(for: 100, monitorID: "monitor-a") == 0.0...100.0,
               "with no saved points the range at 100 is 0...100")
        fixture.curves["monitor-a"] = makeCurve([(50, 40)])
        expect(controller.allowedRange(for: 25, monitorID: "monitor-a") == 0.0...40.0,
               "with only a point above, the range below it starts at 0")
        expect(controller.allowedRange(for: 60, monitorID: "monitor-a") == 40.0...100.0,
               "with only a point below, the range above it ends at 100")
    }

    private static func allowedRangeWithEqualNeighborsIsZeroWidth() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 40), (75, 40)])
        let controller = makeController(fixture)
        expect(controller.allowedRange(for: 50, monitorID: "monitor-a") == 40.0...40.0,
               "neighbors with equal MacBook levels leave a zero-width range")
    }

    private static func curveSetKeepsAscendingOrder() {
        var curve = AmbientCurve()
        curve.set(monitor: 75, builtin: 70)
        curve.set(monitor: 25, builtin: 20)
        curve.set(monitor: 50, builtin: 40)
        expect(curve == makeCurve([(25, 20), (50, 40), (75, 70)]),
               "set inserts points that arrive out of order at their sorted position")
        curve.set(monitor: 50, builtin: 45)
        expect(curve == makeCurve([(25, 20), (50, 45), (75, 70)]),
               "set on an existing monitor percent replaces its value without adding a point")
        curve.remove(monitor: 25)
        expect(curve == makeCurve([(50, 45), (75, 70)]), "remove deletes the point at that monitor percent")
    }

    private static func addKeepsAscendingOrder() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 50)
        controller.confirm()
        controller.begin(monitorID: "monitor-a", point: 25)
        controller.confirm()
        controller.begin(monitorID: "monitor-a", point: 75)
        controller.confirm()
        expect(fixture.curves["monitor-a"]?.points.map { $0.monitor } == [25.0, 50.0, 75.0],
               "points added out of order are kept in ascending monitor order")
        expect(fixture.curves["monitor-a"]?.isComplete == true, "three added points make the curve complete")
    }

    private static func confirmSavesOnlyTheChangedPoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 20), (75, 70)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 50)
        fixture.log.events.removeAll()
        controller.setSlider(40)
        expect(fixture.log.events == ["writeLevel 40"], "the slider writes the MacBook level and not the monitor")
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == makeCurve([(25, 20), (50, 40), (75, 70)]),
               "confirm stores the MacBook level for the point and preserves the others")
        expect(saved?.point(at: 50)?.builtin == 40, "the saved curve reads the new point back by monitor percent")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: 50, sliderValue: 40),
               "session stays active after confirm")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func confirmOnAnExistingPointReplacesItsValue() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 20), (50, 40), (75, 70)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 50)
        expect(controller.active?.sliderValue == 40, "an existing point starts the slider at its stored value")
        controller.setSlider(45)
        fixture.log.events.removeAll()
        controller.confirm()
        expect(fixture.curves["monitor-a"] == makeCurve([(25, 20), (50, 45), (75, 70)]),
               "confirm on an existing point replaces its value and keeps the other points")
        expect(fixture.curves["monitor-a"]?.points.count == 3, "confirm on an existing point adds no point")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
    }

    private static func confirmSavesALowPointOnly() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // The stored 5% and 20% points bound the 10% range at 2...16, and the other points must survive the save.
        fixture.curves["monitor-a"] = makeCurve([(5, 2), (20, 16), (25, 20), (75, 70)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 10)
        fixture.log.events.removeAll()
        controller.setSlider(12)
        expect(fixture.log.events == ["writeLevel 12"], "the slider writes the MacBook level and not the monitor")
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == makeCurve([(5, 2), (10, 12), (20, 16), (25, 20), (75, 70)]),
               "confirm stores the MacBook level for 10% and preserves every other point")
        expect(saved?.point(at: 10)?.builtin == 12, "the saved curve reads the new low point back by monitor percent")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: 10, sliderValue: 12),
               "session stays active after confirm at 10%")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func confirmSavesOnlyTheMonitorOnePoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // The stored 5% point bounds the 1% range at 0...6, and the other points must survive the save.
        fixture.curves["monitor-a"] = makeCurve([(5, 6), (10, 9), (25, 20)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 1)
        fixture.log.events.removeAll()
        controller.setSlider(1)
        expect(fixture.log.events == ["writeLevel 1"], "the slider writes the MacBook level and not the monitor")
        fixture.log.events.removeAll()
        controller.confirm()
        let saved = fixture.curves["monitor-a"]
        expect(saved == makeCurve([(1, 1), (5, 6), (10, 9), (25, 20)]),
               "confirm stores the MacBook level for 1% and preserves every other point")
        expect(saved?.point(at: 1)?.builtin == 1, "the saved curve reads the new point at 1% back by monitor percent")
        expect(fixture.log.events == ["saveCurve monitor-a"], "confirm saves without touching the built-in display")
        expect(controller.isActive && controller.active == ActiveCalibration(monitorID: "monitor-a", point: 1, sliderValue: 1),
               "session stays active after confirm at 1%")
        expect(fixture.store.load() == .session(capturedSession()), "session file stays until the session ends")
    }

    private static func removePointDeletesThePoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 20), (50, 40), (75, 70)])
        let controller = makeController(fixture)
        controller.removePoint(monitorID: "monitor-a", point: 50)
        expect(fixture.curves["monitor-a"] == makeCurve([(25, 20), (75, 70)]),
               "remove deletes the point and keeps the others")
        expect(fixture.log.events == ["saveCurve monitor-a"],
               "remove saves the curve without touching the built-in display")
        expect(!controller.isActive, "remove without a session starts none")
    }

    private static func removePointIsRefusedForTheActivePoint() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 20), (50, 40), (75, 70)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 50)
        fixture.log.events.removeAll()
        controller.removePoint(monitorID: "monitor-a", point: 50)
        expect(fixture.curves["monitor-a"] == makeCurve([(25, 20), (50, 40), (75, 70)]),
               "the point being calibrated cannot be removed")
        expect(fixture.log.events.isEmpty, "refusing to remove the active point saves and writes nothing")
        expect(controller.active?.point == 50, "the session stays on the active point")
        controller.removePoint(monitorID: "monitor-a", point: 25)
        expect(fixture.curves["monitor-a"] == makeCurve([(50, 40), (75, 70)]),
               "another point can be removed while one is being calibrated")
        expect(fixture.log.events == ["saveCurve monitor-a"], "removing another point saves once")
    }

    private static func removingBelowTwoPointsMakesItIncomplete() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.curves["monitor-a"] = makeCurve([(25, 20), (50, 40), (75, 70)])
        let controller = makeController(fixture)
        controller.removePoint(monitorID: "monitor-a", point: 75)
        expect(fixture.curves["monitor-a"]?.isComplete == true, "two points make the curve complete")
        controller.removePoint(monitorID: "monitor-a", point: 50)
        expect(fixture.curves["monitor-a"]?.isComplete == false, "removing down to one point makes the curve incomplete")
    }

    private static func beginIgnoresPointsOutsideOneToHundred() {
        for point in [0, 101] {
            let fixture = Fixture()
            defer { fixture.cleanUp() }
            let controller = makeController(fixture)
            controller.begin(monitorID: "monitor-a", point: point)
            expect(!controller.isActive && controller.active == nil, "point \(point) starts no session")
            expect(fixture.log.events.isEmpty, "point \(point) makes no backend, monitor or calibrating call")
            expect(controller.notice == nil, "point \(point) reports no notice")
            expect(fixture.store.load() == .none, "point \(point) writes no session file")
            expect(fixture.curves["monitor-a"] == nil, "point \(point) stores no curve")
        }
    }

    private static func beginOnANewPointIsIgnoredAtThirtyTwoPoints() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        // Thirty-two saved points at monitor percents 1...32 fill the per-monitor limit.
        fixture.curves["monitor-a"] = AmbientCurve(points: (1...32).map {
            AmbientCurvePoint(monitor: Double($0), builtin: Double($0))
        })
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 50)
        expect(!controller.isActive && controller.active == nil, "a new point at 32 points starts no session")
        expect(fixture.log.events.isEmpty, "a new point at 32 points makes no backend, monitor or calibrating call")
        expect(fixture.curves["monitor-a"]?.points.count == 32, "a new point at 32 points stores nothing")
        controller.begin(monitorID: "monitor-a", point: 16)
        expect(controller.active == ActiveCalibration(monitorID: "monitor-a", point: 16, sliderValue: 16),
               "an existing point can still be calibrated at 32 points")
        expect(fixture.curves["monitor-a"]?.points.count == 32, "beginning an existing point adds no point")
    }

    private static func anchorReachedFollowsTheMonitorLevel() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        expect(!controller.anchorReached(monitorID: "monitor-a"), "no anchor is reached before a session starts")
        controller.begin(monitorID: "monitor-a", point: 50)
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
        controller.begin(monitorID: "monitor-a", point: 5)
        // The fake hooks only log writes, so the test moves the readouts to stand in for the monitor.
        fixture.monitors["monitor-a"] = 5
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout at 5 reaches the 5% anchor")
        fixture.monitors["monitor-a"] = 6.5
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout 1.5 points above 5 reaches the 5% anchor")
        fixture.monitors["monitor-a"] = 10
        expect(!controller.anchorReached(monitorID: "monitor-a"), "a readout at 10 does not reach the 5% anchor")
    }

    private static func anchorReachedUsesTheMonitorOneLevel() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 1)
        // The fake hooks only log writes, so the test moves the readouts to stand in for the monitor.
        fixture.monitors["monitor-a"] = 1
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout at 1 reaches the 1% anchor")
        fixture.monitors["monitor-a"] = 2.5
        expect(controller.anchorReached(monitorID: "monitor-a"), "a readout 1.5 points above 1 reaches the 1% anchor")
        fixture.monitors["monitor-a"] = 5
        expect(!controller.anchorReached(monitorID: "monitor-a"), "a readout at 5 does not reach the 1% anchor")
    }

    private static func everyEndReasonRestoresInOrder() {
        for reason in CalibrationEndReason.allCases {
            let fixture = Fixture()
            defer { fixture.cleanUp() }
            let controller = makeController(fixture)
            controller.begin(monitorID: "monitor-a", point: 25)
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
        controller.begin(monitorID: "monitor-a", point: 25)
        fixture.log.events.removeAll()
        controller.end(.done)
        expect(fixture.log.events == ["writeLevel 44", "setMonitor monitor-a 62", "setCalibrating false"],
               "automatic brightness that was off is not switched on")
    }

    private static func endSkipsMonitorWhenItsBrightnessWasUnknown() {
        let fixture = Fixture()
        defer { fixture.cleanUp() }
        fixture.monitors["monitor-a"] = nil
        fixture.curves["monitor-a"] = makeCurve([(25, 20)])
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 25)
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
        controller.begin(monitorID: "monitor-a", point: 25)
        fixture.log.events.removeAll()
        controller.begin(monitorID: "monitor-b", point: 50)
        expect(fixture.log.events == ["writeLevel 44", "setAuto true", "setMonitor monitor-a 62", "setCalibrating false",
                                      "setCalibrating true", "setAuto false", "setMonitor monitor-b 50", "writeLevel 44"],
               "monitor A is fully restored before monitor B begins")
        expect(fixture.store.load() == .session(capturedSession(monitor: 30, id: "monitor-b")),
               "the saved session now describes monitor B")
        expect(controller.active == ActiveCalibration(monitorID: "monitor-b", point: 50, sliderValue: 44),
               "active calibration moves to monitor B")
    }

    private static func unreadableBuiltInLevelStartsNothing() {
        let fixture = Fixture(level: nil)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 25)
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
        controller.begin(monitorID: "monitor-a", point: 25)
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
        controller.begin(monitorID: "monitor-a", point: 25)
        expect(controller.notice == .builtInUnavailable, "a begin with the built-in screen asleep reports the notice")
        expect(!controller.isActive && controller.active == nil, "no session starts while the screen is asleep")
        expect(fixture.log.events.isEmpty, "no backend write, monitor write or setCalibrating call happens while asleep")
        expect(fixture.store.load() == .none, "no session file is written while the screen is asleep")
    }

    private static func unknownAutomaticBrightnessStillCalibrates() {
        let fixture = Fixture(auto: nil)
        defer { fixture.cleanUp() }
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 25)
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
        controller.begin(monitorID: "monitor-a", point: 25)
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
        fixture.curves["monitor-a"] = makeCurve([(25, 20)])
        fixture.backend.failedLevels = [20]
        let controller = makeController(fixture)
        controller.begin(monitorID: "monitor-a", point: 25)
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
        controller.begin(monitorID: "monitor-a", point: 50)
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
        controller.begin(monitorID: "monitor-a", point: 25)
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
            controller.begin(monitorID: "monitor-a", point: 25)
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
