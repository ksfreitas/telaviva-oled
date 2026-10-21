import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("BuiltInAutoBrightnessOwnerTests: \(message)\n".utf8))
        exit(1)
    }
}

/// Ordered log shared by the fake backend. Every read and write appends one entry.
private final class EventLog {
    var events: [String] = []
}

/// Diagnostic lines captured through `DiagnosticLog.coreSink`.
private final class LineLog {
    var lines: [String] = []
}

private final class FakeBackend: BuiltInDisplayBackend {
    let log: EventLog
    var auto: Bool?
    /// When false the setter reports failure and leaves `auto` unchanged. The attempt is still logged.
    var setAutoSucceeds = true
    /// The hold file. When set, each setter call logs whether the file existed at that moment.
    var recordURL: URL?

    init(log: EventLog, auto: Bool?) {
        self.log = log
        self.auto = auto
    }

    func status() -> BuiltInDisplayStatus { BuiltInDisplayStatus(available: true, reason: nil, displayID: 1) }

    func isAsleep() -> Bool { false }

    func readLevel() -> Double? { nil }

    func writeLevel(_ percent: Double) -> Bool { false }

    func autoBrightnessEnabled() -> Bool? {
        log.events.append("read auto")
        return auto
    }

    func setAutoBrightness(_ enabled: Bool) -> Bool {
        let record: String
        if let recordURL {
            record = FileManager.default.fileExists(atPath: recordURL.path) ? "present" : "absent"
        } else {
            record = "untracked"
        }
        log.events.append("setAuto \(enabled) record=\(record)")
        guard setAutoSucceeds else { return false }
        auto = enabled
        return true
    }
}

/// A unique temp directory for the hold file, a fake backend and one owner built on them.
private final class Fixture {
    let directory: URL
    let events: EventLog
    let lines: LineLog
    let backend: FakeBackend
    let store: AutoBrightnessHoldStore
    let owner: BuiltInAutoBrightnessOwner
    let recordURL: URL

    init(auto: Bool?) {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("auto-brightness-owner-\(UUID().uuidString)", isDirectory: true)
        let recordURL = directory.appendingPathComponent("auto-brightness-hold.json")
        let events = EventLog()
        let lines = LineLog()
        let backend = FakeBackend(log: events, auto: auto)
        backend.recordURL = recordURL
        let store = AutoBrightnessHoldStore(directory: directory)
        self.directory = directory
        self.recordURL = recordURL
        self.events = events
        self.lines = lines
        self.backend = backend
        self.store = store
        self.owner = BuiltInAutoBrightnessOwner(backend: backend, store: store)
        DiagnosticLog.coreSink = { line in lines.lines.append(line) }
    }

    var recordExists: Bool { FileManager.default.fileExists(atPath: recordURL.path) }

    func cleanUp() {
        try? FileManager.default.removeItem(at: directory)
    }
}

private func withFixture(auto: Bool?, _ body: (Fixture) -> Void) {
    let fixture = Fixture(auto: auto)
    defer { fixture.cleanUp() }
    body(fixture)
}

private func storedRecord(_ fixture: Fixture) -> AutoBrightnessHoldRecord? {
    guard let data = try? Data(contentsOf: fixture.recordURL) else { return nil }
    return try? JSONDecoder().decode(AutoBrightnessHoldRecord.self, from: data)
}

