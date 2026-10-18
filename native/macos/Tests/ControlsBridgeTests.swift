import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("ControlsBridgeTests: \(message)") }
}

/// Stands in for the core. Records every event and answers with the next queued reply, or `.empty` when none
/// is queued. `respond` takes precedence when it is set.
private final class FakeStepper: ControlsStepping {
    private(set) var events: [[String: Any]] = []
    var replies: [ControlsResult?] = []
    var respond: (([String: Any]) -> ControlsResult?)?

    func step(_ event: [String: Any]) -> ControlsResult? {
        events.append(event)
        if let respond { return respond(event) }
        return replies.isEmpty ? ControlsResult.empty : replies.removeFirst()
    }

    var last: [String: Any] { events.last ?? [:] }
}

/// A scheduled fire that the test runs by hand. Cancelling only marks it, so a test can also fire a cancelled timer
/// and check that the driver ignores it.
private final class FakeTimer {
    let delay: TimeInterval
    private let work: () -> Void
    private(set) var cancelled = false
    private(set) var fired = false

    init(delay: TimeInterval, work: @escaping () -> Void) {
        self.delay = delay
        self.work = work
    }

    func cancel() { cancelled = true }

    func fire() {
        fired = true
        work()
    }
}

private final class FakeScheduler {
    private(set) var timers: [FakeTimer] = []

    /// Matches `ControlsScheduler`.
    func schedule(_ delay: TimeInterval, _ fire: @escaping () -> Void) -> () -> Void {
        let timer = FakeTimer(delay: delay, work: fire)
        timers.append(timer)
        return { timer.cancel() }
    }

    /// The timers that are neither cancelled nor fired yet.
    var armed: [FakeTimer] { timers.filter { !$0.cancelled && !$0.fired } }
}

private final class FakeClock {
    var now: Double = 100
}

/// Records what the driver hands to the shell: submitted JSON, DDC attempts and draft changes.
private final class Recorder {
    var submitted: [String] = []
    var acceptSubmissions = true
    var attempts: [(request: ControlsAttemptRequest, complete: (ControlsAttemptOutcome) -> Void)] = []
    var draftChanges: [String] = []

    func submit(_ json: String) -> Bool {
        submitted.append(json)
        return acceptSubmissions
    }

    func attempt(_ request: ControlsAttemptRequest, _ complete: @escaping (ControlsAttemptOutcome) -> Void) {
        attempts.append((request: request, complete: complete))
    }
}

/// A driver wired to the fakes. Advance time with `clock.now`.
private final class Harness {
    let stepper: FakeStepper
    let scheduler: FakeScheduler
    let clock: FakeClock
    let recorder: Recorder
    let driver: ControlsDriver

    init() {
        let stepper = FakeStepper(), scheduler = FakeScheduler(), clock = FakeClock(), recorder = Recorder()
        self.stepper = stepper
        self.scheduler = scheduler
        self.clock = clock
        self.recorder = recorder
        let driver = ControlsDriver(
            stepper: stepper,
            submit: { json in recorder.submit(json) },
            runAttempt: { request, complete in recorder.attempt(request, complete) },
            clock: { clock.now },
            scheduler: scheduler.schedule)
        driver.onDraftChanged = { monitorID, feature in
            recorder.draftChanges.append("\(monitorID)/\(feature.controlsName)")
        }
        self.driver = driver
    }
}

/// Queues the DDC attempts that the real-core test starts. The test completes them from the main thread,
/// once the core lists the action that caused them. Keeps the last raw value per VCP code.
private final class RealAttempts {
    private let lock = NSLock()
    private var queued: [(request: ControlsAttemptRequest, complete: (ControlsAttemptOutcome) -> Void)] = []
    /// The discovered monitor's readings: brightness 41 and volume 50.
    private var raw: [UInt8: Double] = [16: 41, 98: 50]

    func enqueue(_ request: ControlsAttemptRequest, _ complete: @escaping (ControlsAttemptOutcome) -> Void) {
        lock.lock(); defer { lock.unlock() }
        queued.append((request: request, complete: complete))
    }

