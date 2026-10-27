import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("KeepAwakeTests: \(message)\n".utf8))
        exit(1)
    }
}

private final class FakeKeepAwakeBackend: KeepAwakeBackend {
    var safety: KeepAwakeSafety = .eligible
    var postingPermission = true
    var permissionRequests = 0
    var acquireCount = 0
    var releaseCount = 0
    var postedCount = 0
    var assertionResult = true
    var postResult = true

    func safetyState() -> KeepAwakeSafety { safety }
    func hasPostingPermission() -> Bool { postingPermission }
    func requestPostingPermission() -> Bool {
        permissionRequests += 1
        postingPermission = true
        return postingPermission
    }
    func acquireAssertions() -> Bool { acquireCount += 1; return assertionResult }
    func releaseAssertions() { releaseCount += 1 }
    func postActivity() -> Bool { postedCount += 1; return postResult }
}

private final class FakeKeepAwakeScheduler: KeepAwakeScheduling {
    private final class Registration: KeepAwakeCancelable {
        let delay: TimeInterval
        let repeating: Bool
        let action: () -> Void
        var canceled = false
        init(delay: TimeInterval, repeating: Bool, action: @escaping () -> Void) {
            self.delay = delay; self.repeating = repeating; self.action = action
        }
        func cancel() { canceled = true }
    }
    private var registrations: [Registration] = []

    func repeating(every interval: TimeInterval, _ action: @escaping () -> Void) -> KeepAwakeCancelable {
        let registration = Registration(delay: interval, repeating: true, action: action)
        registrations.append(registration)
        return registration
    }

    func once(after delay: TimeInterval, _ action: @escaping () -> Void) -> KeepAwakeCancelable {
        let registration = Registration(delay: delay, repeating: false, action: action)
        registrations.append(registration)
        return registration
    }

    var intervals: [TimeInterval] { registrations.filter { $0.repeating && !$0.canceled }.map(\.delay) }
    var liveExpiryDelays: [TimeInterval] { registrations.filter { !$0.repeating && !$0.canceled }.map(\.delay) }
    var allCanceled: Bool { registrations.allSatisfy(\.canceled) }
    func firePulse() { registrations.last(where: { $0.repeating && !$0.canceled })?.action() }
    func fireExpiry() { registrations.last(where: { !$0.repeating && !$0.canceled })?.action() }
}

private struct FakeKeepAwakeClock {
    var value: TimeInterval = 100
    mutating func advance(_ seconds: TimeInterval) { value += seconds }
}

