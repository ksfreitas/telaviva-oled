import Foundation

/// Camera session timing for the macOS shell. The policy lives in the Rust core
/// (`crates/telaviva-core/src/camera_lifecycle.rs`) behind an opaque `TvCameraLifecycle` handle.
/// Each call encodes one JSON event, runs `tv_camera_lifecycle_step` and caches the `state` that the
/// core returns, so the properties always report the state from the last successful call.
/// Time is supplied by the caller from one monotonic clock domain.
///
/// The camera adapter mutates this object only on its queue. A failed call (a non-TV_OK status, an
/// undecodable result or an event the core rejects) is a programmer error. That call returns its stop
/// value and the wrapper faults for good: every later call returns its stop value, so a broken ABI can
/// stop the camera but never restart it.
final class CameraLifecycle {
    /// What the shell does with one camera frame. Decoded from the `value` string of a `frame` result.
    enum FrameDecision: Equatable {
        case discardWarmup
        case assessLight
        case assessPresence
        case assessmentExpired

        init?(resultValue: String) {
            switch resultValue {
            case "discard_warmup": self = .discardWarmup
            case "assess_light": self = .assessLight
            case "assess_presence": self = .assessPresence
            case "assessment_expired": self = .assessmentExpired
            default: return nil
            }
        }
    }

    #if TELAVIVA_CORE
    private let handle: OpaquePointer?
    #endif
    private var cached = LifecycleState.idle
    private var faulted = false

    var generation: UInt64 { cached.generation }
    var cameraID: String? { cached.cameraID }
    var mode: String { cached.mode }
    var captureMode: String { cached.captureMode }
    var alwaysOn: Bool { cached.alwaysOn }
    var settlingUntil: TimeInterval? { cached.settlingUntil }
    var assessmentDeadline: TimeInterval? { cached.assessmentDeadline }
    var acquisitionDeadline: TimeInterval? { cached.acquisitionDeadline }
    var assessmentToken: UInt64 { cached.assessmentToken }
    var eligible: Bool { cached.eligible }
    var acquisitionPending: Bool { cached.acquisitionPending }
    var shouldKeepSession: Bool { cached.shouldKeepSession }

    init() {
        #if TELAVIVA_CORE
        var created: OpaquePointer?
        let status = tv_camera_lifecycle_create(&created)
        handle = status == TV_OK ? created : nil
        #endif
    }

    deinit {
        #if TELAVIVA_CORE
        if let handle { tv_camera_lifecycle_destroy(handle) }
        #endif
    }

    /// Applies a new snapshot from the core. Returns true when the existing session must be stopped
    /// before the new state is used. A failed call returns true.
    func reconcile(mode nextMode: String, captureMode nextCaptureMode: String,
                   generation nextGeneration: UInt64, cameraID nextCameraID: String?,
                   alwaysOn nextAlwaysOn: Bool) -> Bool {
        let cameraValue: Any = nextCameraID ?? NSNull()
        guard let stop = booleanResult([
            "type": "reconcile", "mode": nextMode, "capture_mode": nextCaptureMode,
            "generation": nextGeneration, "camera_id": cameraValue, "always_on": nextAlwaysOn
        ]) else { return true }
        return stop
    }

    func beginAcquisition(now: TimeInterval) {
        acknowledge(["type": "begin_acquisition", "now": now])
    }

    func acquiredFrame() {
        acknowledge(["type": "acquired_frame"])
    }

    func beginLightAssessment(now: TimeInterval) {
        acknowledge(["type": "begin_light_assessment", "now": now])
    }

    /// Returns true when the dark settle started, so the caller keeps the session open. Returns false
    /// when the dark result is final for this sample, and also after a failed call.
    func beginDarkSettle(now: TimeInterval) -> Bool {
        booleanResult(["type": "begin_dark_settle", "now": now]) ?? false
    }

    func isCurrentAssessment(_ token: UInt64) -> Bool {
        booleanResult(["type": "is_current_assessment", "token": token]) ?? false
    }

    /// Classifies a frame that arrived at `now`. A failed call returns `.assessmentExpired`, which stops capture.
    func frame(at now: TimeInterval) -> FrameDecision {
        guard let value = stringResult(["type": "frame", "now": now]) else { return .assessmentExpired }
        guard let decision = FrameDecision(resultValue: value) else {
            markFaulted()
            return .assessmentExpired
        }
        return decision
    }

