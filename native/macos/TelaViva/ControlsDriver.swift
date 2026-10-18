import CoreMedia
import Foundation

/// One DDC primitive that the controls core asks the shell to run.
struct ControlsAttemptRequest: Equatable {
    let monitorID: String
    let write: Bool
    let code: UInt8
    /// The raw target of a write. 0 for a read.
    let value: UInt16
}

/// The result of one DDC primitive. `error` is "unsupported", "io" or "disconnected" when `ok` is false.
struct ControlsAttemptOutcome: Equatable {
    let ok: Bool
    let error: String?
    /// The raw readback of a read, or nil.
    let value: Double?

    static func success(value: Double?) -> ControlsAttemptOutcome {
        ControlsAttemptOutcome(ok: true, error: nil, value: value)
    }

    static func failure(_ error: String) -> ControlsAttemptOutcome {
        ControlsAttemptOutcome(ok: false, error: error, value: nil)
    }
}

/// The monitor that owns the volume keys, as set by a `configure` event.
enum ControlsVolumeOwner: Equatable {
    /// The owner is left as it is.
    case unchanged
    /// No monitor owns the volume keys.
    case none
    /// The monitor with this ID owns the volume keys.
    case monitor(String)
}

/// Schedules `fire` once after `delay` seconds and returns a closure that cancels it.
/// `fire` never runs inside the call.
typealias ControlsScheduler = (TimeInterval, @escaping () -> Void) -> () -> Void

/// Shell side of the controls core. It sends events to the stepper, keeps one timer armed for the reply's
/// deadline, and runs the effects of each reply: `tv_submit` for commands and results, one DDC primitive per
/// attempt and repaints. The lock guards only the timer and stop state; it is never held while an effect runs.
final class ControlsDriver {
    /// Replies to effects run at a deeper level. Deeper chains are dropped, so a misbehaving core cannot recurse
    /// without bound.
    private static let maxEffectDepth = 64
    private static let timerQueue = DispatchQueue(label: "com.telaviva.controls-timer")

    /// Monotonic seconds from the host clock, the clock of the core's `now`.
    static let hostClock: () -> Double = { CMClockGetTime(CMClockGetHostTimeClock()).seconds }

    /// Schedules on a private serial queue with a work item, so the fire never runs inside the call.
    static let dispatchScheduler: ControlsScheduler = { delay, fire in
        let item = DispatchWorkItem(block: fire)
        ControlsDriver.timerQueue.asyncAfter(deadline: .now() + delay, execute: item)
        return { item.cancel() }
    }

    /// Called with the monitor and feature of a draft that needs a repaint. May run on any thread.
    var onDraftChanged: ((String, HardwareKeyFeature) -> Void)? {
        get {
            lock.lock(); defer { lock.unlock() }
            return draftObserver
        }
        set {
            lock.lock(); defer { lock.unlock() }
            draftObserver = newValue
        }
    }

    private let stepper: ControlsStepping
    private let submit: (String) -> Bool
    private let runAttempt: (ControlsAttemptRequest, @escaping (ControlsAttemptOutcome) -> Void) -> Void
    private let clock: () -> Double
    private let scheduler: ControlsScheduler

    /// Guards every field below. It is held across `stepper.step` and the timer arming, and never across an effect.
    private let lock = NSLock()
    private var stopped = false
    /// Deadline of the armed timer. Nil when no timer is armed.
    private var armedDeadline: Double?
    private var cancelTimer: (() -> Void)?
    /// Advanced whenever a timer is armed or cancelled, so a fire from an older arming is ignored.
    private var armEpoch: UInt64 = 0
    private var draftObserver: ((String, HardwareKeyFeature) -> Void)?

    init(stepper: ControlsStepping,
         submit: @escaping (String) -> Bool,
         runAttempt: @escaping (ControlsAttemptRequest, @escaping (ControlsAttemptOutcome) -> Void) -> Void,
         clock: @escaping () -> Double = ControlsDriver.hostClock,
         scheduler: @escaping ControlsScheduler = ControlsDriver.dispatchScheduler) {
        self.stepper = stepper
        self.submit = submit
        self.runAttempt = runAttempt
        self.clock = clock
        self.scheduler = scheduler
    }