private func runLeaseTests() {
    var clock = FakeKeepAwakeClock()
    let backend = FakeKeepAwakeBackend()
    let scheduler = FakeKeepAwakeScheduler()
    let controller = KeepAwakeController(backend: backend, scheduler: scheduler, now: { clock.value },
                                         wallNowMilliseconds: { 0 })

    expect(scheduler.intervals.isEmpty, "disabled startup does not keep a timer running")
    expect(backend.acquireCount == 0 && backend.postedCount == 0, "startup does not inhibit without evidence")
    controller.setSessionEligible(true)
    controller.update(enabled: true, requested: true, generation: 1, observationID: 10, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 1 && backend.postedCount == 1, "fresh present token starts assertions and an immediate pulse")
    expect(scheduler.intervals == [1] && scheduler.liveExpiryDelays == [3], "active lease owns separate pulse and exact expiry timers")
    clock.advance(1)
    scheduler.firePulse()
    expect(backend.postedCount == 2, "worker posts activity without a snapshot refresh")

    controller.update(enabled: true, requested: true, generation: 1, observationID: 10, validForMilliseconds: 3_000)
    clock.advance(1.5)
    scheduler.firePulse()
    expect(backend.postedCount == 3, "same token remains active before original deadline")
    clock.advance(0.6)
    scheduler.fireExpiry()
    expect(backend.releaseCount == 1, "same-token snapshots cannot extend the original deadline")
    let postsAfterExpiry = backend.postedCount
    clock.advance(5)
    scheduler.firePulse()
    expect(backend.postedCount == postsAfterExpiry, "no activity is posted after expiry")
    controller.update(enabled: true, requested: true, generation: 1, observationID: 10, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 1, "a revoked token cannot resurrect itself")

    controller.update(enabled: true, requested: true, generation: 1, observationID: 11, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 2, "a newer positive observation replaces the old lease")
    controller.update(enabled: true, requested: false, generation: 1, observationID: 0, validForMilliseconds: 0)
    expect(backend.releaseCount == 2, "first absent snapshot with reset observation id releases current inhibition")
    controller.update(enabled: true, requested: true, generation: 1, observationID: 11, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 2, "a revoked positive token cannot replay after absence")
    controller.update(enabled: true, requested: true, generation: 1, observationID: 12, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 3, "new positive observation reacquires after absence")
    controller.setSessionEligible(false)
    expect(backend.releaseCount == 3, "session transition releases immediately")
    controller.update(enabled: true, requested: false, generation: 2, observationID: 0, validForMilliseconds: 0)
    controller.setSessionEligible(true)
    controller.update(enabled: true, requested: true, generation: 1, observationID: 13, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 3, "resume requires a newer core generation")
    controller.update(enabled: true, requested: true, generation: 2, observationID: 1, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 3, "inactive generation cannot satisfy the resume floor")
    controller.update(enabled: true, requested: true, generation: 3, observationID: 14, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 4, "fresh generation after the inactive acknowledgement reactivates inhibition")

    backend.safety = .screensaverActive
    scheduler.firePulse()
    expect(backend.releaseCount == 4, "active manual screensaver vetoes further effects")
    let afterSaver = backend.postedCount
    clock.advance(1)
    scheduler.firePulse()
    expect(backend.postedCount == afterSaver, "manual screensaver prevents activity posting")
    backend.safety = .eligible
    controller.safetyDidChange()
    controller.update(enabled: true, requested: true, generation: 3, observationID: 1, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 4, "manual screensaver exit cannot restore its revoked token")
    controller.update(enabled: true, requested: true, generation: 4, observationID: 15, validForMilliseconds: 3_000)
    expect(backend.acquireCount == 5, "new generation after the screensaver veto can reacquire")
    backend.safety = .screensaverStateUnknown
    controller.safetyDidChange()
    expect(backend.releaseCount == 5, "uncertain screensaver state still releases activity effects")
    let afterUnknownSaver = backend.postedCount
    scheduler.firePulse()
    expect(backend.postedCount == afterUnknownSaver, "uncertain screensaver state never posts activity")
    controller.update(enabled: false, requested: false, generation: 4, observationID: 1, validForMilliseconds: 0)
    expect(backend.releaseCount == 5, "disable cleanup is idempotent")
    controller.shutdown()
    expect(scheduler.allCanceled, "shutdown cancels every worker and expiry timer")
}

private func runPermissionTests() {
    let backend = FakeKeepAwakeBackend()
    backend.postingPermission = false
    let controller = KeepAwakeController(backend: backend, scheduler: FakeKeepAwakeScheduler(), now: { 0 },
                                         wallNowMilliseconds: { 0 })
    controller.setSessionEligible(true)
    expect(backend.permissionRequests == 0, "startup never opens a permission prompt")
    controller.update(enabled: true, requested: true, generation: 1, observationID: 1, validForMilliseconds: 2_000)
    expect(backend.acquireCount == 0 && backend.postedCount == 0, "denied event posting blocks all OS effects")
    expect(controller.status == .permissionRequired, "denied event posting is exposed as unavailable")
    controller.requestPostingPermissionFromUserAction()
    expect(backend.permissionRequests == 1 && backend.postedCount == 1, "only an explicit user action requests permission")
    controller.shutdown()
}

