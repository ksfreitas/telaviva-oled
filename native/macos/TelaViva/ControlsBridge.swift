import Foundation

/// Draft status reported by the controls core: `ready` when no change is pending, `pending` while a write
/// is queued or in flight, and `failed` after a write was rejected.
enum ControlsDraftStatus: String, Equatable {
    case ready
    case pending
    case failed
}

/// The draft of one slider. `hasDraft` is false when the core holds no draft for the control.
struct ControlsDraft: Equatable {
    var hasDraft: Bool
    var draft: Double
    var status: ControlsDraftStatus
}

/// One instruction from the controls core. The driver runs the effects of a reply in order.
enum ControlsEffect: Equatable {
    /// Passes `json` (a `set_brightness` or `set_volume` command) to `tv_submit`, then reports `submitted`.
    case submitCommand(token: UInt64, json: String)
    /// Passes `json` (a `hardware_result`) to `tv_submit`, then reports `submitted`.
    case submitResult(token: UInt64, json: String)
    /// Runs one DDC primitive. `value` is the raw target and is present for writes only.
    case runAttempt(token: UInt64, monitorID: String, write: Bool, code: UInt8, value: Double?)
    /// The control of this monitor and feature needs a repaint. `feature` is "brightness" or "volume".
    case draftChanged(monitorID: String, feature: String)
}

/// The decoded reply of one controls step.
struct ControlsResult: Equatable {
    var effects: [ControlsEffect] = []
    /// Absolute monotonic seconds of the next `tick`, or nil.
    var nextDeadline: Double?
    /// The slider draft, or the answer to `query_draft`.
    var draft: ControlsDraft?
    /// The answer to a key event.
    var consume: Bool?

    static let empty = ControlsResult()

    /// Decodes one reply. Returns nil when the JSON is malformed or an effect type is unknown.
    static func decode(_ data: Data) -> ControlsResult? {
        guard let wire = try? JSONDecoder().decode(WireResult.self, from: data) else { return nil }
        var effects: [ControlsEffect] = []
        for item in wire.effects {
            guard let effect = item.effect() else { return nil }
            effects.append(effect)
        }
        var result = ControlsResult(effects: effects, nextDeadline: wire.nextDeadline)
        if let value = wire.value {
            result.consume = value.consume
            if let draft = value.draft {
                guard let status = value.status.flatMap(ControlsDraftStatus.init(rawValue:)) else { return nil }
                result.draft = ControlsDraft(hasDraft: value.hasDraft ?? true, draft: draft, status: status)
            }
        }
        return result
    }
}

/// Anything that runs one controls step. `ControlsBridge` is the production implementation.
protocol ControlsStepping: AnyObject {
    /// Applies one event and returns the reply, or nil when the step failed.
    func step(_ event: [String: Any]) -> ControlsResult?
}

/// Owns the `TvControls` handle. Each call encodes one JSON event, runs `tv_controls_step` and decodes the reply.
/// A failed step (a non-OK status, an undecodable reply or an event the core rejects) faults the bridge for good:
/// every later step returns nil without touching the handle, so a broken ABI stops the controls but never
/// keeps sending events the core did not accept.
final class ControlsBridge: ControlsStepping {
    #if TELAVIVA_CORE
    private let handle: OpaquePointer?
    #endif
    private let lock = NSLock()
    private var faulted = false

    /// True after a failed step. Every later step returns nil.
    var isFaulted: Bool {
        lock.lock(); defer { lock.unlock() }
        return faulted
    }

    init() {
        #if TELAVIVA_CORE
        var created: OpaquePointer?
        let status = tv_controls_create(&created)
        handle = status == TV_OK ? created : nil
        #endif
    }

    deinit {
        #if TELAVIVA_CORE
        if let handle { tv_controls_destroy(handle) }
        #endif
    }

    /// Runs one step. Calls are serialized, so the core never sees two events at once.
    func step(_ event: [String: Any]) -> ControlsResult? {
        lock.lock(); defer { lock.unlock() }
        guard !faulted else { return nil }
        guard let result = perform(event) else {
            faulted = true
            return nil
        }
        return result
    }

    /// Encodes one event, runs one core step and decodes the reply. Returns nil on any failure.
    private func perform(_ event: [String: Any]) -> ControlsResult? {
        #if TELAVIVA_CORE
        guard let handle, JSONSerialization.isValidJSONObject(event),
              let bytes = try? JSONSerialization.data(withJSONObject: event) else { return nil }
        var buffer = TvBuffer(data: nil, len: 0)
        defer { tv_buffer_free(&buffer) }
        let status = bytes.withUnsafeBytes { raw in
            tv_controls_step(handle, raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count, &buffer)
        }
        guard status == TV_OK, let data = buffer.data else { return nil }
        return ControlsResult.decode(Data(bytes: data, count: buffer.len))
        #else
        return nil
        #endif
    }
}

/// Wire form of a reply. Every key is required, so a missing `value`, `effects` or `next_deadline` is malformed.
private struct WireResult: Decodable {
    let value: WireValue?
    let effects: [WireEffect]
    let nextDeadline: Double?

    enum CodingKeys: String, CodingKey {
        case value
        case effects
        case nextDeadline = "next_deadline"
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        value = try container.decode(WireValue?.self, forKey: .value)
        effects = try container.decode([WireEffect].self, forKey: .effects)
        nextDeadline = try container.decode(Double?.self, forKey: .nextDeadline)
    }
}

/// The `value` member: null, a draft (`draft`, `status` and optionally `has_draft`) or a key answer (`consume`).
private struct WireValue: Decodable {
    let draft: Double?
    let status: String?
    let hasDraft: Bool?
    let consume: Bool?

    enum CodingKeys: String, CodingKey {
        case draft
        case status
        case hasDraft = "has_draft"
        case consume
    }
}

/// One effect as it appears on the wire. The fields that a type needs are checked when it is converted.
private struct WireEffect: Decodable {
    let type: String
    let token: UInt64?
    let commandJSON: String?
    let resultJSON: String?
    let monitorID: String?
    let op: String?
    let code: UInt8?
    let value: Double?
    let feature: String?

    enum CodingKeys: String, CodingKey {
        case type
        case token
        case commandJSON = "command_json"
        case resultJSON = "result_json"
        case monitorID = "monitor_id"
        case op
        case code
        case value
        case feature
    }

    /// Converts the wire effect. Returns nil for an unknown type or a missing field.
    func effect() -> ControlsEffect? {
        switch type {
        case "submit_command":
            guard let token, let commandJSON else { return nil }
            return .submitCommand(token: token, json: commandJSON)
        case "submit_result":
            guard let token, let resultJSON else { return nil }
            return .submitResult(token: token, json: resultJSON)
        case "run_attempt":
            guard let token, let monitorID, let code else { return nil }
            switch op {
            case "read":
                return .runAttempt(token: token, monitorID: monitorID, write: false, code: code, value: nil)
            case "write":
                guard let value else { return nil }
                return .runAttempt(token: token, monitorID: monitorID, write: true, code: code, value: value)
            default:
                return nil
            }
        case "draft_changed":
            guard let monitorID, let feature else { return nil }
            return .draftChanged(monitorID: monitorID, feature: feature)
        default:
            return nil
        }
    }
}