    /// Returns true when a deadline has passed. A failed call returns true, so the session stops.
    func timedOut(at now: TimeInterval) -> Bool {
        booleanResult(["type": "timed_out", "now": now]) ?? true
    }

    func cancelDeadlines() {
        acknowledge(["type": "cancel_deadlines"])
    }

    /// True when a callback tagged with `callbackGeneration` belongs to the current eligible session.
    /// A failed call returns false.
    func acceptsCallback(generation callbackGeneration: UInt64) -> Bool {
        booleanResult(["type": "accepts_callback", "generation": callbackGeneration]) ?? false
    }

    /// Encodes one event, runs one core step and returns the core's `value`. Returns nil on any failure.
    /// The cached state is replaced only when the whole result decodes, so a failed call leaves it unchanged.
    private func send(_ event: [String: Any]) -> StepValue? {
        #if TELAVIVA_CORE
        guard !faulted, let handle, JSONSerialization.isValidJSONObject(event),
              let bytes = try? JSONSerialization.data(withJSONObject: event) else { return nil }
        var buffer = TvBuffer(data: nil, len: 0)
        defer { tv_buffer_free(&buffer) }
        let status = bytes.withUnsafeBytes { raw in
            tv_camera_lifecycle_step(handle, raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count, &buffer)
        }
        guard status == TV_OK, let data = buffer.data,
              let result = try? JSONDecoder().decode(StepResult.self, from: Data(bytes: data, count: buffer.len)) else {
            return nil
        }
        cached = result.state
        return result.value
        #else
        return nil
        #endif
    }

    /// Sends an event whose `value` is a Boolean. Any other outcome faults the wrapper and returns nil.
    private func booleanResult(_ event: [String: Any]) -> Bool? {
        guard case .bool(let flag)? = send(event) else {
            markFaulted()
            return nil
        }
        return flag
    }

    /// Sends an event whose `value` is a string. Any other outcome faults the wrapper and returns nil.
    private func stringResult(_ event: [String: Any]) -> String? {
        guard case .text(let text)? = send(event) else {
            markFaulted()
            return nil
        }
        return text
    }

    /// Sends an event that has no value. Any other outcome faults the wrapper.
    private func acknowledge(_ event: [String: Any]) {
        if case .null? = send(event) { return }
        markFaulted()
    }

    /// Stops the camera for good: the cached state reports an idle lifecycle and every later call returns
    /// its stop value.
    private func markFaulted() {
        faulted = true
        cached = LifecycleState.idle
    }
}

/// The `value` of a step result: a Boolean, a frame-decision name, or null for events without a value.
private enum StepValue: Decodable {
    case bool(Bool)
    case text(String)
    case null

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
        } else if let flag = try? container.decode(Bool.self) {
            self = .bool(flag)
        } else {
            self = .text(try container.decode(String.self))
        }
    }
}

/// One step result: `{"value": ..., "state": {...}}`.
private struct StepResult: Decodable {
    let value: StepValue
    let state: LifecycleState
}

/// The flat `state` object that the core returns after every step. Field names follow the core's JSON.
private struct LifecycleState: Decodable {
    let generation: UInt64
    let cameraID: String?
    let mode: String
    let captureMode: String
    let alwaysOn: Bool
    let eligible: Bool
    let acquisitionPending: Bool
    let shouldKeepSession: Bool
    let settlingUntil: TimeInterval?
    let assessmentDeadline: TimeInterval?
    let acquisitionDeadline: TimeInterval?
    let assessmentToken: UInt64

    /// Nothing is scheduled and no session is kept. Used before the first step and after a fault.
    static let idle = LifecycleState(
        generation: 0, cameraID: nil, mode: "disabled", captureMode: "single_frame", alwaysOn: false,
        eligible: false, acquisitionPending: false, shouldKeepSession: false, settlingUntil: nil,
        assessmentDeadline: nil, acquisitionDeadline: nil, assessmentToken: 0)

    enum CodingKeys: String, CodingKey {
        case generation
        case cameraID = "camera_id"
        case mode
        case captureMode = "capture_mode"
        case alwaysOn = "always_on"
        case eligible
        case acquisitionPending = "acquisition_pending"
        case shouldKeepSession = "should_keep_session"
        case settlingUntil = "settling_until"
        case assessmentDeadline = "assessment_deadline"
        case acquisitionDeadline = "acquisition_deadline"
        case assessmentToken = "assessment_token"
    }
}
