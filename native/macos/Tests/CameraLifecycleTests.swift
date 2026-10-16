import Foundation

private struct FakeClock {
    var now: TimeInterval = 0
    mutating func advance(_ seconds: TimeInterval) { now += seconds }
}

private final class FakeSession {
    private(set) var starts = 0
    private(set) var stops = 0
    private(set) var running = false
    func start() { if !running { running = true; starts += 1 } }
    func stop() { if running { running = false; stops += 1 } }
}

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("CameraLifecycleTests: \(message)") }
}

@main
private struct CameraLifecycleTests {
    static func main() {
        var clock = FakeClock()
        var policy = CameraLifecycle()
        let session = FakeSession()
        expect(!policy.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 1,
                                cameraID: "camera-a", alwaysOn: true), "inactive policy should not require a stop")
        expect(policy.shouldKeepSession, "eligible always-on mode should retain capture")
        session.start()

        // Face/light changes, generation changes, and camera cadence changes preserve a healthy session.
        let shouldStop = policy.reconcile(mode: "light_only", captureMode: "continuous_monitoring", generation: 2,
                                          cameraID: "camera-a", alwaysOn: true)
        expect(!shouldStop && policy.shouldKeepSession && session.starts == 1, "eligible transitions must reuse session")
        expect(policy.acceptsCallback(generation: 2), "new generation should be accepted")
        expect(!policy.acceptsCallback(generation: 1), "old generation callback must be stale")
        let firstAssessmentToken = policy.assessmentToken
        policy.beginLightAssessment(now: clock.now)
        expect(!policy.isCurrentAssessment(firstAssessmentToken), "superseded same-generation timer callback must be stale")
        let latestAssessmentToken = policy.assessmentToken
        expect(policy.isCurrentAssessment(latestAssessmentToken), "latest assessment timer remains current")

        // A warmup frame satisfies acquisition but cannot satisfy light assessment.
        policy.beginAcquisition(now: clock.now)
        policy.beginLightAssessment(now: clock.now)
        let canceledAssessmentToken = policy.assessmentToken
        clock.advance(4.9)
        expect(policy.frame(at: clock.now) == .discardWarmup, "warmup frames must be discarded")
        policy.acquiredFrame()
        clock.advance(0.1)
        expect(!policy.timedOut(at: clock.now), "warmup duration must not extend acquisition watchdog")
        clock.advance(4.9)
        expect(policy.frame(at: clock.now) == .assessLight, "first post-settle frame must assess light")
        expect(!policy.timedOut(at: clock.now), "successful assessment clears post-settle watchdog")

        // Successive scheduled checks use caller-provided core cadence and each get a fresh settle window.
        clock.advance(10)
        policy.beginLightAssessment(now: clock.now)
        clock.advance(5)
        expect(policy.frame(at: clock.now) == .assessLight, "second usable-light opportunity must be available")

        // A frozen stream fails five seconds after settling; initial acquisition still expires at five seconds.
        clock.advance(10)
        policy.beginLightAssessment(now: clock.now)
        clock.advance(5)
        clock.advance(5)
        expect(policy.timedOut(at: clock.now), "frozen post-warmup stream must time out")
        policy.beginAcquisition(now: clock.now)
        clock.advance(5)
        expect(policy.timedOut(at: clock.now), "initial acquisition retains its independent five-second limit")

        // A delayed pre-settle frame cannot cancel the fresh post-settle watchdog.
        var frozen = CameraLifecycle()
        _ = frozen.reconcile(mode: "light_only", captureMode: "continuous_monitoring", generation: 20,
                             cameraID: "camera-f", alwaysOn: true)
        frozen.beginAcquisition(now: 0)
        frozen.beginLightAssessment(now: 5)
        frozen.acquiredFrame()
        expect(frozen.frame(at: 9.9) == .discardWarmup, "late warmup frame remains discardable")
        expect(frozen.timedOut(at: 15), "discarded warmup frame must leave assessment watchdog armed")
        frozen.beginLightAssessment(now: 20)
        expect(frozen.frame(at: 30) == .assessmentExpired, "frame arriving after post-settle deadline must fail")
        let invalidatedToken = frozen.assessmentToken
        frozen.cancelDeadlines()
        expect(!frozen.isCurrentAssessment(invalidatedToken), "cancelled timer cannot arm a same-generation watchdog")
        expect(!policy.isCurrentAssessment(canceledAssessmentToken), "completed assessment invalidates its timer")

        // Suspension cancels deadlines and releases the session; stale callbacks remain rejected.
        _ = policy.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 3,
                             cameraID: "camera-a", alwaysOn: true)
        session.start()
        policy.beginLightAssessment(now: clock.now)
        let stopped = policy.reconcile(mode: "suspended", captureMode: "stopped", generation: 4,
                                       cameraID: "camera-a", alwaysOn: true)
        if stopped { session.stop() }
        expect(stopped && !session.running && session.stops == 1, "suspension must stop camera session")
        expect(policy.settlingUntil == nil && policy.assessmentDeadline == nil, "suspension cancels pending timers")
        expect(!policy.acceptsCallback(generation: 4) && !policy.acceptsCallback(generation: 3), "suspended observations are rejected")

        // Camera replacement and shutdown also release any retained session.
        _ = policy.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 5,
                             cameraID: "camera-a", alwaysOn: true)
        session.start()
        let replaced = policy.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 6,
                                        cameraID: "camera-b", alwaysOn: true)
        if replaced { session.stop() }
        expect(replaced && !session.running, "camera replacement must stop prior session")
        _ = policy.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 7,
                             cameraID: "camera-b", alwaysOn: true)
        session.start()
        let shutdown = policy.reconcile(mode: "disabled", captureMode: "stopped", generation: 8,
                                        cameraID: "camera-b", alwaysOn: true)
        if shutdown { session.stop() }
        expect(shutdown && session.stops == 3, "shutdown stops the retained session")

        // Identical snapshots must not restart a return session or a pending one-frame capture.
        var returnPolicy = CameraLifecycle()
        _ = returnPolicy.reconcile(mode: "face", captureMode: "continuous_return", generation: 10,
                                   cameraID: "camera-r", alwaysOn: false)
        session.start()
        let unchangedReturn = returnPolicy.reconcile(mode: "face", captureMode: "continuous_return", generation: 10,
                                                     cameraID: "camera-r", alwaysOn: false)
        expect(!unchangedReturn && session.starts == 4, "identical return snapshot must preserve session")
        let nextReturnGeneration = returnPolicy.reconcile(mode: "face", captureMode: "continuous_return", generation: 11,
                                                          cameraID: "camera-r", alwaysOn: false)
        expect(!nextReturnGeneration, "healthy return session survives generation retargeting")
        var oneFramePolicy = CameraLifecycle()
        _ = oneFramePolicy.reconcile(mode: "face", captureMode: "single_frame", generation: 12,
                                     cameraID: "camera-s", alwaysOn: false)
        expect(!oneFramePolicy.reconcile(mode: "face", captureMode: "single_frame", generation: 12,
                                         cameraID: "camera-s", alwaysOn: false), "unchanged pending one-frame policy does not restart")

        print("Camera lifecycle fake-clock tests passed")
    }
}
