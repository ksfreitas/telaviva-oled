import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("SessionLockTests: \(message)") }
}

/// Lets the main run loop turn so completions delivered on the main queue can run.
private func pump(_ seconds: TimeInterval = 0.3) {
    RunLoop.main.run(until: Date().addingTimeInterval(seconds))
}

/// Pumps the main run loop until the condition holds or the timeout elapses.
private func waitUntil(timeout: TimeInterval, _ condition: () -> Bool) {
    let deadline = Date().addingTimeInterval(timeout)
    while !condition() && Date() < deadline {
        pump(0.05)
    }
}

private struct Submission: Equatable {
    let token: UInt64
    let ok: Bool
    let error: String?
}

private final class CoordinatorRecorder {
    var submissions: [Submission] = []
    var logs: [String] = []
}

/// Records calls to `lockNow`. When `outcome` is set, each call completes synchronously with it;
/// otherwise the completion stays pending until the test completes the oldest call.
private final class FakeLocker: SessionLocking {
    var outcome: LockOutcome?
    private(set) var calls = 0
    private var pending: [(LockOutcome) -> Void] = []

    func lockNow(completion: @escaping (LockOutcome) -> Void) {
        calls += 1
        if let outcome = outcome {
            completion(outcome)
        } else {
            pending.append(completion)
        }
    }

    /// Completes the oldest pending call with the given outcome.
    func completeOldest(with outcome: LockOutcome) {
        guard !pending.isEmpty else { return }
        let completion = pending.removeFirst()
        completion(outcome)
    }
}

/// Counts how many times the fake lock symbol ran. A @convention(c) function cannot capture
/// context, so the counter lives at file scope.
private var lockSymbolRuns = 0

private let recordLockSymbolRun: @convention(c) () -> Void = { lockSymbolRuns += 1 }

private func makeCoordinator(_ locker: SessionLocking, _ recorder: CoordinatorRecorder) -> LockCoordinator {
    LockCoordinator(
        locker: locker,
        submit: { token, ok, error in
            recorder.submissions.append(Submission(token: token, ok: ok, error: error))
        },
        log: { line in recorder.logs.append(line) }
    )
}

@main
private struct SessionLockTests {
    static func main() {
        oneLockPerTokenAndRepeatedObservationDoesNothing()
        requestedFalseDoesNothingAndKeepsTheLastToken()
        newTokenActsAgain()
        outcomesMapToSubmitAndLog()
        sameTokenWhilePendingDoesNotLockAgain()
        missingSymbolCompletesUnavailableWithoutPolling()
        loadedSymbolRunsOnceAndPollingStopsWhenLocked()
        neverConfirmedCompletesNotConfirmed()
        print("Session lock tests passed")
    }

    // (1) One lock per token; observing the same token again does nothing.
    private static func oneLockPerTokenAndRepeatedObservationDoesNothing() {
        let locker = FakeLocker()
        locker.outcome = .locked
        let recorder = CoordinatorRecorder()
        let coordinator = makeCoordinator(locker, recorder)

        coordinator.observe(requested: true, token: 9)
        pump()
        coordinator.observe(requested: true, token: 9)
        coordinator.observe(requested: true, token: 9)
        pump()

        expect(locker.calls == 1, "the first observation of a token calls the locker exactly once")
        expect(recorder.submissions == [Submission(token: 9, ok: true, error: nil)],
               "the single lock of a token is submitted once as ok")
    }

    // (2) requested == false does nothing, and does not forget the last handled token.
    private static func requestedFalseDoesNothingAndKeepsTheLastToken() {
        let locker = FakeLocker()
        locker.outcome = .locked
        let recorder = CoordinatorRecorder()
        let coordinator = makeCoordinator(locker, recorder)

        coordinator.observe(requested: false, token: 7)
        coordinator.observe(requested: false, token: nil)
        pump()
        expect(locker.calls == 0 && recorder.submissions.isEmpty && recorder.logs.isEmpty,
               "requested false neither locks, submits, nor logs")

        coordinator.observe(requested: true, token: 7)
        pump()
        expect(locker.calls == 1, "a token observed after requested false is acted on once")

        coordinator.observe(requested: false, token: nil)
        coordinator.observe(requested: true, token: 7)
        pump()
        expect(locker.calls == 1, "requested false does not forget the last handled token")
    }