    /// Completes every queued attempt with success. A write stores its target, and a read returns the stored value.
    func completeQueued() {
        lock.lock()
        let work = queued
        queued.removeAll()
        lock.unlock()
        for item in work {
            lock.lock()
            if item.request.write { raw[item.request.code] = Double(item.request.value) }
            let value = raw[item.request.code] ?? 0
            lock.unlock()
            item.complete(.success(value: value))
        }
    }
}

@main
private struct ControlsBridgeTests {
    static func main() {
        decodeReadsEveryReplyShape()
        decodeRejectsMalformedAndUnknownReplies()
        sliderSendsEventAndReturnsDraft()
        deadlineArmsRearmsCancelsAndFires()
        submitEffectsReportAcceptance()
        runAttemptReportsOutcome()
        draftChangedReachesObserver()
        observeSendsNormalizedSnapshot()
        configureSendsVolumeOwnerOnlyWhenChanged()
        nonFiniteSliderIsNotSent()
        nonFiniteConfigureStepsSendTheDefault()
        keyEventCarriesOwnerAndConsume()
        shutdownStopsEverything()
        effectChainsStopAtDepthLimit()
        #if TELAVIVA_CORE
        realCoreStepsAndFaults()
        realCoreDriverCompletesOneAction()
        realCoreDriverRunsBrightnessKey()
        #endif
        print("Controls bridge tests passed")
    }

    private static func decode(_ text: String) -> ControlsResult? {
        ControlsResult.decode(Data(text.utf8))
    }