private func runStaleGenerationTests() {
    let backend = FakeKeepAwakeBackend()
    let controller = KeepAwakeController(backend: backend, scheduler: FakeKeepAwakeScheduler(), now: { 0 },
                                         wallNowMilliseconds: { 0 })
    controller.setSessionEligible(true)
    controller.update(enabled: true, requested: true, generation: 10, observationID: 1, validForMilliseconds: 2_000)
    expect(backend.acquireCount == 1, "initial high-generation evidence is accepted")
    controller.update(enabled: true, requested: false, generation: 20, observationID: 0, validForMilliseconds: 0)
    let acquisitionsAfterAbsence = backend.acquireCount
    controller.update(enabled: true, requested: true, generation: 15, observationID: 2, validForMilliseconds: 2_000)
    expect(backend.acquireCount == acquisitionsAfterAbsence,
           "an unseen positive sample older than the latest negative generation is rejected")
    controller.update(enabled: true, requested: true, generation: 20, observationID: 1, validForMilliseconds: 2_000)
    expect(backend.acquireCount == acquisitionsAfterAbsence + 1,
           "fresh positive evidence at the latest observed generation remains eligible")
    controller.shutdown()
}

private func runGuardOverrideTests() {
    expect(MacKeepAwakeBackend.resolveGuardState(override: true, live: { false }) == true,
           "a positive lock or saver notification vetoes immediately")
    expect(MacKeepAwakeBackend.resolveGuardState(override: false, live: { true }) == true,
           "a stale negative notification cannot override a live active lock or saver")
    expect(MacKeepAwakeBackend.resolveGuardState(override: false, live: { nil }) == nil,
           "a negative notification cannot turn an unknown live guard into eligibility")
    expect(MacKeepAwakeBackend.resolveGuardState(override: false, live: { false }) == false,
           "a negative notification clears its local veto when the live guard is known inactive")
}

private func runCameraEligibilityTests() {
    expect(KeepAwakeSafety.cameraMonitoringAllowed(for: .eligible), "active unlocked session permits monitoring")
    expect(KeepAwakeSafety.cameraMonitoringAllowed(for: .screensaverActive), "manual screensaver vetoes inhibition without stopping local monitoring")
    expect(KeepAwakeSafety.cameraMonitoringAllowed(for: .screensaverStateUnknown), "unknown screensaver query does not stop monitoring")
    expect(KeepAwakeSafety.cameraMonitoringAllowed(for: .displayAsleep), "display sleep vetoes inhibition without stopping camera demand")
    expect(KeepAwakeSafety.cameraMonitoringAllowed(for: .lidClosed), "clamshell use keeps camera demand available for an external camera")
    expect(!KeepAwakeSafety.cameraMonitoringAllowed(for: .sessionInactive), "inactive or locked session stops monitoring")
    expect(!KeepAwakeSafety.cameraMonitoringAllowed(for: .sessionStateUnknown), "unknown lock state keeps camera closed")
    expect(!KeepAwakeSafety.cameraMonitoringAllowed(for: .systemSleeping), "system sleep stops monitoring")

    expect(MacKeepAwakeBackend.resolveSafetyState(sessionActive: true, sessionLocked: false,
                                                   screensaverActive: nil, displayAsleep: false,
                                                   systemSleeping: false) == .screensaverStateUnknown,
           "an uncertain private screensaver query gets its own discriminator")
    expect(MacKeepAwakeBackend.resolveSafetyState(sessionActive: true, sessionLocked: nil,
                                                   screensaverActive: false, displayAsleep: false,
                                                   systemSleeping: false) == .sessionStateUnknown,
           "an uncertain lock query remains a fail-closed camera gate")
    expect(MacKeepAwakeBackend.resolveSafetyState(sessionActive: true, sessionLocked: false,
                                                   screensaverActive: true, displayAsleep: false,
                                                   systemSleeping: false) == .screensaverActive,
           "an explicitly active screensaver remains a strict effect veto")
    expect(MacKeepAwakeBackend.resolveSafetyState(sessionActive: true, sessionLocked: false,
                                                   screensaverActive: false, displayAsleep: true,
                                                   systemSleeping: false) == .displayAsleep,
           "display sleep remains a strict effect veto")
    expect(MacKeepAwakeBackend.resolveSafetyState(sessionActive: true, sessionLocked: true,
                                                   screensaverActive: false, displayAsleep: false,
                                                   systemSleeping: false) == .sessionInactive,
           "a locked session remains a camera and effect veto")
    expect(MacKeepAwakeBackend.resolveSafetyState(sessionActive: true, sessionLocked: true,
                                                   screensaverActive: false, displayAsleep: false,
                                                   systemSleeping: true) == .systemSleeping,
           "actual system sleep takes precedence over other guard diagnostics")
}