    // (3) A new token acts again; a nil token does nothing.
    private static func newTokenActsAgain() {
        let locker = FakeLocker()
        locker.outcome = .locked
        let recorder = CoordinatorRecorder()
        let coordinator = makeCoordinator(locker, recorder)

        coordinator.observe(requested: true, token: 1)
        pump()
        coordinator.observe(requested: true, token: 2)
        pump()
        coordinator.observe(requested: true, token: 5)
        pump()
        expect(locker.calls == 3, "each new token calls the locker again")
        expect(recorder.submissions.map(\.token) == [1, 2, 5], "each new token is submitted in order")

        coordinator.observe(requested: true, token: nil)
        pump()
        expect(locker.calls == 3, "a requested observation without a token does nothing")
    }

    // (4) Outcomes map to submit(ok, error) exactly, and each completion writes one log line
    // that carries no identifiers.
    private static func outcomesMapToSubmitAndLog() {
        let cases: [(LockOutcome, Submission)] = [
            (.locked, Submission(token: 4242, ok: true, error: nil)),
            (.unavailable, Submission(token: 4242, ok: false, error: "unavailable")),
            (.notConfirmed, Submission(token: 4242, ok: false, error: "not_confirmed")),
        ]
        for (outcome, expected) in cases {
            let locker = FakeLocker()
            locker.outcome = outcome
            let recorder = CoordinatorRecorder()
            let coordinator = makeCoordinator(locker, recorder)

            coordinator.observe(requested: true, token: 4242)
            pump()

            expect(recorder.submissions == [expected], "\(outcome) is submitted as \(expected)")
            expect(recorder.logs.count == 1 && !recorder.logs[0].isEmpty
                   && !recorder.logs[0].contains("4242"),
                   "\(outcome) writes one short log line without the token")
        }
    }

    // (5) While the first lock is pending, the same token does not call the locker again.
    private static func sameTokenWhilePendingDoesNotLockAgain() {
        let locker = FakeLocker()
        let recorder = CoordinatorRecorder()
        let coordinator = makeCoordinator(locker, recorder)

        coordinator.observe(requested: true, token: 3)
        coordinator.observe(requested: true, token: 3)
        coordinator.observe(requested: true, token: 3)
        pump()
        expect(locker.calls == 1, "the same token does not call the locker while the first call is pending")
        expect(recorder.submissions.isEmpty, "nothing is submitted before the pending lock completes")

        locker.completeOldest(with: .locked)
        pump()
        expect(recorder.submissions == [Submission(token: 3, ok: true, error: nil)],
               "the pending lock is submitted once it completes")

        coordinator.observe(requested: true, token: 3)
        pump()
        expect(locker.calls == 1, "the same token does not lock again after the pending lock completed")
    }

    // (6) SessionLocker without a loadable symbol completes unavailable and never polls.
    private static func missingSymbolCompletesUnavailableWithoutPolling() {
        var polls = 0
        var outcomes: [LockOutcome] = []
        let locker = SessionLocker(
            loadSymbol: { nil },
            isLocked: {
                polls += 1
                return true
            }
        )

        locker.lockNow { outcomes.append($0) }
        waitUntil(timeout: 1) { !outcomes.isEmpty }
        pump()

        expect(outcomes == [.unavailable], "a missing lock symbol completes unavailable")
        expect(polls == 0, "a missing lock symbol never polls the lock state")
    }

    // (7) A loaded symbol runs exactly once, and polling stops at the first confirmed lock.
    private static func loadedSymbolRunsOnceAndPollingStopsWhenLocked() {
        var polls = 0
        var outcomes: [LockOutcome] = []
        let runsBefore = lockSymbolRuns
        let locker = SessionLocker(
            loadSymbol: { recordLockSymbolRun },
            isLocked: {
                polls += 1
                if polls <= 2 {
                    return nil
                }
                return true
            }
        )

        locker.lockNow { outcomes.append($0) }
        waitUntil(timeout: 3) { !outcomes.isEmpty }
        pump()

        expect(outcomes == [.locked], "a confirmed lock completes locked")
        expect(lockSymbolRuns - runsBefore == 1, "the loaded lock symbol runs exactly once")
        expect(polls == 3, "polling continues through unknown answers and stops at the first lock")
    }

    // (8) A session that never reports locked completes notConfirmed after confirmWithin.
    private static func neverConfirmedCompletesNotConfirmed() {
        var polls = 0
        var outcomes: [LockOutcome] = []
        let locker = SessionLocker(
            loadSymbol: { recordLockSymbolRun },
            isLocked: {
                polls += 1
                return false
            },
            confirmWithin: 0.3,
            pollInterval: 0.05
        )

        locker.lockNow { outcomes.append($0) }
        waitUntil(timeout: 3) { !outcomes.isEmpty }
        pump()

        expect(outcomes == [.notConfirmed], "a lock that never confirms completes notConfirmed")
        expect(polls >= 2, "the lock state is polled repeatedly before giving up")
    }
}