@main
private struct BuiltInAutoBrightnessOwnerTests {
    static func main() {
        firstAcquireWithAutoOnSavesRecordBeforeTurningItOff()
        storedRecordHoldsVersionAndOriginalEnabled()
        secondHolderNeitherReadsNorWrites()
        releasingOneOfTwoHoldersWritesNothing()
        releasingLastWithStillOffWritesAutoOnAndDeletesRecord()
        releasingLastWhenUserTurnedOnMeanwhileWritesNothingAndDeletesRecord()
        releasingLastWhenReadIsUnknownRestoresAndDeletesRecord()
        failedRestoreOnReleaseKeepsRecordForLaunchRecovery()
        failedRestoreThenAcquireJoinsOwedRestoreWithoutReadingOrWriting()
        failedRestoreThenUserTurnedOnStillSuspendsAndReleaseWritesNothing()
        failedRestoreWithUnknownReadKeepsRecordAndOwesRestore()
        releasedInEitherOrderRestoresExactlyOnce()
        acquireWithAutoOffReturnsAlreadyOffAndReleaseWritesNothing()
        acquireWithUnknownStateReturnsUnknownAndWritesNothing()
        failingSetterReturnsNotChangedAndDeletesRecord()
        failingRecordSaveReturnsNotChangedWithoutWriting()
        idempotentAcquireByTheSameHolderDoesNotReadAgain()
        releaseWithoutAcquireWritesNothing()
        recoverWithLeftoverFileAndAutoOffTurnsItOnAndDeletesRecord()
        recoverWithLeftoverFileAndAutoOnWritesNothingAndDeletesRecord()
        recoverWithLeftoverFileAndUnknownReadTurnsItOnAndDeletesRecord()
        recoveryWithFailingSetterKeepsRecord()
        failedRecoveryThenAcquireJoinsOwedRestoreWithoutReadingOrWriting()
        corruptRecordIsDeletedWithoutWrites()
        handleForwardsAcquireAndReleaseForItsHolder()
        print("Built-in auto-brightness owner tests passed")
    }

    private static func firstAcquireWithAutoOnSavesRecordBeforeTurningItOff() {
        withFixture(auto: true) { fixture in
            let outcome = fixture.owner.acquire(.protection)
            expect(outcome == .suspended, "first acquire with auto on should return suspended, got \(outcome)")
            expect(fixture.events.events == ["read auto", "setAuto false record=present"],
                   "record must exist before the setter runs: \(fixture.events.events)")
            expect(fixture.backend.auto == false, "auto brightness should be off after acquire")
            expect(fixture.recordExists, "hold file should remain until release")
        }
    }