private func runGuardResumeFenceTests() {
    var wallClock: UInt64 = 1_000
    let backend = FakeKeepAwakeBackend()
    let controller = KeepAwakeController(backend: backend, scheduler: FakeKeepAwakeScheduler(), now: { 0 },
                                         wallNowMilliseconds: { wallClock })
    controller.setSessionEligible(true)
    controller.update(enabled: true, requested: true, generation: 7, observationID: 900,
                      validForMilliseconds: 5_000)
    expect(backend.acquireCount == 1, "fresh evidence starts inhibition before a safety veto")

    backend.safety = .displayAsleep
    controller.safetyDidChange()
    expect(backend.releaseCount == 1, "display-off immediately releases effects")
    controller.update(enabled: true, requested: true, generation: 7, observationID: 1_500,
                      validForMilliseconds: 5_000)
    expect(backend.acquireCount == 1, "camera evidence during display-off cannot reacquire effects")

    wallClock = 2_000
    backend.safety = .eligible
    controller.safetyDidChange()
    controller.update(enabled: true, requested: true, generation: 7, observationID: 1_999,
                      validForMilliseconds: 5_000)
    expect(backend.acquireCount == 1, "evidence captured before display wake cannot pass the resume fence")
    controller.update(enabled: true, requested: true, generation: 7, observationID: 2_001,
                      validForMilliseconds: 5_000)
    expect(backend.acquireCount == 2, "new same-generation evidence after display wake reacquires inhibition")
    controller.shutdown()
}

private func runSessionReconciliationTests() {
    // Enabling keep-awake can expose a stale false session gate in the core. Its generic suspended
    // status is the signal to reconcile from the current local guard state.
    let resumed = KeepAwakeSessionReconciliation.sessionValueToResubmit(
        keepAwakeEnabled: true, cameraMode: "suspended", displayStatus: "suspended",
        localMonitoringAllowed: true
    )
    expect(resumed == true, "enabling keep-awake reconciles a stale inactive core session while unlocked")

    // A real lock must remain authoritative even when the core still reports the same generic state.
    let lockedResume = KeepAwakeSessionReconciliation.sessionValueToResubmit(
        keepAwakeEnabled: true, cameraMode: "suspended", displayStatus: "suspended",
        localMonitoringAllowed: false
    )
    expect(lockedResume == nil, "a locally locked session is never reopened by reconciliation")

    expect(KeepAwakeSessionReconciliation.sessionValueToResubmit(
        keepAwakeEnabled: false, cameraMode: "disabled", displayStatus: "disabled",
        localMonitoringAllowed: true
    ) == nil, "turning keep-awake off does not trigger a session reconciliation")
    expect(KeepAwakeSessionReconciliation.sessionValueToResubmit(
        keepAwakeEnabled: true, cameraMode: "suspended", displayStatus: "suspended_display_sleep",
        localMonitoringAllowed: true
    ) == nil, "display sleep suspension does not trigger a session reconciliation")
}

@main
private struct KeepAwakeTestMain {
    static func main() {
        runLeaseTests()
        runPermissionTests()
        runStaleGenerationTests()
        runGuardOverrideTests()
        runCameraEligibilityTests()
        runGuardResumeFenceTests()
        runSessionReconciliationTests()
        print("KeepAwakeTests: all checks passed")
    }
}
