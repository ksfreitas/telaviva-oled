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

        // A dark face frame starts one five-second settle; frames before the deadline are discarded
        // and a usable frame at the deadline goes to normal face assessment.
        var darkUsable = CameraLifecycle()
        _ = darkUsable.reconcile(mode: "face", captureMode: "single_frame", generation: 30,
                                 cameraID: "camera-d", alwaysOn: false)
        darkUsable.beginAcquisition(now: 0)
        darkUsable.acquiredFrame()
        expect(darkUsable.frame(at: 1) == .assessPresence, "first face frame must be assessed")
        expect(darkUsable.beginDarkSettle(now: 1), "first dark face frame must start a settle")
        expect(darkUsable.settlingUntil == 6, "dark settle must last five seconds")
        expect(darkUsable.frame(at: 3) == .discardWarmup, "frames during the dark settle must be discarded")
        expect(darkUsable.frame(at: 5.9) == .discardWarmup, "frames before the dark settle deadline must be discarded")
        expect(darkUsable.frame(at: 6) == .assessPresence, "usable frame at the dark settle deadline must assess face")
        expect(darkUsable.settlingUntil == nil, "settled frame must clear the dark settle")

        // A dark frame at the settle deadline is final for its sample and is reported as insufficient light.
        var darkFinal = CameraLifecycle()
        _ = darkFinal.reconcile(mode: "face", captureMode: "single_frame", generation: 31,
                                cameraID: "camera-d", alwaysOn: false)
        darkFinal.beginAcquisition(now: 0)
        darkFinal.acquiredFrame()
        expect(darkFinal.frame(at: 1) == .assessPresence, "first dark face frame must be assessed")
        expect(darkFinal.beginDarkSettle(now: 1), "first dark face frame must start a settle")
        expect(darkFinal.frame(at: 6) == .assessPresence, "frame at the settle deadline must be assessed")
        expect(!darkFinal.beginDarkSettle(now: 6), "dark frame at the settle deadline must be reported, not settled again")

        // The next sample resets the settle, so its first dark frame starts a fresh five-second settle.
        darkFinal.beginAcquisition(now: 10)
        darkFinal.acquiredFrame()
        expect(darkFinal.frame(at: 11) == .assessPresence, "second sample's first face frame must be assessed")
        expect(darkFinal.beginDarkSettle(now: 11), "second sample must start a fresh dark settle")
        expect(darkFinal.settlingUntil == 16, "fresh dark settle must last five seconds")
        expect(darkFinal.frame(at: 16) == .assessPresence, "second sample must reach its own settled frame")

        // Light-only samples never enter the dark settle and keep assessing light directly.
        var lightOnlyDark = CameraLifecycle()
        _ = lightOnlyDark.reconcile(mode: "light_only", captureMode: "single_frame", generation: 32,
                                    cameraID: "camera-l", alwaysOn: false)
        lightOnlyDark.beginAcquisition(now: 0)
        lightOnlyDark.acquiredFrame()
        expect(!lightOnlyDark.beginDarkSettle(now: 1), "dark settle must not start outside face mode")
        expect(lightOnlyDark.settlingUntil == nil, "light-only frame must not start a dark settle")
        expect(lightOnlyDark.frame(at: 1) == .assessLight, "light-only frame must still assess light directly")

        print("Camera lifecycle fake-clock tests passed")
    }
}