    private static func decodeReadsEveryReplyShape() {
        expect(decode(#"{"value":null,"effects":[],"next_deadline":null}"#) == ControlsResult.empty,
               "a null value, no effects and a null deadline decode to the empty result")

        let slider = decode(#"{"value":{"draft":37.5,"status":"pending"},"effects":[],"next_deadline":120.02}"#)
        expect(slider?.draft == ControlsDraft(hasDraft: true, draft: 37.5, status: .pending),
               "a slider reply carries its pending draft")
        expect(slider?.nextDeadline == 120.02, "a numeric next_deadline is kept")
        expect(slider?.consume == nil && slider?.effects == [], "a slider reply has no consume and no effects")

        let query = decode(#"""
        {"value":{"has_draft":false,"draft":41,"status":"ready"},"effects":[],"next_deadline":null}
        """#)
        expect(query?.draft == ControlsDraft(hasDraft: false, draft: 41, status: .ready),
               "a query_draft reply reads has_draft")
        expect(query?.nextDeadline == nil, "a null next_deadline is nil")

        let key = decode(#"{"value":{"consume":true},"effects":[],"next_deadline":null}"#)
        expect(key?.consume == true && key?.draft == nil, "a key reply carries consume only")

        let effects = decode(#"""
        {"value":null,"next_deadline":null,"effects":[
          {"type":"submit_command","token":1,"command_json":"{\"type\":\"set_brightness\"}"},
          {"type":"submit_result","token":2,"result_json":"{\"type\":\"hardware_result\"}"},
          {"type":"run_attempt","token":3,"monitor_id":"mon-1","op":"write","code":16,"value":37},
          {"type":"run_attempt","token":4,"monitor_id":"mon-2","op":"read","code":98},
          {"type":"draft_changed","monitor_id":"mon-1","feature":"volume"}
        ]}
        """#)
        let expected: [ControlsEffect] = [
            .submitCommand(token: 1, json: #"{"type":"set_brightness"}"#),
            .submitResult(token: 2, json: #"{"type":"hardware_result"}"#),
            .runAttempt(token: 3, monitorID: "mon-1", write: true, code: 16, value: 37),
            .runAttempt(token: 4, monitorID: "mon-2", write: false, code: 98, value: nil),
            .draftChanged(monitorID: "mon-1", feature: "volume")
        ]
        expect(effects?.effects == expected, "every effect type decodes with its fields")
    }

    private static func decodeRejectsMalformedAndUnknownReplies() {
        expect(decode(#"{"value":null,"effects":[{"type":"mystery"}],"next_deadline":null}"#) == nil,
               "an unknown effect type is rejected")
        expect(decode("not json") == nil, "text that is not JSON is rejected")
        expect(decode(#"{"value":null,"effects":[],"next_deadline":null} trailing"#) == nil,
               "trailing data is rejected")
        expect(decode(#"{"effects":[],"next_deadline":null}"#) == nil, "a reply without value is rejected")
        expect(decode(#"{"value":null,"next_deadline":null}"#) == nil, "a reply without effects is rejected")
        expect(decode(#"{"value":null,"effects":[]}"#) == nil, "a reply without next_deadline is rejected")
        expect(decode(#"{"value":{"draft":1,"status":"maybe"},"effects":[],"next_deadline":null}"#) == nil,
               "an unknown draft status is rejected")
        expect(decode(#"{"value":{"consume":"yes"},"effects":[],"next_deadline":null}"#) == nil,
               "a mistyped consume is rejected")
        let attempt = #"{"type":"run_attempt","token":3,"monitor_id":"mon-1""#
        let sideways = #"{"value":null,"effects":[\#(attempt),"op":"sideways","code":16}],"next_deadline":null}"#
        expect(decode(sideways) == nil,
               "an unknown attempt operation is rejected")
        expect(decode(#"{"value":null,"effects":[\#(attempt),"op":"write","code":16}],"next_deadline":null}"#) == nil,
               "a write without a value is rejected")
        expect(decode(#"{"value":null,"effects":[\#(attempt),"op":"read","code":300}],"next_deadline":null}"#) == nil,
               "a VCP code beyond one byte is rejected")
        let stringToken = #"{"value":null,"effects":[{"type":"submit_command","token":"1","command_json":"{}"}],"#
        expect(decode(stringToken + #""next_deadline":null}"#) == nil,
               "a string token is rejected")
    }

    private static func sliderSendsEventAndReturnsDraft() {
        let harness = Harness()
        let draft = ControlsDraft(hasDraft: true, draft: 37, status: .pending)
        harness.stepper.replies = [ControlsResult(draft: draft)]
        let returned = harness.driver.slider(monitorID: "mon-1", feature: .brightness, value: 37)
        expect(returned == draft, "the slider returns the draft from its reply")

        let event = harness.stepper.last
        expect(event["type"] as? String == "slider" && event["monitor_id"] as? String == "mon-1"
               && event["feature"] as? String == "brightness" && event["value"] as? Double == 37
               && event["now"] as? Double == 100, "the slider event carries the monitor, feature, value and clock")
        expect(JSONSerialization.isValidJSONObject(event), "the slider event is valid JSON")
    }

    private static func deadlineArmsRearmsCancelsAndFires() {
        let harness = Harness()
        let stepper = harness.stepper, scheduler = harness.scheduler
        let clock = harness.clock, driver = harness.driver
        stepper.replies = [
            ControlsResult(nextDeadline: 100.5),
            ControlsResult(nextDeadline: 100.5),
            ControlsResult(nextDeadline: 101.0),
            ControlsResult(),
            ControlsResult(nextDeadline: 103.0)
        ]

        driver.slider(monitorID: "mon-1", feature: .brightness, value: 1)
        expect(scheduler.armed.count == 1 && scheduler.armed[0].delay == 0.5,
               "a deadline arms one timer for the time left")
        driver.slider(monitorID: "mon-1", feature: .brightness, value: 2)
        expect(scheduler.timers.count == 1, "the same deadline keeps the armed timer")
        driver.slider(monitorID: "mon-1", feature: .brightness, value: 3)
        expect(scheduler.timers.count == 2 && scheduler.timers[0].cancelled && scheduler.armed.count == 1
               && scheduler.armed[0].delay == 1.0, "a new deadline cancels the old timer and arms a new one")
        driver.slider(monitorID: "mon-1", feature: .brightness, value: 4)
        expect(scheduler.armed.isEmpty, "a null deadline cancels the armed timer")

        clock.now = 102
        driver.slider(monitorID: "mon-1", feature: .brightness, value: 5)
        expect(scheduler.armed.count == 1 && scheduler.armed[0].delay == 1.0,
               "the delay is measured from the clock at arming")

        let beforeStaleFire = stepper.events.count
        scheduler.timers[0].fire()
        expect(stepper.events.count == beforeStaleFire, "a cancelled timer that fires sends nothing")

        stepper.replies = [ControlsResult()]
        clock.now = 103
        scheduler.armed[0].fire()
        expect(stepper.last["type"] as? String == "tick" && stepper.events.count == beforeStaleFire + 1,
               "the armed timer sends one tick")
        expect(scheduler.armed.isEmpty, "a tick that arms nothing leaves no timer")

        stepper.replies = [ControlsResult(nextDeadline: 150)]
        clock.now = 200
        driver.slider(monitorID: "mon-1", feature: .brightness, value: 6)
        expect(scheduler.armed.count == 1 && scheduler.armed[0].delay == 0,
               "a deadline in the past arms the timer with no delay")
    }

    private static func submitEffectsReportAcceptance() {
        let harness = Harness()
        let command = #"{"type":"set_brightness","monitor_id":"mon-1","connection_generation":3,"value":37}"#
        harness.stepper.replies = [ControlsResult(effects: [.submitCommand(token: 1, json: command)])]
        harness.driver.slider(monitorID: "mon-1", feature: .brightness, value: 37)
        expect(harness.recorder.submitted == [command], "the command JSON reaches submit verbatim")
        let accepted = harness.stepper.last
        expect(accepted["type"] as? String == "submitted" && accepted["token"] as? UInt64 == 1
               && accepted["accepted"] as? Bool == true, "an accepted submission reports accepted true")

        let result = #"{"type":"hardware_result","action_id":17,"generation":8,"ok":true,"value":37}"#
        harness.recorder.acceptSubmissions = false
        harness.stepper.replies = [ControlsResult(effects: [.submitResult(token: 2, json: result)])]
        harness.driver.displayPower(on: true)
        expect(harness.recorder.submitted == [command, result], "a result reaches submit verbatim")
        let rejected = harness.stepper.last
        expect(rejected["token"] as? UInt64 == 2 && rejected["accepted"] as? Bool == false,
               "a rejected submission reports accepted false")
    }

    private static func runAttemptReportsOutcome() {
        let harness = Harness()
        harness.stepper.replies = [ControlsResult(effects: [
            .runAttempt(token: 2, monitorID: "mon-1", write: true, code: 16, value: 37),
            .runAttempt(token: 3, monitorID: "mon-2", write: false, code: 98, value: nil),
            .runAttempt(token: 5, monitorID: "mon-1", write: true, code: 16, value: 36.6)
        ])]
        harness.driver.displayPower(on: true)
        let attempts = harness.recorder.attempts
        expect(attempts.count == 3, "each run_attempt effect runs one attempt")
        expect(attempts[0].request == ControlsAttemptRequest(monitorID: "mon-1", write: true, code: 16, value: 37),
               "a write carries its raw target")
        expect(attempts[1].request == ControlsAttemptRequest(monitorID: "mon-2", write: false, code: 98, value: 0),
               "a read carries no target")
        expect(attempts[2].request.value == 37, "a fractional target is rounded to the nearest integer")

        attempts[0].complete(.success(value: 37))
        let done = harness.stepper.last
        expect(done["type"] as? String == "attempt_done" && done["token"] as? UInt64 == 2
               && done["ok"] as? Bool == true && done["value"] as? Double == 37 && done["error"] == nil,
               "a successful attempt reports ok with its value and no error")

        attempts[1].complete(.failure("disconnected"))
        let failed = harness.stepper.last
        expect(failed["ok"] as? Bool == false && failed["error"] as? String == "disconnected"
               && failed["value"] is NSNull, "a failed attempt reports its error and a null value")

        harness.stepper.replies = [ControlsResult(effects: [
            .runAttempt(token: 4, monitorID: "mon-1", write: false, code: 16, value: nil)
        ])]
        harness.driver.displayPower(on: true)
        harness.recorder.attempts[3].complete(.failure(""))
        expect(harness.stepper.last["error"] as? String == "io", "a failure without a name is reported as io")
    }

    private static func draftChangedReachesObserver() {
        let harness = Harness()
        harness.stepper.replies = [ControlsResult(effects: [
            .draftChanged(monitorID: "mon-1", feature: "volume"),
            .draftChanged(monitorID: "mon-1", feature: "sound")
        ])]
        harness.driver.displayPower(on: true)
        expect(harness.recorder.draftChanges == ["mon-1/volume"],
               "a known feature reaches the observer and an unknown one does not")
    }

    private static func observeSendsNormalizedSnapshot() {
        let json = #"""
        {"revision": 42,
         "monitors": [
           {"id": "mon-1", "name": "One", "generation": 3, "display_state": "asleep", "identity_status": "stable",
            "brightness": {"supported": true, "normalized": 41, "raw": 82, "min": 0, "max": 200},
            "volume": {"supported": true, "normalized": null, "raw": 0, "min": 0, "max": 100}},
           {"id": "mon-2", "name": "Two", "generation": null},
           {"id": "mon-3", "name": "Three", "generation": 4, "volume": {"supported": false, "min": 5}}
         ],
         "pending_actions": [
           {"id": 17, "generation": 8, "monitor_id": "mon-1", "kind": "set_brightness", "value": 37.0}
         ]}
        """#
        let snapshot = try! JSONDecoder().decode(CoreSnapshot.self, from: Data(json.utf8))
        let harness = Harness()
        harness.driver.observe(snapshot)

        let event = harness.stepper.last
        expect(event["type"] as? String == "observe" && event["revision"] as? UInt64 == 42,
               "observe carries the revision")
        let monitors = event["monitors"] as? [[String: Any]] ?? []
        expect(monitors.count == 2, "a monitor without a generation is left out")

        let one = monitors[0]
        expect(one["id"] as? String == "mon-1" && one["generation"] as? UInt64 == 3
               && one["display_state"] as? String == "asleep" && one["identity_status"] as? String == "stable",
               "monitor identity and states are copied")
        let brightness = one["brightness"] as? [String: Any] ?? [:]
        expect(brightness["observed"] as? Double == 41 && brightness["min"] as? Double == 0
               && brightness["max"] as? Double == 200 && brightness["supported"] as? Bool == true,
               "brightness is the normalized percentage with the raw range")
        let volume = one["volume"] as? [String: Any] ?? [:]
        expect(volume["observed"] is NSNull, "a null observed stays null")

        let missing = monitors[1]
        expect(missing["display_state"] as? String == "awake" && missing["identity_status"] as? String == "ambiguous",
               "missing states take their defaults")
        let absentBrightness = missing["brightness"] as? [String: Any] ?? [:]
        expect(absentBrightness["supported"] as? Bool == false && absentBrightness["observed"] is NSNull
               && absentBrightness["min"] as? Double == 0 && absentBrightness["max"] as? Double == 100,
               "a missing channel is unsupported with the default range")
        let partialVolume = missing["volume"] as? [String: Any] ?? [:]
        expect(partialVolume["min"] as? Double == 5 && partialVolume["max"] as? Double == 100,
               "a missing max takes the default while a given min is kept")

        let pending = event["pending_actions"] as? [[String: Any]] ?? []
        expect(pending.count == 1 && pending[0]["id"] as? UInt64 == 17 && pending[0]["value"] as? Double == 37
               && pending[0]["kind"] as? String == "set_brightness", "pending actions carry their value")
        expect(JSONSerialization.isValidJSONObject(event), "the observe event is valid JSON")

        let count = harness.stepper.events.count
        harness.driver.observe(CoreSnapshot.unavailable)
        expect(harness.stepper.events.count == count, "an unpublished snapshot sends nothing")
    }

    private static func configureSendsVolumeOwnerOnlyWhenChanged() {
        let harness = Harness()
        harness.driver.configure(brightnessStep: 5, volumeStep: 7, keysEnabled: true, volumeOwner: .unchanged)
        var event = harness.stepper.last
        expect(event["type"] as? String == "configure" && event["brightness_step"] as? Double == 5
               && event["volume_step"] as? Double == 7 && event["keys_enabled"] as? Bool == true
               && event["volume_owner"] == nil, "an unchanged owner is left out of configure")

        harness.driver.configure(brightnessStep: 5, volumeStep: 7, keysEnabled: true, volumeOwner: .none)
        event = harness.stepper.last
        expect(event["volume_owner"] is NSNull, "no owner is sent as null")

        harness.driver.configure(brightnessStep: 5, volumeStep: 7, keysEnabled: true, volumeOwner: .monitor("mon-1"))
        event = harness.stepper.last
        expect(event["volume_owner"] as? String == "mon-1", "an owner is sent as its monitor ID")
    }

    private static func nonFiniteSliderIsNotSent() {
        let harness = Harness()
        for value in [Double.nan, .infinity, -.infinity] {
            expect(harness.driver.slider(monitorID: "mon-1", feature: .brightness, value: value) == nil,
                   "a non-finite slider value returns nil")
        }
        expect(harness.stepper.events.isEmpty, "a non-finite slider value sends no event")
    }

    private static func nonFiniteConfigureStepsSendTheDefault() {
        let harness = Harness()
        harness.driver.configure(brightnessStep: .nan, volumeStep: .infinity, keysEnabled: true,
                                 volumeOwner: .unchanged)
        var event = harness.stepper.last
        expect(event["type"] as? String == "configure" && event["brightness_step"] as? Double == 5
               && event["volume_step"] as? Double == 5, "non-finite steps are sent as the key default of 5")
        expect(JSONSerialization.isValidJSONObject(event), "the configure event with non-finite steps is valid JSON")

        harness.driver.configure(brightnessStep: -.infinity, volumeStep: 7, keysEnabled: true, volumeOwner: .none)
        event = harness.stepper.last
        expect(event["brightness_step"] as? Double == 5 && event["volume_step"] as? Double == 7,
               "a negative infinite step is sent as 5 and a finite step is kept")
    }

    private static func keyEventCarriesOwnerAndConsume() {
        let harness = Harness()
        harness.stepper.replies = [ControlsResult(consume: true)]
        let owner = HardwareKeyOwner(monitorID: "mon-1", generation: 3)
        let consumed = harness.driver.key("volume_up", down: true, isRepeat: false, fine: true, owner: owner)
        expect(consumed, "the key answer is returned as consume")
        let event = harness.stepper.last
        expect(event["type"] as? String == "key" && event["key"] as? String == "volume_up"
               && event["phase"] as? String == "down" && event["repeat"] as? Bool == false
               && event["fine"] as? Bool == true, "the key event carries the key, phase, repeat and fine")
        let eventOwner = event["owner"] as? [String: Any] ?? [:]
        expect(eventOwner["monitor_id"] as? String == "mon-1" && eventOwner["generation"] as? UInt64 == 3,
               "the key owner carries the monitor and generation")

        harness.stepper.replies = [ControlsResult(consume: false)]
        let passed = harness.driver.key("mute", down: false, isRepeat: true, fine: false, owner: nil)
        let release = harness.stepper.last
        expect(!passed && release["owner"] is NSNull && release["phase"] as? String == "up"
               && release["repeat"] as? Bool == true, "a key without an owner is a release that passes")

        harness.stepper.replies = [ControlsResult()]
        expect(!harness.driver.key("brightness_down", down: true, isRepeat: false, fine: false, owner: nil),
               "a reply without a consume answer does not consume")
    }

    private static func shutdownStopsEverything() {
        let harness = Harness()
        let stepper = harness.stepper, driver = harness.driver
        stepper.replies = [
            ControlsResult(effects: [.runAttempt(token: 1, monitorID: "mon-1", write: true, code: 16, value: 40)],
                           nextDeadline: 103),
            // The reply to shutdown is ignored, so its submission must never happen.
            ControlsResult(effects: [.submitCommand(token: 2, json: "{}")])
        ]
        driver.slider(monitorID: "mon-1", feature: .brightness, value: 40)
        let attempt = harness.recorder.attempts[0].complete
        let timer = harness.scheduler.timers[0]

        driver.shutdown()
        expect(stepper.last["type"] as? String == "shutdown", "shutdown is sent to the core")
        expect(timer.cancelled, "shutdown cancels the armed timer")
        expect(harness.recorder.submitted.isEmpty, "the reply to shutdown runs no effects")

        let afterShutdown = stepper.events.count
        expect(driver.slider(monitorID: "mon-1", feature: .brightness, value: 1) == nil,
               "a slider after shutdown returns nil")
        expect(!driver.key("volume_up", down: true, isRepeat: false, fine: false, owner: nil),
               "a key after shutdown is not consumed")
        expect(driver.queryDraft(monitorID: "mon-1", feature: .volume)
               == ControlsDraft(hasDraft: false, draft: 0, status: .ready),
               "a query after shutdown returns the idle draft")
        timer.fire()
        attempt(.success(value: 40))
        driver.shutdown()
        expect(stepper.events.count == afterShutdown, "after shutdown the core sees nothing more")
        expect(harness.recorder.attempts.count == 1 && harness.recorder.submitted.isEmpty,
               "a late attempt completion runs no further effects")
    }

    private static func effectChainsStopAtDepthLimit() {
        let harness = Harness()
        var nextToken: UInt64 = 1
        // Every submission answers with another submission, so only the depth limit can end the chain.
        harness.stepper.respond = { event in
            guard let type = event["type"] as? String, type == "display_power" || type == "submitted" else {
                return ControlsResult.empty
            }
            nextToken += 1
            return ControlsResult(effects: [.submitCommand(token: nextToken, json: "{}")])
        }
        harness.driver.displayPower(on: true)
        expect(harness.recorder.submitted.count == 65, "an effect chain stops after the depth limit of 64")
    }

    #if TELAVIVA_CORE
    /// The real `TvControls` handle: it accepts well-formed events and faults on the first rejected one.
    private static func realCoreStepsAndFaults() {
        let bridge = ControlsBridge()
        expect(!bridge.isFaulted, "a new handle is not faulted")
        expect(bridge.step(["type": "configure", "now": 1.0, "brightness_step": 5.0, "volume_step": 5.0,
                            "keys_enabled": true]) != nil, "configure is accepted by the core")
        expect(bridge.step(["type": "observe", "now": 1.5, "revision": 1, "monitors": [],
                            "pending_actions": []]) != nil, "an empty observation is accepted")
        expect(!bridge.isFaulted, "accepted events leave the bridge healthy")
        expect(bridge.step(["type": "no_such_event", "now": 2.0]) == nil, "an unknown event is rejected")
        expect(bridge.isFaulted, "a rejected event faults the bridge")
        expect(bridge.step(["type": "tick", "now": 3.0]) == nil, "a faulted bridge returns nil")
    }

    /// Runs one manual slider change through the real core and the driver: the core lists the action,
    /// the attempt runs, the result is submitted and the draft clears.
    private static func realCoreDriverCompletesOneAction() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-controls-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let monitorID = "controls-monitor"
        let core = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(pump { core.snapshot.revision != nil }, "the core publishes its first snapshot")
        expect(core.submit(["type": "monitor_discovered", "monitor": [
            "id": monitorID, "name": "Controls", "identity_status": "stable",
            "connection_path": "test", "brightness": ["supported": true, "value": 41, "raw": 41, "min": 0, "max": 100],
            "volume": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100]
        ]]), "the synthetic monitor is accepted by the local core")
        core.refresh()

        let controls = ControlsBridge()
        let attempts = RealAttempts()
        let driver = ControlsDriver(
            stepper: controls,
            submit: { json in core.submitJSON(json) },
            runAttempt: { request, complete in attempts.enqueue(request, complete) })
        driver.configure(brightnessStep: 5, volumeStep: 5, keysEnabled: true, volumeOwner: .unchanged)

        // Feeds each newly published snapshot to the driver, once.
        var observed: UInt64?
        func feedDriver() {
            guard core.snapshot.revision != observed else { return }
            observed = core.snapshot.revision
            driver.observe(core.snapshot)
        }
        func listsAction() -> Bool {
            let actions = core.snapshot.pending_actions ?? []
            return actions.contains { $0.monitor_id == monitorID && $0.kind == "set_brightness" }
        }

        expect(pump {
            feedDriver()
            return core.snapshot.monitors?.contains(where: { $0.id == monitorID && $0.generation != nil }) == true
        }, "the monitor with a generation reaches the driver")

        let draft = driver.slider(monitorID: monitorID, feature: .brightness, value: 73)
        expect(draft?.hasDraft == true && draft?.draft == 73, "the slider answers with the target as the draft")
        // The attempts are completed only after the action is visible in the snapshot.
        expect(pump { feedDriver(); return listsAction() }, "the core lists the set_brightness action")
        expect(pump {
            feedDriver()
            attempts.completeQueued()
            let state = driver.queryDraft(monitorID: monitorID, feature: .brightness)
            return (core.snapshot.pending_actions ?? []).isEmpty && (!state.hasDraft || state.status == .ready)
        }, "the action completes, the list empties and the draft clears")
        expect(!controls.isFaulted, "every event the driver sent was accepted by the core")

        driver.shutdown()
        var finished = false
        core.beginShutdown { finished = true }
        expect(pump { finished }, "shutdown completes before the temporary directory is removed")
    }

    /// Holds a brightness key through the real core and the driver: the press asks for one step of +5 from the
    /// observed 41, the core lists the action at 46, the attempt runs and the list empties.
    private static func realCoreDriverRunsBrightnessKey() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-controls-key-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let monitorID = "key-monitor"
        let core = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(pump { core.snapshot.revision != nil }, "the core publishes its first snapshot")
        expect(core.submit(["type": "monitor_discovered", "monitor": [
            "id": monitorID, "name": "Keys", "identity_status": "stable",
            "connection_path": "test", "brightness": ["supported": true, "value": 41, "raw": 41, "min": 0, "max": 100],
            "volume": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100]
        ]]), "the synthetic monitor is accepted by the local core")
        core.refresh()

        let controls = ControlsBridge()
        let attempts = RealAttempts()
        let driver = ControlsDriver(
            stepper: controls,
            submit: { json in core.submitJSON(json) },
            runAttempt: { request, complete in attempts.enqueue(request, complete) })
        driver.configure(brightnessStep: 5, volumeStep: 5, keysEnabled: true, volumeOwner: .none)

        // Feeds each newly published snapshot to the driver, once.
        var observed: UInt64?
        func feedDriver() {
            guard core.snapshot.revision != observed else { return }
            observed = core.snapshot.revision
            driver.observe(core.snapshot)
        }
        func listsAction(value: Double) -> Bool {
            let actions = core.snapshot.pending_actions ?? []
            return actions.contains { $0.monitor_id == monitorID && $0.kind == "set_brightness" && $0.value == value }
        }

        var generation: UInt64?
        expect(pump {
            feedDriver()
            generation = core.snapshot.monitors?.first(where: { $0.id == monitorID })?.generation
            return generation != nil
        }, "the monitor with a generation reaches the driver")
        let owner = HardwareKeyOwner(monitorID: monitorID, generation: generation ?? 0)

        expect(driver.key("brightness_up", down: true, isRepeat: false, fine: false, owner: owner),
               "the brightness key press is consumed")
        expect(driver.key("brightness_up", down: false, isRepeat: false, fine: false, owner: owner),
               "the brightness key release is consumed")
        // The attempts are completed only after the action is visible in the snapshot.
        expect(pump { feedDriver(); return listsAction(value: 46) },
               "the core lists the set_brightness action at 46")
        expect(pump {
            feedDriver()
            attempts.completeQueued()
            return (core.snapshot.pending_actions ?? []).isEmpty
        }, "the action completes and the list empties")
        expect(!driver.key("brightness_up", down: false, isRepeat: false, fine: false, owner: nil),
               "a key event without an owner passes")
        expect(!controls.isFaulted, "every event the driver sent was accepted by the core")

        driver.shutdown()
        var finished = false
        core.beginShutdown { finished = true }
        expect(pump { finished }, "shutdown completes before the temporary directory is removed")
    }

    /// Runs the main run loop until `step` returns true. `step` runs once per turn. Returns false on timeout.
    private static func pump(timeout: TimeInterval = 5, _ step: () -> Bool) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while true {
            if step() { return true }
            if Date() >= deadline { return false }
            _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.01))
        }
    }
    #endif
}
