import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("BuiltInProtectionTests: \(message)\n".utf8))
        exit(1)
    }
}

private func levelText(_ value: Double) -> String {
    value.rounded() == value ? String(Int(value)) : String(value)
}

/// 2026-10-16T12:00:00Z, the clock every test uses.
private let fixedNow = Date(timeIntervalSince1970: 1_792_152_000)
private let fixedStartedAt = "2026-10-16T12:00:00Z"

/// A decision with the given reasons. Defaults to a presence dim to 30 percent.
private func protection(reasons: [String] = ["presence"], target: Double? = 30, holdAuto: Bool = false,
                        available: Bool = true) -> CoreBuiltInProtection {
    CoreBuiltInProtection(protectable: true, protected: true, available: available, reasons: reasons,
                          targetPercent: target, holdAutoBrightness: holdAuto, holding: false)
}

/// The decision when nothing dims the built-in display.
private func idle(available: Bool = true) -> CoreBuiltInProtection {
    protection(reasons: [], target: nil, holdAuto: false, available: available)
}

/// Collects the core diagnostic lines written while `body` runs.
private func captureCoreLogs(_ body: () -> Void) -> [String] {
    var lines: [String] = []
    let previous = DiagnosticLog.coreSink
    DiagnosticLog.coreSink = { lines.append($0) }
    body()
    DiagnosticLog.coreSink = previous
    return lines
}

/// Ordered log shared by the backend and the owner spy. Each write, acquire, release and holding report appends one entry.
private final class EventLog {
    var events: [String] = []
}

private final class FakeBackend: BuiltInDisplayBackend {
    let log: EventLog
    var level: Double?
    /// Upcoming writes consume these results (true applies the level). Later writes follow `failWrites`.
    var writeResults: [Bool] = []
    var failWrites = false
    /// Calls to `readLevel`, to prove a leftover baseline is not read again.
    var reads = 0
    var statusValue = BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)

    init(log: EventLog, level: Double?) {
        self.log = log
        self.level = level
    }

    func status() -> BuiltInDisplayStatus { statusValue }

    func isAsleep() -> Bool { false }

    func readLevel() -> Double? {
        reads += 1
        return level
    }

    func writeLevel(_ percent: Double) -> Bool {
        log.events.append("writeLevel \(levelText(percent))")
        let applies = writeResults.isEmpty ? !failWrites : writeResults.removeFirst()
        guard applies else { return false }
        level = percent
        return true
    }

    func autoBrightnessEnabled() -> Bool? { nil }

    func setAutoBrightness(_ enabled: Bool) -> Bool { false }
}

/// Records acquire and release in the shared log and returns a configurable acquire outcome.
private final class SpyHolder: BuiltInAutoBrightnessHolding {
    let log: EventLog
    var outcome = AutoBrightnessAcquireOutcome.suspended

    init(log: EventLog) {
        self.log = log
    }

    func acquire() -> AutoBrightnessAcquireOutcome {
        log.events.append("acquire")
        return outcome
    }

    func release() {
        log.events.append("release")
    }
}

/// Stores one-shot blocks until a test fires them. The controller schedules no repeating work.
private final class FakeScheduler: BuiltInMonitorScheduling {
    final class Registration: BuiltInMonitorCancelable {
        let block: () -> Void
        private(set) var canceled = false
        private(set) var fired = false

        init(block: @escaping () -> Void) {
            self.block = block
        }

        var isLive: Bool { !canceled && !fired }

        func cancel() {
            canceled = true
        }

        func fire() {
            fired = true
            block()
        }
    }

    /// Delays of every one-shot scheduled so far, in order.
    private(set) var delays: [TimeInterval] = []
    private var onces: [Registration] = []