    /// Sends the key steps and key routing. A non-finite step is sent as the key default, 5, because JSON cannot
    /// carry it. `volumeOwner` `.unchanged` leaves the owner as it is.
    func configure(brightnessStep: Double, volumeStep: Double, keysEnabled: Bool, volumeOwner: ControlsVolumeOwner) {
        var event: [String: Any] = [
            "type": "configure", "now": clock(),
            "brightness_step": brightnessStep.isFinite ? brightnessStep : 5.0,
            "volume_step": volumeStep.isFinite ? volumeStep : 5.0, "keys_enabled": keysEnabled
        ]
        switch volumeOwner {
        case .unchanged: break
        case .none: event["volume_owner"] = NSNull()
        case .monitor(let monitorID): event["volume_owner"] = monitorID
        }
        dispatch(event)
    }

    /// Sends the monitors and pending actions of a published snapshot. A snapshot without a revision is skipped.
    func observe(_ snapshot: CoreSnapshot) {
        guard let revision = snapshot.revision else { return }
        let monitors: [[String: Any]] = (snapshot.monitors ?? []).compactMap { monitor -> [String: Any]? in
            // A monitor without a generation cannot be addressed by a command, so it is left out.
            guard let generation = monitor.generation else { return nil }
            return [
                "id": monitor.id,
                "generation": generation,
                "display_state": monitor.display_state ?? "awake",
                "identity_status": monitor.identity_status ?? "ambiguous",
                "brightness": ControlsDriver.channel(monitor.brightness),
                "volume": ControlsDriver.channel(monitor.volume)
            ]
        }
        let pending: [[String: Any]] = (snapshot.pending_actions ?? []).map { action -> [String: Any] in
            [
                "id": action.id,
                "generation": action.generation,
                "monitor_id": action.monitor_id,
                "kind": action.kind,
                "value": ControlsDriver.nullable(action.value)
            ]
        }
        dispatch(["type": "observe", "now": clock(), "revision": revision, "monitors": monitors,
                  "pending_actions": pending])
    }

    /// Sends a slider change and returns the draft from the reply, or nil when the core ignored the target.
    /// A non-finite value is never sent, because JSON cannot carry it, and returns nil.
    @discardableResult
    func slider(monitorID: String, feature: HardwareKeyFeature, value: Double) -> ControlsDraft? {
        guard value.isFinite else { return nil }
        return dispatch(["type": "slider", "now": clock(), "monitor_id": monitorID,
                         "feature": feature.controlsName, "value": value]).draft
    }

    /// Sends one decoded volume, mute or brightness key event. Returns whether the shell should swallow it.
    func key(_ key: String, down: Bool, isRepeat: Bool, fine: Bool, owner: HardwareKeyOwner?) -> Bool {
        var ownerValue: Any = NSNull()
        if let owner {
            ownerValue = ["monitor_id": owner.monitorID, "generation": owner.generation] as [String: Any]
        }
        return dispatch(["type": "key", "now": clock(), "key": key, "phase": down ? "down" : "up",
                         "repeat": isRepeat, "fine": fine, "owner": ownerValue]).consume ?? false
    }

    /// Returns the draft of a control for drawing. An unknown answer is an idle draft at 0.
    func queryDraft(monitorID: String, feature: HardwareKeyFeature) -> ControlsDraft {
        dispatch(["type": "query_draft", "now": clock(), "monitor_id": monitorID,
                  "feature": feature.controlsName]).draft
            ?? ControlsDraft(hasDraft: false, draft: 0, status: .ready)
    }

    func monitorProfile(monitorID: String, manufacturer: String) {
        dispatch(["type": "monitor_profile", "now": clock(), "monitor_id": monitorID, "manufacturer": manufacturer])
    }

    func monitorRemoved(monitorID: String) {
        dispatch(["type": "monitor_removed", "now": clock(), "monitor_id": monitorID])
    }

    func displayPower(on: Bool) {
        dispatch(["type": "display_power", "now": clock(), "on": on])
    }

