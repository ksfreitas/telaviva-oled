import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("CameraLifecycleBridgeTests: \(message)") }
}

@main
private struct CameraLifecycleBridgeTests {
    static func main() {
        acquisitionWarmupAndAssessmentThroughTheCore()
        staleCallbackIsRejected()
        rejectedEventFaultsTheLifecycle()
        tenThousandStepsComplete()
        print("Camera lifecycle bridge tests passed")
    }

    /// Reconciles to face mode, acquires a frame, discards warm-up frames inside the settle window and assesses
    /// the first frame after it.
    private static func acquisitionWarmupAndAssessmentThroughTheCore() {
        let lifecycle = CameraLifecycle()
        expect(!lifecycle.eligible && !lifecycle.shouldKeepSession, "a new lifecycle must not be eligible")
        expect(!lifecycle.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 1,
                                    cameraID: "camera-a", alwaysOn: true), "the first reconcile must not require a stop")
        expect(lifecycle.eligible && lifecycle.shouldKeepSession, "always-on face mode must keep the session")
        expect(lifecycle.mode == "face" && lifecycle.cameraID == "camera-a" && lifecycle.generation == 1,
               "reconcile must cache the snapshot fields")

        lifecycle.beginAcquisition(now: 0)
        expect(lifecycle.acquisitionPending && lifecycle.acquisitionDeadline == 5,
               "acquisition must arm a five-second deadline")

        lifecycle.beginLightAssessment(now: 0)
        let token = lifecycle.assessmentToken
        expect(lifecycle.settlingUntil == 5 && lifecycle.assessmentDeadline == 10,
               "an assessment must settle for five seconds and then wait five more")
        expect(lifecycle.isCurrentAssessment(token), "the latest assessment token must be current")
        expect(lifecycle.frame(at: 4.9) == .discardWarmup, "a frame during the settle window must be discarded")

        lifecycle.acquiredFrame()
        expect(lifecycle.frame(at: 5) == .assessPresence, "the first post-settle frame must be assessed for a face")
        expect(!lifecycle.acquisitionPending && lifecycle.settlingUntil == nil && lifecycle.assessmentDeadline == nil,
               "an assessed frame must clear the settle window and the deadlines")
        expect(!lifecycle.isCurrentAssessment(token), "a completed assessment must not stay current")

        lifecycle.beginLightAssessment(now: 20)
        let pendingToken = lifecycle.assessmentToken
        lifecycle.cancelDeadlines()
        expect(!lifecycle.isCurrentAssessment(pendingToken) && lifecycle.settlingUntil == nil,
               "cancelDeadlines must cancel a pending assessment")
    }

    /// A callback from an earlier generation, or one that arrives after the camera is disabled, is rejected.
    private static func staleCallbackIsRejected() {
        let lifecycle = CameraLifecycle()
        _ = lifecycle.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 1,
                                cameraID: "camera-a", alwaysOn: true)
        expect(!lifecycle.reconcile(mode: "light_only", captureMode: "continuous_monitoring", generation: 2,
                                    cameraID: "camera-a", alwaysOn: true),
               "a generation change on the same camera must keep the session")
        expect(!lifecycle.acceptsCallback(generation: 1), "a callback from the previous generation must be stale")
        expect(lifecycle.acceptsCallback(generation: 2), "a callback from the current generation must be accepted")
        _ = lifecycle.reconcile(mode: "disabled", captureMode: "single_frame", generation: 3,
                                cameraID: "camera-a", alwaysOn: false)
        expect(!lifecycle.acceptsCallback(generation: 2), "callbacks must be rejected once the camera is disabled")
    }

    /// The core rejects an event above its 64 KiB cap. The failed call must stop the camera without crashing, and
    /// the wrapper must stay stopped for later calls.
    private static func rejectedEventFaultsTheLifecycle() {
        let lifecycle = CameraLifecycle()
        expect(!lifecycle.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 1,
                                    cameraID: "camera-a", alwaysOn: true), "the setup reconcile must not require a stop")
        let oversizedID = String(repeating: "c", count: 70_000)
        expect(lifecycle.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 2,
                                   cameraID: oversizedID, alwaysOn: true), "a rejected reconcile must stop the session")
        expect(!lifecycle.eligible && !lifecycle.shouldKeepSession && lifecycle.settlingUntil == nil,
               "a faulted lifecycle must report no session")
        expect(!lifecycle.acceptsCallback(generation: 2), "a faulted lifecycle must reject every callback")
        expect(lifecycle.frame(at: 0) == .assessmentExpired, "a faulted lifecycle must not assess frames")
        expect(lifecycle.timedOut(at: 0), "a faulted lifecycle must report a timeout so the caller stops")
        expect(lifecycle.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 3,
                                   cameraID: "camera-a", alwaysOn: true), "a faulted lifecycle must keep requesting a stop")
    }

    /// Cheap steps keep returning results across many calls, so every returned buffer is released on its path.
    private static func tenThousandStepsComplete() {
        let lifecycle = CameraLifecycle()
        _ = lifecycle.reconcile(mode: "face", captureMode: "continuous_monitoring", generation: 1,
                                cameraID: "camera-a", alwaysOn: true)
        var accepted = 0
        for _ in 0..<10_000 {
            if lifecycle.acceptsCallback(generation: 1) { accepted += 1 }
        }
        expect(accepted == 10_000, "every one of 10,000 steps must return a result")
    }
}