    func repeating(interval: TimeInterval, leeway: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable {
        fatalError("BuiltInProtectionTests: the protection controller schedules no repeating work")
    }

    func once(after: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable {
        let registration = Registration(block: block)
        delays.append(after)
        onces.append(registration)
        return registration
    }

    var liveCount: Int { onces.filter { $0.isLive }.count }

    /// Runs the oldest live one-shot, as the scheduler would after its delay. False when none is pending.
    @discardableResult
    func fireNext() -> Bool {
        guard let next = onces.first(where: { $0.isLive }) else { return false }
        next.fire()
        return true
    }
}

/// One temp directory, one fake built-in display, one owner spy and the holding reports.
private final class Fixture {
    let log: EventLog
    let backend: FakeBackend
    let holder: SpyHolder
    let scheduler = FakeScheduler()
    let store: BuiltInProtectionStore
    let directory: URL
    /// Each `holding` value reported, in order.
    var holdingReports: [Bool] = []
    /// Whether the recovery record existed at each report.
    var recordExisted: [Bool] = []
    /// The controller's clock. Starts at `fixedNow` and moves only through `advance`.
    var clock = fixedNow

    init(level: Double?) {
        let log = EventLog()
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("builtin-protection-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        self.log = log
        self.backend = FakeBackend(log: log, level: level)
        self.holder = SpyHolder(log: log)
        self.directory = directory
        self.store = BuiltInProtectionStore(directory: directory)
    }

    var sessionURL: URL { directory.appendingPathComponent("builtin-protection-session.json") }

    func makeController() -> BuiltInProtectionController {
        BuiltInProtectionController(backend: backend, scheduler: scheduler, store: store, holder: holder,
                                    reportHolding: { [self] value in self.recordHolding(value) },
                                    now: { [self] in self.clock })
    }

    /// Moves the fake clock forward, to test the verification window.
    func advance(by seconds: TimeInterval) {
        clock = clock.addingTimeInterval(seconds)
    }

    func recordHolding(_ value: Bool) {
        holdingReports.append(value)
        recordExisted.append(FileManager.default.fileExists(atPath: sessionURL.path))
        log.events.append("holding \(value)")
    }

    /// Forgets what was recorded so far, so one phase of a test can be checked alone.
    func clearRecords() {
        log.events.removeAll()
        holdingReports.removeAll()
        recordExisted.removeAll()
    }

    func writes(of percent: Double) -> Int {
        log.events.filter { $0 == "writeLevel \(levelText(percent))" }.count
    }

    func cleanUp() {
        try? FileManager.default.removeItem(at: directory)
    }
}

/// A controller that has dimmed a 62.5 percent screen to 30 and whose restore failed three times.
private func parkedFixture() -> (Fixture, BuiltInProtectionController) {
    let fixture = Fixture(level: 62.5)
    let controller = fixture.makeController()
    controller.reconcile(protection(target: 30, holdAuto: true))
    fixture.backend.failWrites = true
    controller.reconcile(idle())
    fixture.scheduler.fireNext()
    fixture.scheduler.fireNext()
    return (fixture, controller)
}

/// A controller whose dim to 30 percent failed three times, so it is parked while the write keeps failing.
private func parkedDimFixture() -> (Fixture, BuiltInProtectionController) {
    let fixture = Fixture(level: 80)
    let controller = fixture.makeController()
    fixture.backend.failWrites = true
    controller.reconcile(protection(target: 30))
    fixture.scheduler.fireNext()
    fixture.scheduler.fireNext()
    return (fixture, controller)
}

/// Runs one dim with the owner outcome given and returns the notice the controller shows.
private func noticeAfterDim(_ outcome: AutoBrightnessAcquireOutcome) -> BuiltInProtectionController.Notice? {
    let fixture = Fixture(level: 62.5)
    defer { fixture.cleanUp() }
    fixture.holder.outcome = outcome
    let controller = fixture.makeController()
    controller.reconcile(protection(target: 30, holdAuto: true))
    expect(fixture.backend.level == 30, "the dim still happens when the owner outcome is \(outcome)")
    return controller.notice
}

@main
private struct BuiltInProtectionTests {
    static func main() {
        dimSavesRecordThenReportsHoldingThenAcquiresThenWrites()
        dimWithOptionOffLeavesAutoBrightnessAlone()
        targetAboveBaselineWritesNothing()
        restoreWritesBaselineThenReleasesThenReports()
        unreadableBaselineSkipsTheDimAndLogs()
        failedDimWriteSchedulesOneRetryThatReapplies()
        failedDimWriteIsRetriedByTheNextSnapshot()
        failedRestoreRetriesThreeTimesThenParksKeepingTheRecord()
        parkedRestoreResumesWhenTheDisplayIsBackOnTheNextReconcile()
        unavailableDisplayGetsNoWrites()
        leftoverRecordRestoresTheStoredBaselineWithoutRereadingIt()
        corruptRecordIsDeletedWithoutHolding()
        restoreNowWritesOnceThenCleansUp()
        restoreNowFailureKeepsTheRecordAndHolding()
        changingTargetOrOptionWhileHoldingRewritesAndTogglesTheOwner()
        unknownOrNotChangedAutoBrightnessNoticesAndStillDims()
        onlyNilAndOfflineReasonsAreProtectable()
        changedDecisionStartsAFreshRestoreEpisode()
        failedDimRetriesTwiceThenParksWithoutTimer()
        changedTargetAfterParkedDimStartsAFreshEpisode()
        parkedDimWritesNothingOnLaterSnapshots()
        availabilityChangeAfterParkedDimStartsAFreshEpisode()
        matchingLevelEndsParkedDimEpisodeSoDriftGetsFreshAttempts()
        failedRestoreKeepsWritingOnEverySnapshot()
        reconcileAfterRestoreNowDoesNothing()
        reconcileAfterRestoreNowWithoutHoldingDoesNothing()
        identicalSnapshotsWithinTheWindowSkipTheRead()
        driftIsCorrectedOnceTheWindowExpires()
        changedTargetVerifiesImmediatelyInsideTheWindow()
        availabilityFlipVerifiesImmediatelyInsideTheWindow()
        print("Built-in protection tests passed")
    }

    private static func dimSavesRecordThenReportsHoldingThenAcquiresThenWrites() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        expect(fixture.recordExisted == [true], "the recovery record exists when holding is reported")
        expect(fixture.log.events == ["holding true", "acquire", "writeLevel 30"],
               "the dim order is record, holding true, owner acquire, then the write")
        expect(fixture.store.load() == .record(BuiltInProtectionRecord(started_at: fixedStartedAt, baseline_percent: 62.5)),
               "the record stores the fresh baseline and the clock")
        expect(controller.isHolding, "the controller holds after a dim")
        expect(fixture.backend.level == 30, "the level is the target")
    }

    private static func dimWithOptionOffLeavesAutoBrightnessAlone() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: false))
        expect(fixture.log.events == ["holding true", "writeLevel 30"], "option off never acquires the owner")
        expect(controller.isHolding, "the dim still holds with the option off")
    }

    private static func targetAboveBaselineWritesNothing() {
        let fixture = Fixture(level: 40)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 70))
        expect(fixture.log.events == ["holding true"], "a target above the baseline writes nothing")
        expect(fixture.backend.level == 40, "the level is not raised above the baseline")
    }

    private static func restoreWritesBaselineThenReleasesThenReports() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        fixture.clearRecords()
        controller.reconcile(idle())
        expect(fixture.log.events == ["writeLevel 62.5", "release", "holding false"],
               "restore writes the baseline, then releases the owner, then reports holding false")
        expect(fixture.backend.level == 62.5, "the level returns to the unrounded baseline")
        expect(fixture.recordExisted == [false], "the record is already deleted when holding false is reported")
        expect(fixture.store.load() == .none, "the record is deleted after a verified restore")
        expect(!controller.isHolding, "the controller is idle after a verified restore")
    }

    private static func unreadableBaselineSkipsTheDimAndLogs() {
        let fixture = Fixture(level: nil)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        let lines = captureCoreLogs {
            controller.reconcile(protection(target: 30, holdAuto: true))
        }
        expect(fixture.log.events.isEmpty, "an unreadable baseline writes nothing and acquires nothing")
        expect(fixture.store.load() == .none, "an unreadable baseline writes no record")
        expect(fixture.holdingReports.isEmpty, "an unreadable baseline reports no holding")
        expect(!controller.isHolding, "an unreadable baseline does not hold")
        expect(lines == ["builtin_dim skipped: baseline unreadable"], "the skipped dim is logged")
    }

    private static func failedDimWriteSchedulesOneRetryThatReapplies() {
        let fixture = Fixture(level: 80)
        defer { fixture.cleanUp() }
        fixture.backend.writeResults = [false]
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        expect(fixture.backend.level == 80, "a failed write leaves the level unchanged")
        expect(fixture.scheduler.delays == [1.0], "a failed dim schedules one retry one second later")
        expect(fixture.scheduler.fireNext(), "the retry fires")
        expect(fixture.backend.level == 30, "the retry re-applies the target")
        expect(fixture.scheduler.liveCount == 0, "no retry stays pending after a verified dim")
    }

    private static func failedDimWriteIsRetriedByTheNextSnapshot() {
        let fixture = Fixture(level: 80)
        defer { fixture.cleanUp() }
        fixture.backend.writeResults = [false]
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        controller.reconcile(protection(target: 30))
        expect(fixture.backend.level == 30, "the next snapshot re-applies the target")
        expect(fixture.scheduler.liveCount == 0, "the next snapshot cancels the pending retry")
    }

    private static func failedRestoreRetriesThreeTimesThenParksKeepingTheRecord() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        fixture.backend.failWrites = true
        fixture.clearRecords()
        controller.reconcile(idle())
        expect(fixture.scheduler.delays == [1.0], "the first failed restore schedules a retry one second later")
        expect(fixture.scheduler.fireNext(), "the first retry fires")
        expect(fixture.scheduler.delays == [1.0, 1.0], "the second failed restore schedules another retry")
        expect(fixture.scheduler.fireNext(), "the second retry fires")
        expect(fixture.scheduler.delays == [1.0, 1.0], "the third failed attempt schedules nothing")
        expect(fixture.writes(of: 62.5) == 3, "exactly three restore attempts are made")
        expect(fixture.scheduler.liveCount == 0, "the parked controller has no timer")
        expect(controller.notice == .restoreFailed, "the parked restore is reported")
        expect(controller.isHolding, "the controller keeps holding")
        expect(fixture.store.load() == .record(BuiltInProtectionRecord(started_at: fixedStartedAt, baseline_percent: 62.5)),
               "the record is kept")
        expect(!fixture.log.events.contains("release"), "the owner is not released before a verified restore")
        expect(fixture.holdingReports.isEmpty, "holding false is not reported")
    }

    private static func parkedRestoreResumesWhenTheDisplayIsBackOnTheNextReconcile() {
        let (fixture, controller) = parkedFixture()
        defer { fixture.cleanUp() }
        fixture.clearRecords()
        fixture.backend.failWrites = false
        controller.reconcile(idle(available: false))
        expect(fixture.log.events.isEmpty, "a parked controller writes nothing while the display is unavailable")
        controller.reconcile(idle())
        expect(fixture.backend.level == 62.5, "the next reconcile restores the baseline")
        expect(!controller.isHolding, "the resumed restore is verified")
        expect(controller.notice == nil, "a verified restore clears the restore failure notice")
        expect(fixture.store.load() == .none, "the record is deleted after the resumed restore")
    }

    private static func unavailableDisplayGetsNoWrites() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(available: false))
        expect(fixture.log.events.isEmpty && fixture.store.load() == .none,
               "an unavailable display is not dimmed")
        controller.reconcile(protection(target: 30))
        fixture.clearRecords()
        controller.reconcile(idle(available: false))
        expect(fixture.log.events.isEmpty, "a holding controller writes nothing while unavailable")
        expect(controller.isHolding, "the controller keeps holding while unavailable")
    }

    private static func leftoverRecordRestoresTheStoredBaselineWithoutRereadingIt() {
        let fixture = Fixture(level: 10)
        defer { fixture.cleanUp() }
        // The level left behind by the crash is 10; the stored baseline is 62.5.
        expect(fixture.store.save(BuiltInProtectionRecord(started_at: "2026-10-16T11:00:00Z", baseline_percent: 62.5)),
               "the leftover record is written")
        let controller = fixture.makeController()
        controller.recoverIfNeeded()
        expect(controller.isHolding, "a leftover record starts holding")
        expect(fixture.holdingReports == [true], "recovery reports holding true")
        expect(fixture.log.events == ["holding true"], "recovery writes nothing")
        controller.reconcile(idle())
        expect(fixture.log.events == ["holding true", "writeLevel 62.5", "holding false"],
               "the first empty snapshot restores the stored baseline")
        expect(fixture.backend.reads == 1, "the level is read once, for the read-back, and the baseline is not re-read")
        expect(fixture.store.load() == .none, "the record is deleted after the restore")
    }

    private static func corruptRecordIsDeletedWithoutHolding() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let written = (try? Data("not a record".utf8).write(to: fixture.sessionURL)) != nil
        expect(written, "garbage bytes are written to the record path")
        let controller = fixture.makeController()
        controller.recoverIfNeeded()
        expect(fixture.store.load() == .none, "the unreadable record is deleted")
        expect(!controller.isHolding, "an unreadable record does not start holding")
        expect(fixture.log.events.isEmpty && fixture.holdingReports.isEmpty, "nothing is written or reported")
    }

    private static func restoreNowWritesOnceThenCleansUp() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        fixture.clearRecords()
        controller.restoreNow()
        expect(fixture.log.events == ["writeLevel 62.5", "release", "holding false"],
               "restoreNow writes the baseline once, releases the owner and reports holding false")
        expect(fixture.backend.level == 62.5, "restoreNow restores the unrounded baseline")
        expect(fixture.store.load() == .none, "restoreNow deletes the record")
        expect(!controller.isHolding, "restoreNow leaves the controller idle")
        controller.restoreNow()
        expect(fixture.log.events.count == 3, "a second restoreNow does nothing")
    }

    private static func restoreNowFailureKeepsTheRecordAndHolding() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        fixture.clearRecords()
        fixture.backend.failWrites = true
        controller.restoreNow()
        expect(fixture.log.events == ["writeLevel 62.5", "release"],
               "restoreNow makes one attempt and releases the owner even when the write fails")
        expect(controller.isHolding, "a failed restoreNow keeps holding")
        expect(fixture.store.load() == .record(BuiltInProtectionRecord(started_at: fixedStartedAt, baseline_percent: 62.5)),
               "a failed restoreNow keeps the record for the next launch")
        expect(fixture.scheduler.liveCount == 0, "restoreNow schedules no retry")
    }

    private static func changingTargetOrOptionWhileHoldingRewritesAndTogglesTheOwner() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: false))
        fixture.clearRecords()
        controller.reconcile(protection(target: 10, holdAuto: true))
        expect(fixture.log.events == ["acquire", "writeLevel 10"],
               "a new target rewrites the level, and the option acquires the owner before the write")
        controller.reconcile(protection(target: 10, holdAuto: false))
        expect(fixture.log.events == ["acquire", "writeLevel 10", "release"],
               "turning the option off releases the owner and writes nothing when the level already matches")
        expect(fixture.backend.level == 10, "the level stays at the new target")
    }

    private static func unknownOrNotChangedAutoBrightnessNoticesAndStillDims() {
        expect(noticeAfterDim(.unknown) == .autoBrightnessUnknown, "an unknown automatic brightness state is noticed")
        expect(noticeAfterDim(.notChanged) == .autoBrightnessNotChanged, "a refused automatic brightness change is noticed")
        expect(noticeAfterDim(.suspended) == nil, "a suspended owner adds no notice")
        expect(noticeAfterDim(.alreadyOff) == nil, "an already-off setting adds no notice")
    }

    private static func onlyNilAndOfflineReasonsAreProtectable() {
        expect(BuiltInProtectionController.isProtectable(reason: nil), "a display with no reason is protectable")
        expect(BuiltInProtectionController.isProtectable(reason: .displayOffline), "an offline display is protectable")
        expect(!BuiltInProtectionController.isProtectable(reason: .noBuiltInDisplay), "a missing display is not protectable")
        expect(!BuiltInProtectionController.isProtectable(reason: .brightnessNotAdjustable), "a non-adjustable display is not protectable")
        expect(!BuiltInProtectionController.isProtectable(reason: .apiUnavailable), "an unavailable API is not protectable")
    }

    private static func changedDecisionStartsAFreshRestoreEpisode() {
        let (fixture, controller) = parkedFixture()
        defer { fixture.cleanUp() }
        // The level is still at the target, so the dim verifies without a write.
        fixture.backend.failWrites = false
        controller.reconcile(protection(target: 30))
        expect(fixture.scheduler.liveCount == 0, "a dim back at the target leaves no retry pending")
        fixture.backend.failWrites = true
        controller.reconcile(idle())
        expect(fixture.scheduler.liveCount == 1, "a new restore episode retries instead of parking at once")
    }

    /// A dim whose writes never change the level makes three attempts in one episode, then parks with no timer.
    private static func failedDimRetriesTwiceThenParksWithoutTimer() {
        let fixture = Fixture(level: 80)
        defer { fixture.cleanUp() }
        fixture.backend.failWrites = true
        let controller = fixture.makeController()
        let lines = captureCoreLogs {
            controller.reconcile(protection(target: 30))
            expect(fixture.scheduler.fireNext(), "the first dim retry fires")
            expect(fixture.scheduler.fireNext(), "the second dim retry fires")
            expect(!fixture.scheduler.fireNext(), "the third failed dim schedules no retry")
        }
        expect(fixture.writes(of: 30) == 3, "exactly three dim attempts are made in the episode")
        expect(fixture.scheduler.liveCount == 0, "the parked dim leaves no pending block")
        expect(fixture.backend.level == 80, "the level never changes")
        expect(lines == ["builtin_dim unverified attempt=1", "builtin_dim unverified attempt=2", "builtin_dim_failed attempts=3"],
               "each failed dim attempt is logged and the parked one is reported")
    }

    /// A new target after a parked dim starts a fresh episode with three more attempts.
    private static func changedTargetAfterParkedDimStartsAFreshEpisode() {
        let fixture = Fixture(level: 80)
        defer { fixture.cleanUp() }
        fixture.backend.failWrites = true
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        fixture.scheduler.fireNext()
        fixture.scheduler.fireNext()
        expect(fixture.scheduler.liveCount == 0, "the first dim episode is parked")
        controller.reconcile(protection(target: 20))
        expect(fixture.scheduler.liveCount == 1, "a new target retries instead of parking at once")
        expect(fixture.scheduler.fireNext(), "the first retry of the new target fires")
        expect(fixture.scheduler.fireNext(), "the second retry of the new target fires")
        expect(!fixture.scheduler.fireNext(), "the new episode parks after three attempts")
        expect(fixture.writes(of: 30) == 3, "the old target keeps its three attempts")
        expect(fixture.writes(of: 20) == 3, "the new target gets three attempts")
    }

    /// A parked dim writes nothing and logs nothing on later snapshots that carry the same decision.
    private static func parkedDimWritesNothingOnLaterSnapshots() {
        let (fixture, controller) = parkedDimFixture()
        defer { fixture.cleanUp() }
        expect(fixture.writes(of: 30) == 3, "the dim is parked after three attempts")
        let lines = captureCoreLogs {
            for _ in 0..<5 {
                controller.reconcile(protection(target: 30))
            }
        }
        expect(fixture.writes(of: 30) == 3, "a parked dim makes no write on later snapshots")
        expect(lines.isEmpty, "a parked dim logs nothing on later snapshots")
        expect(fixture.scheduler.liveCount == 0, "a parked dim schedules nothing on later snapshots")
    }

    /// The display coming back after a lid close or wake starts a fresh episode, so the next dim gets its attempts.
    private static func availabilityChangeAfterParkedDimStartsAFreshEpisode() {
        let (fixture, controller) = parkedDimFixture()
        defer { fixture.cleanUp() }
        let before = fixture.writes(of: 30)
        let lines = captureCoreLogs {
            controller.reconcile(protection(target: 30, available: false))
            controller.reconcile(protection(target: 30, available: true))
        }
        expect(fixture.writes(of: 30) == before + 1, "the display coming back gets one new write attempt")
        expect(lines == ["builtin_dim unverified attempt=1"], "the new episode starts at attempt one instead of parking at once")
    }

    /// A level that already matches the target ends a parked dim episode, so a later drift away from the target gets fresh attempts.
    private static func matchingLevelEndsParkedDimEpisodeSoDriftGetsFreshAttempts() {
        let (fixture, controller) = parkedDimFixture()
        defer { fixture.cleanUp() }
        expect(fixture.writes(of: 30) == 3, "the dim is parked after three attempts")
        // Something else brings the level to the target, so the next snapshot matches it and must not write.
        fixture.backend.level = 30
        let before = fixture.log.events
        controller.reconcile(protection(target: 30))
        expect(fixture.log.events == before, "a matching level writes nothing: \(fixture.log.events)")
        // The level drifts away again. The match above verified it, so the window must end before the next snapshot
        // reads it. The episode is fresh, so the next snapshot makes a new write attempt.
        fixture.advance(by: 6)
        fixture.backend.level = 80
        let lines = captureCoreLogs {
            controller.reconcile(protection(target: 30))
        }
        expect(fixture.writes(of: 30) == 4, "the drifted level gets one new write attempt")
        expect(lines == ["builtin_dim unverified attempt=1"], "the new episode starts at attempt one: \(lines)")
        expect(fixture.scheduler.liveCount == 1, "the new attempt schedules its retry")
    }

    /// A failing restore is not parked for writes: each later snapshot with no reasons still writes the baseline.
    private static func failedRestoreKeepsWritingOnEverySnapshot() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        fixture.backend.failWrites = true
        controller.reconcile(idle())
        fixture.scheduler.fireNext()
        fixture.scheduler.fireNext()
        let before = fixture.writes(of: 62.5)
        controller.reconcile(idle())
        controller.reconcile(idle())
        expect(fixture.writes(of: 62.5) == before + 2, "each later snapshot makes one more restore write attempt")
    }

    /// After quit, a snapshot with active reasons neither reads nor writes the level, acquires the owner, or starts holding.
    private static func reconcileAfterRestoreNowDoesNothing() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30, holdAuto: true))
        controller.restoreNow()
        fixture.clearRecords()
        let reads = fixture.backend.reads
        controller.reconcile(protection(target: 30, holdAuto: true))
        expect(fixture.backend.reads == reads, "a snapshot after quit reads no level")
        expect(fixture.log.events.isEmpty, "a snapshot after quit writes nothing, acquires nothing and reports nothing")
        expect(fixture.store.load() == .none, "a snapshot after quit writes no record")
        expect(!controller.isHolding, "a snapshot after quit does not start holding")
        expect(fixture.backend.level == 62.5, "the restored baseline is not dimmed again")
    }

    /// Quit also stops a controller that never dimmed, so a late snapshot cannot start the first dim.
    private static func reconcileAfterRestoreNowWithoutHoldingDoesNothing() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.restoreNow()
        fixture.clearRecords()
        let reads = fixture.backend.reads
        controller.reconcile(protection(target: 30, holdAuto: true))
        expect(fixture.backend.reads == reads && fixture.log.events.isEmpty, "a snapshot after an idle quit touches nothing")
        expect(fixture.store.load() == .none && !controller.isHolding, "a snapshot after an idle quit does not start holding")
    }

    /// While dimmed, identical snapshots inside the window trust the level that the dim verified and read the display no more.
    private static func identicalSnapshotsWithinTheWindowSkipTheRead() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        expect(fixture.backend.level == 30, "the dim verifies the level")
        fixture.clearRecords()
        let reads = fixture.backend.reads
        for _ in 0..<10 {
            controller.reconcile(protection(target: 30))
        }
        expect(fixture.backend.reads == reads, "ten identical snapshots inside the window read the display no more")
        expect(fixture.log.events.isEmpty, "ten identical snapshots inside the window write, acquire and report nothing")
        expect(controller.isHolding, "the controller keeps holding while the window is open")
    }

    /// Drift inside the window waits for the window to end. The first snapshot after six seconds reads and dims again.
    private static func driftIsCorrectedOnceTheWindowExpires() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        fixture.advance(by: 4)
        // The OS brightens the panel. Four seconds after the verification the window is still open.
        fixture.backend.level = 80
        let reads = fixture.backend.reads
        controller.reconcile(protection(target: 30))
        expect(fixture.backend.reads == reads, "drift inside the window is not read yet")
        expect(fixture.backend.level == 80, "drift inside the window is not corrected yet")
        fixture.advance(by: 2)
        controller.reconcile(protection(target: 30))
        expect(fixture.backend.level == 30, "drift is corrected once six seconds have passed")
        expect(fixture.backend.reads == reads + 2, "the expired window reads the level, then reads back the write")
        expect(fixture.writes(of: 30) == 2, "the drift gets one new write")
    }

    /// A changed target is a new decision, so it verifies at once even though the window from the last check is open.
    private static func changedTargetVerifiesImmediatelyInsideTheWindow() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        let reads = fixture.backend.reads
        controller.reconcile(protection(target: 20))
        expect(fixture.backend.reads == reads + 2, "a changed target reads the level and the readback at once")
        expect(fixture.backend.level == 20, "a changed target is applied inside the window")
    }

    /// An availability flip is a new decision. The display coming back verifies at once, even though the window is open.
    private static func availabilityFlipVerifiesImmediatelyInsideTheWindow() {
        let fixture = Fixture(level: 62.5)
        defer { fixture.cleanUp() }
        let controller = fixture.makeController()
        controller.reconcile(protection(target: 30))
        controller.reconcile(protection(target: 30, available: false))
        // The OS brightens the panel while the display is unavailable.
        fixture.backend.level = 80
        let reads = fixture.backend.reads
        controller.reconcile(protection(target: 30))
        expect(fixture.backend.reads == reads + 2, "the display coming back reads the level and the readback at once")
        expect(fixture.backend.level == 30, "the display coming back is re-dimmed inside the window")
    }
}