    /// Stops the driver. Later calls return empty values, and timers and attempt completions do nothing.
    func shutdown() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        stopped = true
        // The reply is ignored: no effects run once the driver has stopped.
        _ = stepper.step(["type": "shutdown", "now": clock()])
        cancelTimer?()
        cancelTimer = nil
        armedDeadline = nil
        armEpoch += 1
    }

    /// Sends one event, then runs the effects of its reply at depth 0.
    @discardableResult
    private func dispatch(_ event: [String: Any]) -> ControlsResult {
        let result = send(event)
        execute(result, depth: 0)
        return result
    }

    /// Steps the core under the lock and re-arms the timer from its deadline. Returns `.empty` when stopped
    /// or when the step fails.
    private func send(_ event: [String: Any]) -> ControlsResult {
        lock.lock(); defer { lock.unlock() }
        guard !stopped, let result = stepper.step(event) else { return .empty }
        rearm(deadline: result.nextDeadline)
        return result
    }

    /// Keeps one timer armed for `deadline`. Caller holds `lock`.
    private func rearm(deadline: Double?) {
        guard let deadline else {
            guard cancelTimer != nil else { return }
            cancelTimer?()
            cancelTimer = nil
            armedDeadline = nil
            armEpoch += 1
            return
        }
        guard deadline != armedDeadline else { return }
        cancelTimer?()
        armEpoch += 1
        let epoch = armEpoch
        armedDeadline = deadline
        cancelTimer = scheduler(max(0, deadline - clock())) { [weak self] in
            self?.onTimer(epoch)
        }
    }

    /// Runs the effects of one reply in order, stopping when the driver stops.
    private func execute(_ result: ControlsResult, depth: Int) {
        guard depth <= ControlsDriver.maxEffectDepth else { return }
        for effect in result.effects {
            guard !isStopped else { return }
            switch effect {
            case .submitCommand(let token, let json), .submitResult(let token, let json):
                let accepted = submit(json)
                execute(send(["type": "submitted", "now": clock(), "token": token, "accepted": accepted]),
                        depth: depth + 1)
            case .runAttempt(let token, let monitorID, let write, let code, let value):
                let request = ControlsAttemptRequest(monitorID: monitorID, write: write, code: code,
                                                     value: ControlsDriver.rawValue(value))
                runAttempt(request) { [weak self] outcome in
                    self?.finishAttempt(token: token, outcome: outcome, depth: depth + 1)
                }
            case .draftChanged(let monitorID, let feature):
                if let kind = HardwareKeyFeature(controlsName: feature) {
                    onDraftChanged?(monitorID, kind)
                }
            }
        }
    }

    /// Reports the outcome of one DDC primitive. A completion that arrives after shutdown does nothing.
    private func finishAttempt(token: UInt64, outcome: ControlsAttemptOutcome, depth: Int) {
        guard !isStopped else { return }
        var event: [String: Any] = ["type": "attempt_done", "now": clock(), "token": token, "ok": outcome.ok,
                                    "value": ControlsDriver.nullable(outcome.value)]
        if !outcome.ok {
            let error = outcome.error ?? ""
            event["error"] = error.isEmpty ? "io" : error
        }
        execute(send(event), depth: depth)
    }

    /// Runs when the armed timer fires. A fire from a stale arming, or after shutdown, does nothing.
    private func onTimer(_ epoch: UInt64) {
        guard claimTimer(epoch) else { return }
        execute(send(["type": "tick", "now": clock()]), depth: 0)
    }

    /// Clears the armed state when `epoch` is still the current arming. Returns false for a stale or late fire.
    private func claimTimer(_ epoch: UInt64) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard !stopped, epoch == armEpoch, cancelTimer != nil else { return false }
        // Cleared before the tick, so the reply may arm a new timer.
        armedDeadline = nil
        cancelTimer = nil
        return true
    }

    private var isStopped: Bool {
        lock.lock(); defer { lock.unlock() }
        return stopped
    }

    /// One channel of an observation. `value` is the normalized percentage. A missing channel is unsupported
    /// and has the default range.
    private static func channel(_ channel: VcpValue?) -> [String: Any] {
        guard let channel else {
            return ["supported": false, "observed": NSNull(), "min": 0.0, "max": 100.0]
        }
        return ["supported": channel.supported, "observed": nullable(channel.value),
                "min": channel.min ?? 0.0, "max": channel.max ?? 100.0]
    }

    private static func nullable(_ number: Double?) -> Any {
        guard let number else { return NSNull() }
        return number
    }

    /// Rounds a raw target to the nearest integer and clamps it to 16 bits. A missing or NaN target is 0.
    private static func rawValue(_ value: Double?) -> UInt16 {
        guard let value, !value.isNaN else { return 0 }
        return UInt16(min(max(value.rounded(), 0), 65_535))
    }
}

extension ControlsDriver: HardwareKeyControlling, ManualControlsDriving {}

extension HardwareKeyFeature {
    /// The feature name of the controls protocol.
    var controlsName: String {
        switch self {
        case .brightness: return "brightness"
        case .volume: return "volume"
        }
    }

    init?(controlsName: String) {
        switch controlsName {
        case "brightness": self = .brightness
        case "volume": self = .volume
        default: return nil
        }
    }
}
