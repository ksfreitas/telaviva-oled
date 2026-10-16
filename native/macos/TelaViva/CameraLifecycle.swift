import Foundation

/// Queue-confined lifecycle policy shared by the camera adapter and hardware-free tests.
/// Time is supplied by the caller from one monotonic clock domain.
struct CameraLifecycle {
    enum FrameDecision: Equatable {
        case discardWarmup
        case assessLight
        case assessPresence
        case assessmentExpired
    }

    private(set) var generation: UInt64 = 0
    private(set) var cameraID: String?
    private(set) var mode = "disabled"
    private(set) var captureMode = "single_frame"
    private(set) var alwaysOn = false
    private(set) var settlingUntil: TimeInterval?
    private(set) var assessmentDeadline: TimeInterval?
    private(set) var acquisitionDeadline: TimeInterval?
    private(set) var assessmentToken: UInt64 = 0

    var eligible: Bool { mode == "face" || mode == "light_only" }
    var acquisitionPending: Bool { acquisitionDeadline != nil }
    var shouldKeepSession: Bool {
        eligible && alwaysOn && (captureMode == "continuous_monitoring" || captureMode == "continuous_return")
    }
    func acceptsCallback(generation callbackGeneration: UInt64) -> Bool {
        eligible && callbackGeneration == generation
    }

    /// Returns true when the existing session must be stopped before applying the new state.
    mutating func reconcile(mode nextMode: String, captureMode nextCaptureMode: String,
                            generation nextGeneration: UInt64, cameraID nextCameraID: String?,
                            alwaysOn nextAlwaysOn: Bool) -> Bool {
        let nextEligible = nextMode == "face" || nextMode == "light_only"
        let unchanged = mode == nextMode && captureMode == nextCaptureMode && generation == nextGeneration
            && cameraID == nextCameraID && alwaysOn == nextAlwaysOn
        let sameCamera = cameraID == nextCameraID
        let retainAlwaysOn = alwaysOn && nextAlwaysOn && nextEligible && sameCamera
            && (nextCaptureMode == "continuous_monitoring" || nextCaptureMode == "continuous_return")
        let retainReturn = captureMode == "continuous_return" && nextCaptureMode == "continuous_return"
            && nextEligible && sameCamera
        let stop = eligible && (!nextEligible || !sameCamera || (!unchanged && !retainAlwaysOn && !retainReturn))
        mode = nextMode
        captureMode = nextCaptureMode
        generation = nextGeneration
        cameraID = nextCameraID
        alwaysOn = nextAlwaysOn
        if stop { cancelDeadlines() }
        return stop
    }

    mutating func beginAcquisition(now: TimeInterval, timeout: TimeInterval = 5) {
        acquisitionDeadline = now + timeout
    }

    mutating func acquiredFrame() { acquisitionDeadline = nil }

    /// Each low-light request gets five seconds of exposure settling and then a fresh
    /// five-second deadline for a post-settle frame.
    mutating func beginLightAssessment(now: TimeInterval, settling: TimeInterval = 5,
                                      frameTimeout: TimeInterval = 5) {
        assessmentToken &+= 1
        settlingUntil = now + settling
        assessmentDeadline = now + settling + frameTimeout
    }

    func isCurrentAssessment(_ token: UInt64) -> Bool {
        token == assessmentToken && assessmentDeadline != nil
    }

    mutating func frame(at now: TimeInterval) -> FrameDecision {
        if let assessmentDeadline, now >= assessmentDeadline {
            cancelDeadlines()
            return .assessmentExpired
        }
        if let settlingUntil, now < settlingUntil { return .discardWarmup }
        settlingUntil = nil
        assessmentDeadline = nil
        acquisitionDeadline = nil
        return mode == "light_only" ? .assessLight : .assessPresence
    }

    mutating func timedOut(at now: TimeInterval) -> Bool {
        if let deadline = acquisitionDeadline, now >= deadline {
            cancelDeadlines()
            return true
        }
        if let deadline = assessmentDeadline, now >= deadline {
            cancelDeadlines()
            return true
        }
        return false
    }

    mutating func cancelDeadlines() {
        assessmentToken &+= 1
        settlingUntil = nil
        assessmentDeadline = nil
        acquisitionDeadline = nil
    }
}