    private static func storedRecordHoldsVersionAndOriginalEnabled() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.calibration)
            expect(storedRecord(fixture) == AutoBrightnessHoldRecord(original_enabled: true),
                   "hold file should record original_enabled true at version 1")
        }
    }

    private static func secondHolderNeitherReadsNorWrites() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.calibration)
            let before = fixture.events.events
            let outcome = fixture.owner.acquire(.protection)
            expect(outcome == .suspended, "second holder should receive the first outcome, got \(outcome)")
            expect(fixture.events.events == before, "second holder must not read or write: \(fixture.events.events)")
        }
    }

    private static func releasingOneOfTwoHoldersWritesNothing() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.calibration)
            _ = fixture.owner.acquire(.protection)
            let before = fixture.events.events
            fixture.owner.release(.calibration)
            expect(fixture.events.events == before, "releasing one of two holders must not write: \(fixture.events.events)")
            expect(fixture.recordExists, "hold file must remain while a holder is left")
        }
    }

    private static func releasingLastWithStillOffWritesAutoOnAndDeletesRecord() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            fixture.owner.release(.protection)
            expect(fixture.events.events == [
                "read auto", "setAuto false record=present", "read auto", "setAuto true record=present",
            ], "last release should read and then restore before deleting: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "auto brightness should be restored on")
            expect(!fixture.recordExists, "hold file should be deleted after restore")
        }
    }

    private static func releasingLastWhenUserTurnedOnMeanwhileWritesNothingAndDeletesRecord() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            fixture.backend.auto = true
            let before = fixture.events.events
            fixture.owner.release(.protection)
            expect(fixture.events.events == before + ["read auto"],
                   "release must read and then skip the write when the user turned it on: \(fixture.events.events)")
            expect(!fixture.recordExists, "hold file should be deleted even without a write")
        }
    }

    /// The app turned the setting off, so an unknown read is not evidence that the user turned it on. Only a fresh read
    /// of "on" skips the restore.
    private static func releasingLastWhenReadIsUnknownRestoresAndDeletesRecord() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            fixture.backend.auto = nil
            let before = fixture.events.events
            fixture.owner.release(.protection)
            expect(fixture.events.events == before + ["read auto", "setAuto true record=present"],
                   "release with an unknown read should read and then restore once: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "auto brightness should be restored when the read is unknown")
            expect(!fixture.recordExists, "hold file should be deleted after the restore")
        }
    }

    private static func failedRestoreOnReleaseKeepsRecordForLaunchRecovery() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            fixture.backend.setAutoSucceeds = false
            fixture.owner.release(.protection)
            expect(fixture.events.events == [
                "read auto", "setAuto false record=present", "read auto", "setAuto true record=present",
            ], "the failed restore should be attempted once: \(fixture.events.events)")
            expect(fixture.backend.auto == false, "auto brightness should stay off when the restore fails")
            expect(fixture.recordExists, "hold file must remain when the restore fails")
            expect(fixture.lines.lines.contains("auto_brightness_restore_failed"),
                   "failed restore should log auto_brightness_restore_failed")
            // A relaunch builds a new owner. Its recovery retries the restore and deletes the record once it succeeds.
            fixture.backend.setAutoSucceeds = true
            let relaunched = BuiltInAutoBrightnessOwner(backend: fixture.backend, store: fixture.store)
            relaunched.recoverIfNeeded()
            expect(fixture.events.events == [
                "read auto", "setAuto false record=present", "read auto", "setAuto true record=present",
                "read auto", "setAuto true record=present",
            ], "launch recovery should retry the restore: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "launch recovery should restore automatic brightness")
            expect(!fixture.recordExists, "launch recovery should delete the record after a successful restore")
        }
    }

    private static func failedRestoreThenAcquireJoinsOwedRestoreWithoutReadingOrWriting() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            fixture.backend.setAutoSucceeds = false
            fixture.owner.release(.protection)
            expect(fixture.recordExists, "hold file must remain when the restore fails")
            // The setter works again. The setting reads off only because the restore failed, so the next acquire must
            // join the owed restore instead of reading that off as the user's choice.
            fixture.backend.setAutoSucceeds = true
            let before = fixture.events.events
            let outcome = fixture.owner.acquire(.calibration)
            expect(outcome == .suspended, "acquire after a failed restore should return suspended, got \(outcome)")
            expect(fixture.events.events == before,
                   "acquire after a failed restore must neither read nor write: \(fixture.events.events)")
            expect(fixture.recordExists, "acquire after a failed restore must keep the record")
            fixture.owner.release(.calibration)
            expect(fixture.events.events == before + ["read auto", "setAuto true record=present"],
                   "release should read and then restore exactly once: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "release should restore automatic brightness")
            expect(!fixture.recordExists, "release should delete the record after the restore")
        }
    }

    private static func failedRestoreThenUserTurnedOnStillSuspendsAndReleaseWritesNothing() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            fixture.backend.setAutoSucceeds = false
            fixture.owner.release(.protection)
            // The user turns automatic brightness on while the restore is still owed.
            fixture.backend.auto = true
            let before = fixture.events.events
            let outcome = fixture.owner.acquire(.calibration)
            expect(outcome == .suspended, "acquire after a failed restore should return suspended, got \(outcome)")
            expect(fixture.events.events == before,
                   "acquire after a failed restore must neither read nor write: \(fixture.events.events)")
            fixture.owner.release(.calibration)
            expect(fixture.events.events == before + ["read auto"],
                   "release must read and then skip the write when the user turned it on: \(fixture.events.events)")
            expect(!fixture.recordExists, "release should delete the record when the user turned it on")
            expect(fixture.backend.auto == true, "automatic brightness should stay on")
        }
    }

    private static func failedRestoreWithUnknownReadKeepsRecordAndOwesRestore() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            // The read is unknown at release and the restore fails. The record must stay so the restore is still owed.
            fixture.backend.auto = nil
            fixture.backend.setAutoSucceeds = false
            fixture.owner.release(.protection)
            expect(fixture.events.events == [
                "read auto", "setAuto false record=present", "read auto", "setAuto true record=present",
            ], "the failed restore should be attempted once after an unknown read: \(fixture.events.events)")
            expect(fixture.recordExists, "hold file must remain when the restore fails after an unknown read")
            expect(fixture.lines.lines.contains("auto_brightness_restore_failed"),
                   "failed restore should log auto_brightness_restore_failed")
            // The setter works again. The setting reads off only because the restore failed, so the next acquire must
            // join the owed restore instead of reading that off as the user's choice.
            fixture.backend.setAutoSucceeds = true
            let before = fixture.events.events
            let outcome = fixture.owner.acquire(.calibration)
            expect(outcome == .suspended, "acquire after a failed restore should return suspended, got \(outcome)")
            expect(fixture.events.events == before,
                   "acquire after a failed restore must neither read nor write: \(fixture.events.events)")
            expect(fixture.recordExists, "acquire after a failed restore must keep the record")
            fixture.owner.release(.calibration)
            expect(fixture.events.events == before + ["read auto", "setAuto true record=present"],
                   "release should read and then restore exactly once: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "release should restore automatic brightness")
            expect(!fixture.recordExists, "release should delete the record after the restore")
        }
    }

    private static func releasedInEitherOrderRestoresExactlyOnce() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.calibration)
            _ = fixture.owner.acquire(.protection)
            fixture.owner.release(.protection)
            fixture.owner.release(.calibration)
            let restores = fixture.events.events.filter { $0 == "setAuto true record=present" }
            expect(restores.count == 1, "automatic brightness should be restored exactly once: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "auto brightness should be on after both holders release")
        }
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.calibration)
            _ = fixture.owner.acquire(.protection)
            fixture.owner.release(.calibration)
            fixture.owner.release(.protection)
            let restores = fixture.events.events.filter { $0 == "setAuto true record=present" }
            expect(restores.count == 1, "automatic brightness should be restored exactly once in reverse order: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "auto brightness should be on after both holders release in reverse order")
        }
    }

    private static func acquireWithAutoOffReturnsAlreadyOffAndReleaseWritesNothing() {
        withFixture(auto: false) { fixture in
            let outcome = fixture.owner.acquire(.protection)
            expect(outcome == .alreadyOff, "acquire with auto off should return alreadyOff, got \(outcome)")
            expect(fixture.events.events == ["read auto"], "acquire with auto off must only read: \(fixture.events.events)")
            expect(!fixture.recordExists, "acquire with auto off must not save a record")
            fixture.owner.release(.protection)
            expect(fixture.events.events == ["read auto"], "release after alreadyOff must not write: \(fixture.events.events)")
            expect(fixture.backend.auto == false, "auto brightness should stay off")
        }
    }

    private static func acquireWithUnknownStateReturnsUnknownAndWritesNothing() {
        withFixture(auto: nil) { fixture in
            let outcome = fixture.owner.acquire(.calibration)
            expect(outcome == .unknown, "acquire with an unknown state should return unknown, got \(outcome)")
            expect(fixture.events.events == ["read auto"], "unknown acquire must only read: \(fixture.events.events)")
            expect(!fixture.recordExists, "unknown acquire must not save a record")
            expect(fixture.lines.lines.contains("auto_brightness_unknown"), "unknown acquire should log auto_brightness_unknown")
            fixture.owner.release(.calibration)
            expect(fixture.events.events == ["read auto"], "release after unknown must not write: \(fixture.events.events)")
        }
    }

    private static func failingSetterReturnsNotChangedAndDeletesRecord() {
        withFixture(auto: true) { fixture in
            fixture.backend.setAutoSucceeds = false
            let outcome = fixture.owner.acquire(.protection)
            expect(outcome == .notChanged, "failing setter should return notChanged, got \(outcome)")
            expect(fixture.events.events == ["read auto", "setAuto false record=present"],
                   "failing setter should be attempted once after the save: \(fixture.events.events)")
            expect(!fixture.recordExists, "record must be deleted when the setter fails")
            expect(fixture.lines.lines.contains("auto_brightness_not_changed"), "failing setter should log auto_brightness_not_changed")
            fixture.owner.release(.protection)
            expect(fixture.events.events.count == 2, "release after notChanged must not write: \(fixture.events.events)")
        }
    }

    private static func failingRecordSaveReturnsNotChangedWithoutWriting() {
        withFixture(auto: true) { fixture in
            // A regular file where the hold directory should be makes the save fail before any write.
            expect(FileManager.default.createFile(atPath: fixture.directory.path, contents: Data("blocker".utf8)),
                   "fixture should block the hold directory")
            let outcome = fixture.owner.acquire(.protection)
            expect(outcome == .notChanged, "failing save should return notChanged, got \(outcome)")
            expect(fixture.events.events == ["read auto"], "failing save must not call the setter: \(fixture.events.events)")
            expect(fixture.lines.lines.contains("auto_brightness_not_changed"), "failing save should log auto_brightness_not_changed")
        }
    }

    private static func idempotentAcquireByTheSameHolderDoesNotReadAgain() {
        withFixture(auto: true) { fixture in
            _ = fixture.owner.acquire(.protection)
            let before = fixture.events.events
            let outcome = fixture.owner.acquire(.protection)
            expect(outcome == .suspended, "repeated acquire should return the held outcome, got \(outcome)")
            expect(fixture.events.events == before, "repeated acquire must not read or write: \(fixture.events.events)")
        }
    }

    private static func releaseWithoutAcquireWritesNothing() {
        withFixture(auto: true) { fixture in
            fixture.owner.release(.calibration)
            expect(fixture.events.events.isEmpty, "release without acquire must not touch the backend: \(fixture.events.events)")
        }
    }

    private static func recoverWithLeftoverFileAndAutoOffTurnsItOnAndDeletesRecord() {
        withFixture(auto: false) { fixture in
            expect(fixture.store.save(AutoBrightnessHoldRecord(original_enabled: true)), "fixture should write the leftover record")
            fixture.owner.recoverIfNeeded()
            expect(fixture.events.events == ["read auto", "setAuto true record=present"],
                   "recovery with auto off should read and then turn it on: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "recovery should turn automatic brightness on")
            expect(!fixture.recordExists, "recovery should delete the record")
        }
    }

    private static func recoverWithLeftoverFileAndAutoOnWritesNothingAndDeletesRecord() {
        withFixture(auto: true) { fixture in
            expect(fixture.store.save(AutoBrightnessHoldRecord(original_enabled: true)), "fixture should write the leftover record")
            fixture.owner.recoverIfNeeded()
            expect(fixture.events.events == ["read auto"], "recovery with auto on must only read: \(fixture.events.events)")
            expect(!fixture.recordExists, "recovery should delete the record")
        }
    }

    private static func recoverWithLeftoverFileAndUnknownReadTurnsItOnAndDeletesRecord() {
        withFixture(auto: nil) { fixture in
            expect(fixture.store.save(AutoBrightnessHoldRecord(original_enabled: true)), "fixture should write the leftover record")
            fixture.owner.recoverIfNeeded()
            expect(fixture.events.events == ["read auto", "setAuto true record=present"],
                   "recovery with an unknown read should read and then turn it on: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "recovery should turn automatic brightness on")
            expect(!fixture.recordExists, "recovery should delete the record")
        }
    }

    private static func recoveryWithFailingSetterKeepsRecord() {
        withFixture(auto: false) { fixture in
            expect(fixture.store.save(AutoBrightnessHoldRecord(original_enabled: true)), "fixture should write the leftover record")
            fixture.backend.setAutoSucceeds = false
            fixture.owner.recoverIfNeeded()
            expect(fixture.events.events == ["read auto", "setAuto true record=present"],
                   "failed recovery should attempt the restore once: \(fixture.events.events)")
            expect(fixture.backend.auto == false, "auto brightness should stay off when recovery fails")
            expect(storedRecord(fixture) == AutoBrightnessHoldRecord(original_enabled: true),
                   "recovery must keep the record unchanged when the restore fails")
            expect(fixture.lines.lines.contains("auto_brightness_restore_failed"),
                   "failed recovery should log auto_brightness_restore_failed")
            expect(!fixture.lines.lines.contains("auto_brightness_recovered"),
                   "failed recovery must not log auto_brightness_recovered")
        }
    }

    private static func failedRecoveryThenAcquireJoinsOwedRestoreWithoutReadingOrWriting() {
        withFixture(auto: false) { fixture in
            expect(fixture.store.save(AutoBrightnessHoldRecord(original_enabled: true)), "fixture should write the leftover record")
            fixture.backend.setAutoSucceeds = false
            fixture.owner.recoverIfNeeded()
            expect(fixture.recordExists, "failed recovery must keep the record")
            // The setter works again. The setting reads off only because the recovery restore failed, so the acquire must
            // join the owed restore instead of reading that off as the user's choice.
            fixture.backend.setAutoSucceeds = true
            let before = fixture.events.events
            let outcome = fixture.owner.acquire(.calibration)
            expect(outcome == .suspended, "acquire after a failed recovery should return suspended, got \(outcome)")
            expect(fixture.events.events == before,
                   "acquire after a failed recovery must neither read nor write: \(fixture.events.events)")
            expect(fixture.recordExists, "acquire after a failed recovery must keep the record")
            fixture.owner.release(.calibration)
            expect(fixture.events.events == before + ["read auto", "setAuto true record=present"],
                   "release should read and then restore exactly once: \(fixture.events.events)")
            expect(fixture.backend.auto == true, "release should restore automatic brightness")
            expect(!fixture.recordExists, "release should delete the record after the restore")
        }
    }

    private static func corruptRecordIsDeletedWithoutWrites() {
        withFixture(auto: false) { fixture in
            expect((try? FileManager.default.createDirectory(at: fixture.directory, withIntermediateDirectories: true)) != nil,
                   "fixture should create the hold directory")
            expect(FileManager.default.createFile(atPath: fixture.recordURL.path, contents: Data("{ not json".utf8)),
                   "fixture should write a corrupt record")
            fixture.owner.recoverIfNeeded()
            expect(fixture.events.events.isEmpty, "corrupt record must not touch the backend: \(fixture.events.events)")
            expect(!fixture.recordExists, "corrupt record should be deleted")
            expect(fixture.lines.lines.contains("auto_brightness_record_unreadable"), "corrupt record should be logged")
        }
    }

    private static func handleForwardsAcquireAndReleaseForItsHolder() {
        withFixture(auto: true) { fixture in
            let calibration = fixture.owner.handle(for: .calibration)
            let protection = fixture.owner.handle(for: .protection)
            expect(calibration.acquire() == .suspended, "calibration handle should acquire and suspend")
            expect(protection.acquire() == .suspended, "protection handle should join the hold")
            expect(fixture.events.events == ["read auto", "setAuto false record=present"],
                   "handles must share one read and one write: \(fixture.events.events)")
            protection.release()
            expect(fixture.events.events.count == 2, "releasing the protection handle must not write while calibration holds")
            calibration.release()
            expect(fixture.events.events.last == "setAuto true record=present",
                   "releasing the last handle should restore automatic brightness: \(fixture.events.events)")
            expect(!fixture.recordExists, "last handle release should delete the record")
        }
    }
}
