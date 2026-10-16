import AppKit
import Foundation
import IOKit.hidsystem

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("HardwareKeyRouterTests: \(message)") }
}

private func display(id: String = "monitor-a", generation: UInt64 = 1, origin: CGPoint = .zero,
                     bounds: CGRect = CGRect(x: 0, y: 0, width: 1000, height: 800),
                     builtin: Bool = false, brightness: Double? = 50, volume: Double? = 50,
                     brightnessSupported: Bool = true, volumeSupported: Bool = true,
                     awake: Bool = true, audioUID: String? = nil) -> HardwareKeyDisplay {
    HardwareKeyDisplay(displayID: 7, bounds: bounds.offsetBy(dx: origin.x, dy: origin.y), isBuiltin: builtin,
                       monitorID: id, generation: generation, audioDeviceUID: audioUID,
                       brightness: HardwareKeyFeatureState(value: brightness, supported: brightnessSupported, awake: awake),
                       volume: HardwareKeyFeatureState(value: volume, supported: volumeSupported, awake: awake))
}

private func key(_ keyType: Int, phase: HardwareKeyEvent.Phase = .down, repeat isRepeat: Bool = false,
                 fine: Bool = false, modifiersEligible: Bool = true) -> HardwareKeyEvent {
    let feature: HardwareKeyFeature = (keyType == Int(NX_KEYTYPE_SOUND_UP) || keyType == Int(NX_KEYTYPE_SOUND_DOWN)) ? .volume : .brightness
    let direction: HardwareKeyEvent.Direction = (keyType == Int(NX_KEYTYPE_BRIGHTNESS_DOWN) || keyType == Int(NX_KEYTYPE_SOUND_DOWN)) ? .decrease : .increase
    return HardwareKeyEvent(keyIdentity: keyType, feature: feature, direction: direction, phase: phase,
                            isRepeat: isRepeat, isFineStep: fine, modifiersEligible: modifiersEligible)
}

private func muteKey(phase: HardwareKeyEvent.Phase = .down, repeat isRepeat: Bool = false) -> HardwareKeyEvent {
    HardwareKeyEvent(keyIdentity: Int(NX_KEYTYPE_MUTE), feature: .volume, direction: .decrease, phase: phase,
                     isRepeat: isRepeat, isFineStep: false, modifiersEligible: true, isMute: true)
}

private let outputUID = "1E6D8877-0000-0000-0722-0104B5462878"
private let elsewhere = CGPoint(x: 5000, y: 5000)

private final class FakeSubmission {
    private let condition = NSCondition()
    private(set) var commands: [HardwareKeyCommand] = []
    private var completions: [((Bool) -> Void)] = []

    func submit(_ command: HardwareKeyCommand, completion: @escaping (Bool) -> Void) {
        condition.lock(); commands.append(command); completions.append(completion); condition.broadcast(); condition.unlock()
    }

    func waitForCommands(_ count: Int) -> [HardwareKeyCommand] {
        condition.lock()
        let deadline = Date(timeIntervalSinceNow: 3)
        while commands.count < count && condition.wait(until: deadline) {}
        let result = commands
        condition.unlock()
        expect(result.count >= count, "expected async submission count \(count), got \(result.count)")
        return result
    }

    func acknowledge(_ index: Int, accepted: Bool) {
        condition.lock(); let completion = completions[index]; condition.unlock(); completion(accepted)
    }
}

/// Runs the main run loop briefly so asynchronous status and permission publications are delivered.
private func drain() { RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.05)) }

private final class FakePermissions {
    var value: HardwareKeyPermissions
    init(_ accessibility: Bool, _ inputMonitoring: Bool) {
        value = HardwareKeyPermissions(accessibility: accessibility, inputMonitoring: inputMonitoring)
    }
    func set(_ accessibility: Bool, _ inputMonitoring: Bool) {
        value = HardwareKeyPermissions(accessibility: accessibility, inputMonitoring: inputMonitoring)
    }
}

private final class FakeTapInstaller {
    var allow = false
    private(set) var calls = 0

    func install() -> (CFMachPort, CFRunLoopSource)? {
        calls += 1
        guard allow, let port = CFMachPortCreate(kCFAllocatorDefault, { _, _, _, _ in }, nil, nil),
              let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, port, 0) else { return nil }
        return (port, source)
    }
}

@main
private struct HardwareKeyRouterTests {
    static func main() {
        sdkEventClassification()
        keyboardBrightnessEvents()
        targetAndStepPolicy()
        sequenceRouting()
        volumeTargetPolicy()
        volumeFollowsSoundOutput()
        volumeHoldAcrossOutputChanges()
        volumeBoundariesModifiersAndLatches()
        muteBehavior()
        volumeStaleGenerationPassesThrough()
        disableAndShutdownClearOutput()
        permissionLossKeepsOutput()
        statusPolicyMatrix()
        permissionChangeNotifications()
        tapRetryAfterFailure()
        inputMonitoringAndRevocationStatus()
        intentCoalescingAndRebase()
        laterKeySupersedesOlderManualAcceptance()
        rejectedAndStaleWork()
        handoffInvalidationAndLateShutdown()
        observedChangesAndOrphanRepeats()
        rebasingDoesNotGrowInFlightWork()
        print("Hardware-key router and coordinator tests passed")
    }

    private static func sdkEventClassification() {
        let normal = CGEventFlags(rawValue: 0)
        let type = CGEventType(rawValue: UInt32(NX_SYSDEFINED))!
        func payload(_ key: Int, state: Int = 0x0A, repeat repeated: Bool = false) -> Int64 {
            Int64((UInt64(key) << 16) | (UInt64(state) << 8) | (repeated ? 1 : 0))
        }
        let brightnessUp = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                                    data1: payload(Int(NX_KEYTYPE_BRIGHTNESS_UP)), flags: normal)
        expect(brightnessUp?.feature == .brightness && brightnessUp?.direction == .increase, "SDK brightness-up code decodes")
        let soundDown = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                                 data1: payload(Int(NX_KEYTYPE_SOUND_DOWN), state: 0x0B), flags: normal)
        expect(soundDown?.feature == .volume && soundDown?.phase == .up, "SDK sound-down key-up decodes")
        let fine = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                            data1: payload(Int(NX_KEYTYPE_SOUND_UP)), flags: [.maskShift, .maskAlternate])
        expect(fine?.isFineStep == true, "exact Shift+Option is the fine step")
        let command = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                               data1: payload(Int(NX_KEYTYPE_SOUND_UP)), flags: [.maskCommand])
        expect(command?.modifiersEligible == false, "Command is preserved for the OS")
        let mute = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                            data1: payload(Int(NX_KEYTYPE_MUTE)), flags: normal)
        expect(mute != nil, "SDK mute code decodes")
        expect(mute?.isMute == true && mute?.feature == .volume, "mute is a volume toggle")
        expect(mute?.phase == .down && mute?.modifiersEligible == true, "plain mute press is eligible")
        let shiftedMute = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                                  data1: payload(Int(NX_KEYTYPE_MUTE)), flags: [.maskShift, .maskAlternate])
        expect(shiftedMute?.modifiersEligible == false, "Shift+Option is not a mute combination")
        let commandMute = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                                  data1: payload(Int(NX_KEYTYPE_MUTE)), flags: [.maskCommand])
        expect(commandMute?.modifiersEligible == false, "Command mute is preserved for the OS")
        let muteUp = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                             data1: payload(Int(NX_KEYTYPE_MUTE), state: 0x0B), flags: normal)
        expect(muteUp?.phase == .up, "SDK mute key-up decodes")
        let muteRepeat = HardwareKeyEvent.decode(type: type, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                                 data1: payload(Int(NX_KEYTYPE_MUTE), repeat: true), flags: normal)
        expect(muteRepeat?.isRepeat == true, "SDK mute repeat decodes")
        expect(HardwareKeyEvent.decode(type: .keyDown, subtype: Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS),
                                       data1: payload(Int(NX_KEYTYPE_BRIGHTNESS_UP)), flags: normal) == nil,
               "ordinary keyboard events do not classify as media keys")
        expect(HardwareKeyEvent.decode(type: type, subtype: 0,
                                       data1: payload(Int(NX_KEYTYPE_BRIGHTNESS_UP)), flags: normal) == nil,
               "non-auxiliary system-defined events pass through")
    }

    /// Brightness keys on Apple Silicon keyboards arrive as ordinary key events with key codes 144 (up) and 145 (down).
    private static func keyboardBrightnessEvents() {
        let fn: CGEventFlags = [.maskSecondaryFn]
        let press = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 144, isRepeat: false, flags: fn)
        expect(press?.feature == .brightness && press?.direction == .increase, "keyboard key code 144 is brightness up")
        expect(press?.keyIdentity == Int(NX_KEYTYPE_BRIGHTNESS_UP), "keyboard brightness up shares the media key identity")
        expect(press?.phase == .down && press?.isRepeat == false, "keyboard brightness press is a first key-down")
        expect(press?.modifiersEligible == true && press?.isFineStep == false, "fn alone keeps keyboard brightness eligible")
        let release = HardwareKeyEvent.decodeKeyboard(type: .keyUp, keyCode: 144, isRepeat: false, flags: fn)
        expect(release?.phase == .up && release?.feature == .brightness, "keyboard brightness key-up decodes")
        let lower = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 145, isRepeat: false, flags: fn)
        expect(lower?.feature == .brightness && lower?.direction == .decrease, "keyboard key code 145 is brightness down")
        expect(lower?.keyIdentity == Int(NX_KEYTYPE_BRIGHTNESS_DOWN), "keyboard brightness down shares the media key identity")
        let repeated = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 144, isRepeat: true, flags: fn)
        expect(repeated?.isRepeat == true, "keyboard autorepeat decodes")
        let fine = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 144, isRepeat: false,
                                                   flags: [.maskSecondaryFn, .maskShift, .maskAlternate])
        expect(fine?.isFineStep == true && fine?.modifiersEligible == true, "keyboard shift+option is the fine step")
        let withCommand = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 144, isRepeat: false,
                                                          flags: [.maskSecondaryFn, .maskCommand])
        expect(withCommand?.modifiersEligible == false, "keyboard command is preserved for the os")
        let withControl = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 144, isRepeat: false,
                                                          flags: [.maskSecondaryFn, .maskControl])
        expect(withControl?.modifiersEligible == false, "keyboard control is preserved for the os")
        let otherCodes: [Int64] = [0, 122, 120, 143, 146, 160]
        for code in otherCodes {
            expect(HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: code, isRepeat: false, flags: fn) == nil,
                   "ordinary key codes are not brightness keys")
        }
        expect(HardwareKeyEvent.decodeKeyboard(type: .flagsChanged, keyCode: 144, isRepeat: false, flags: fn) == nil,
               "flags-changed events do not decode as keys")
        expect(HardwareKeyEvent.decodeKeyboard(type: .mouseMoved, keyCode: 144, isRepeat: false, flags: fn) == nil,
               "mouse events do not decode as keys")

        // Routing mirrors the media key: a press follows the pointer, and a held key coalesces its repeats.
        func keyboard(_ type: CGEventType, repeat isRepeat: Bool = false) -> HardwareKeyEvent {
            guard let event = HardwareKeyEvent.decodeKeyboard(type: type, keyCode: 144, isRepeat: isRepeat, flags: fn) else {
                fatalError("HardwareKeyRouterTests: keyboard brightness key did not decode")
            }
            return event
        }
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let monitor = display()
        expect(router.processForTesting(keyboard(.keyDown), pointer: .zero, displays: [monitor]), "keyboard press is consumed")
        // Waiting for the first command keeps the repeat's coalesced write deterministic, as in intentCoalescingAndRebase.
        let first = fake.waitForCommands(1)
        expect(first.count == 1 && first[0].monitorID == "monitor-a" && first[0].feature == .brightness,
               "keyboard press targets the brightness of the monitor under the pointer")
        expect(first[0].target == 50 + HardwareKeyPolicy.normalStep, "keyboard press steps brightness by the normal step")
        expect(router.processForTesting(keyboard(.keyDown, repeat: true), pointer: .zero, displays: [monitor]),
               "keyboard autorepeat is consumed")
        expect(router.processForTesting(keyboard(.keyUp), pointer: .zero, displays: [monitor]), "keyboard release is consumed")
        expect(fake.commands.count == 1, "held keyboard brightness submits no second command while in flight")
        fake.acknowledge(0, accepted: true)
        let coalesced = fake.waitForCommands(2)[1]
        expect(coalesced.target == 50 + 2 * HardwareKeyPolicy.normalStep, "keyboard repeat coalesces into one follow-up write")
        fake.acknowledge(1, accepted: true)
        router.waitForWorkerForTesting()
        router.shutdown()

        // Outside every display the whole sequence passes through and nothing is submitted.
        let passFake = FakeSubmission()
        let passRouter = HardwareKeyRouter(submit: passFake.submit)
        let passDown = passRouter.processForTesting(keyboard(.keyDown), pointer: elsewhere, displays: [monitor])
        let passRepeat = passRouter.processForTesting(keyboard(.keyDown, repeat: true), pointer: elsewhere, displays: [monitor])
        let passUp = passRouter.processForTesting(keyboard(.keyUp), pointer: elsewhere, displays: [monitor])
        expect((passDown, passRepeat, passUp) == (false, false, false), "keyboard brightness outside every display passes through")
        passRouter.waitForWorkerForTesting()
        expect(passFake.commands.isEmpty, "pass-through keyboard brightness submits nothing")
        passRouter.shutdown()
    }

    private static func targetAndStepPolicy() {
        let first = display()
        expect(HardwareKeyPolicy.target(at: CGPoint(x: 20, y: 20), displays: [first], feature: .brightness) != nil,
               "single eligible external target is selected")
        expect(HardwareKeyPolicy.target(at: CGPoint(x: 20, y: 20), displays: [first, display(id: "b")], feature: .brightness) == nil,
               "overlapping display geometry is ambiguous")
        expect(HardwareKeyPolicy.target(at: CGPoint(x: 1200, y: 20), displays: [first], feature: .brightness) == nil,
               "pointer gap has no fallback")
        expect(HardwareKeyPolicy.target(at: CGPoint(x: 20, y: 20), displays: [display(builtin: true)], feature: .brightness) == nil,
               "built-in display cannot receive keys")
        expect(HardwareKeyPolicy.target(at: CGPoint(x: 20, y: 20), displays: [display(brightnessSupported: false)], feature: .brightness) == nil,
               "unsupported feature passes through independently")
        expect(HardwareKeyPolicy.target(at: CGPoint(x: 20, y: 20), displays: [display(awake: false)], feature: .brightness) == nil,
               "sleeping display passes through")
        expect(HardwareKeyPolicy.stepped(96.875, direction: .increase, fine: false) == 100, "normal step clamps at maximum")
        expect(HardwareKeyPolicy.stepped(0, direction: .decrease, fine: true) == 0, "fine step clamps at minimum")
        expect(HardwareKeyPolicy.stepped(50, direction: .increase, fine: false) == 56.25, "normal step is 6.25")
        expect(HardwareKeyPolicy.stepped(50, direction: .decrease, fine: true) == 48.4375, "fine step is 1.5625")
    }

    private static func sequenceRouting() {
        var sequence = HardwareKeySequence()
        let initial = display()
        var values: [String: Double] = ["monitor-a": 50]
        var writes: [Double] = []
        let valueFor: (HardwareKeyDisplay, HardwareKeyFeature) -> Double? = { d, _ in values[d.monitorID!] }
        let reserve: (HardwareKeyDisplay, HardwareKeyFeature, HardwareKeyEvent.Direction, Bool, Double) -> Bool = { d, _, direction, fine, current in
            let next = HardwareKeyPolicy.stepped(current, direction: direction, fine: fine)
            if next != current { writes.append(next); values[d.monitorID!] = next }
            return true
        }
        let down = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        expect(sequence.process(down, pointer: CGPoint(x: 1, y: 1), enabled: true, defaultOutputUID: nil, displays: [initial], valueFor: valueFor, reserve: reserve),
               "eligible keydown is consumed after reserving")
        expect(sequence.process(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: CGPoint(x: 1200, y: 10), enabled: true, defaultOutputUID: nil,
                                displays: [initial], valueFor: valueFor, reserve: reserve), "repeat stays latched across pointer movement")
        expect(writes == [56.25, 62.5], "repeat increments the original monitor intent")
        expect(sequence.process(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up, modifiersEligible: false), pointer: .zero,
                                enabled: true, defaultOutputUID: nil, displays: [initial], valueFor: valueFor, reserve: reserve),
               "handled key-up remains consumed after modifiers change")
        expect(writes.count == 2, "key-up applies no delta")

        expect(!sequence.process(key(Int(NX_KEYTYPE_SOUND_UP), modifiersEligible: false), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                  displays: [initial], valueFor: valueFor, reserve: reserve), "ineligible modifiers pass through")
        expect(!sequence.process(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                 displays: [initial], valueFor: valueFor, reserve: reserve), "unhandled sequence key-up passes through")
        expect(!sequence.process(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: CGPoint(x: 2000, y: 1), enabled: true, defaultOutputUID: nil,
                                 displays: [initial], valueFor: valueFor, reserve: reserve), "ineligible initial target passes through")
        expect(!sequence.process(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                 displays: [initial], valueFor: valueFor, reserve: reserve), "no-hit sequence remains passthrough")

        let boundary = display(brightness: 100)
        values["monitor-a"] = 100
        expect(sequence.process(down, pointer: .zero, enabled: true, defaultOutputUID: nil, displays: [boundary], valueFor: valueFor, reserve: reserve),
               "owned key at maximum is consumed")
        expect(writes.count == 2, "boundary press reserves no redundant write")
        expect(sequence.process(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                displays: [boundary], valueFor: valueFor, reserve: reserve), "boundary release is consumed")

        values["monitor-a"] = 50
        expect(sequence.process(down, pointer: .zero, enabled: true, defaultOutputUID: nil, displays: [initial], valueFor: valueFor, reserve: reserve), "fresh press starts a new latch")
        let replaced = display(generation: 2)
        expect(sequence.process(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                   displays: [replaced], valueFor: valueFor, reserve: reserve), "stale held key remains consumed")
        expect(sequence.process(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                  displays: [replaced], valueFor: valueFor, reserve: reserve), "stale held key-up remains consumed")
        expect(writes.count == 3, "stale generation cancels future deltas")
        sequence.clear()
        expect(!sequence.process(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: .zero, enabled: true, defaultOutputUID: nil,
                                 displays: [initial], valueFor: valueFor, reserve: reserve), "teardown clears held-key state")
    }

    private static func intentCoalescingAndRebase() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let d = display()
        let down = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        expect(router.processForTesting(down, pointer: .zero, displays: [d]), "first press reserves an intent")
        let first = fake.waitForCommands(1)
        expect(first[0].target == 56.25 && first[0].generation == 1, "first worker command has normalized target and generation")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: .zero, displays: [d]), "repeat while first command is in flight is consumed")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: .zero, displays: [d]), "second repeat accumulates while first command is in flight")
        fake.acknowledge(0, accepted: true)
        let second = fake.waitForCommands(2)
        expect(second[1].target == 68.75, "in-flight repeats coalesce to latest accumulated value")
        fake.acknowledge(1, accepted: true)

        router.manualControlChanged(monitorID: "monitor-a", generation: 1, feature: .brightness, value: 20, revision: 1)
        expect(router.processForTesting(down, pointer: .zero, displays: [d]), "manual rebase is used by next key")
        let third = fake.waitForCommands(3)
        expect(third[2].target == 26.25, "manual app value rebases future keyboard steps")
        fake.acknowledge(2, accepted: false)
    }

    private static func rejectedAndStaleWork() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let d = display(audioUID: outputUID)
        router.setDefaultOutput(uid: outputUID)
        let down = key(Int(NX_KEYTYPE_SOUND_UP))
        expect(router.processForTesting(down, pointer: .zero, displays: [d]), "rejected submission event is already consumed")
        let submitted = fake.waitForCommands(1)
        expect(submitted[0].feature == .volume, "volume command uses corresponding feature")
        fake.acknowledge(0, accepted: false)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: .zero, displays: [d]),
                                        "submission failure does not reinject or unconsume the paired release")

        let staleFake = FakeSubmission()
        let staleRouter = HardwareKeyRouter(submit: staleFake.submit)
        expect(staleRouter.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: .zero, displays: [d]), "stale-work setup press")
        _ = staleFake.waitForCommands(1)
        expect(staleRouter.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: .zero,
                                              displays: [display(generation: 2)]), "stale repeat remains consumed")
        expect(staleRouter.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: .zero,
                                              displays: [display(generation: 2)]), "stale hold release remains consumed")
        staleFake.acknowledge(0, accepted: true)
        // Acknowledgement of an in-flight stale command cannot schedule any write for its old generation.
        staleRouter.shutdown()
    }

    private static func laterKeySupersedesOlderManualAcceptance() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let d = display()
        _ = router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: .zero, displays: [d])
        router.manualControlChanged(monitorID: "monitor-a", generation: 1, feature: .brightness, value: 20, revision: 42)
        let up = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        expect(router.processForTesting(up, pointer: .zero, displays: [d]), "key after slider reserves a newer target")
        let first = fake.waitForCommands(1)[0]
        expect(first.target == 26.25, "key target starts from latest slider value")

        router.manualControlAccepted(monitorID: "monitor-a", generation: 1, feature: .brightness,
                                     value: 20, revision: 42)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: .zero, displays: [d]),
               "held key remains routed after the late slider acceptance")
        fake.acknowledge(0, accepted: true)
        let second = fake.waitForCommands(2)[1]
        expect(second.target == 32.5, "older slider acceptance cannot rebase over the newer key target")
        fake.acknowledge(1, accepted: false)
        router.shutdown()
    }
    private static func handoffInvalidationAndLateShutdown() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let d = display()
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: .zero, displays: [d]), "handoff setup")
        let old = fake.waitForCommands(1)[0]
        expect(router.isCommandCurrent(old), "a reserved command remains valid before handoff")
        router.manualControlChanged(monitorID: "monitor-a", generation: 1, feature: .brightness, value: 20, revision: 1)
        expect(!router.isCommandCurrent(old), "an accepted slider command invalidates an already handed-off key")
        fake.acknowledge(0, accepted: false)
        router.waitForWorkerForTesting()
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: .zero, displays: [d]), "held key continues from the manual rebase")
        let current = fake.waitForCommands(2)[1]
        expect(current.target == 26.25 && router.isCommandCurrent(current), "stale completion cannot erase a newer manual epoch")
        router.shutdown()
        expect(!router.isCommandCurrent(current), "shutdown invalidates handed-off work")
        router.apply(enabled: true, displays: [d])
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [d]), "late snapshots cannot restart a stopped router")
        fake.acknowledge(1, accepted: true)
        router.waitForWorkerForTesting()
    }

    private static func observedChangesAndOrphanRepeats() {
        var sequence = HardwareKeySequence()
        var reserved = false
        expect(!sequence.process(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: .zero,
                                 enabled: true, defaultOutputUID: nil, displays: [display()], valueFor: { _, _ in 50 },
                                 reserve: { _, _, _, _, _ in reserved = true; return true }),
               "a repeat without its initial down must pass through")
        expect(!reserved, "orphan repeats reserve no work")

        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        router.setDefaultOutput(uid: outputUID)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [display(audioUID: outputUID)]), "observed rebase setup")
        _ = fake.waitForCommands(1)
        fake.acknowledge(0, accepted: true)
        router.waitForWorkerForTesting()
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: .zero, displays: [display(volume: 56, audioUID: outputUID)])
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [display(volume: 30, audioUID: outputUID)]), "external adjustment is reconciled before a new press")
        let updated = fake.waitForCommands(2)[1]
        expect(updated.target == 36.25, "external observed change replaces idle keyboard intent")
        fake.acknowledge(1, accepted: true)
        router.waitForWorkerForTesting()
        router.shutdown()
    }

    private static func rebasingDoesNotGrowInFlightWork() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let d = display(audioUID: outputUID)
        router.setDefaultOutput(uid: outputUID)
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [d])
        _ = fake.waitForCommands(1)
        for revision in 0..<100 {
            router.manualControlChanged(monitorID: "monitor-a", generation: 1, feature: .volume, value: 40, revision: UInt64(revision + 1))
            _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: .zero, displays: [d])
        }
        router.waitForWorkerForTesting()
        expect(fake.waitForCommands(1).count == 1, "manual rebases cannot create unbounded outstanding submissions")
        fake.acknowledge(0, accepted: false)
        let latest = fake.waitForCommands(2)
        expect(latest[1].target == 46.25, "one coalesced intent follows a superseded in-flight epoch")
        fake.acknowledge(1, accepted: true)
        router.waitForWorkerForTesting()
        router.shutdown()
    }

    private static func volumeTargetPolicy() {
        let match = display(audioUID: outputUID)
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [match])?.monitorID == "monitor-a",
               "default output equal to a monitor UID selects it")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID.lowercased(), displays: [match]) != nil,
               "UID match is case-insensitive")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: nil, displays: [match]) == nil, "nil output selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: "", displays: [display(audioUID: "")]) == nil, "empty UID never matches")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: "BuiltInSpeakerDevice", displays: [match]) == nil, "unmatched UID selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID,
                                              displays: [match, display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0), audioUID: outputUID.lowercased())]) == nil,
               "duplicate UIDs are ambiguous")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [display(audioUID: nil)]) == nil, "unstable identity selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [display(builtin: true, audioUID: outputUID)]) == nil, "built-in selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [display(volumeSupported: false, audioUID: outputUID)]) == nil, "unsupported volume selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [display(awake: false, audioUID: outputUID)]) == nil, "sleeping display selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [display(volume: nil, audioUID: outputUID)]) == nil, "unknown value selects nothing")
        expect(HardwareKeyPolicy.volumeTarget(defaultOutputUID: outputUID, displays: [display(volume: 101, audioUID: outputUID)]) == nil, "out-of-range value selects nothing")
    }

    /// Drives one full down/repeat/up sequence and reports (down, repeat, up) consumption.
    private static func hold(_ router: HardwareKeyRouter, keyType: Int = Int(NX_KEYTYPE_SOUND_UP), pointer: CGPoint,
                             displays: [HardwareKeyDisplay]) -> (Bool, Bool, Bool) {
        let down = router.processForTesting(key(keyType), pointer: pointer, displays: displays)
        let repeated = router.processForTesting(key(keyType, repeat: true), pointer: pointer, displays: displays)
        let up = router.processForTesting(key(keyType, phase: .up), pointer: pointer, displays: displays)
        return (down, repeated, up)
    }

    private static func volumeFollowsSoundOutput() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let monitor = display(audioUID: outputUID)
        router.setDefaultOutput(uid: outputUID)
        // The pointer is in a gap, far away, or over the built-in display: it is irrelevant for volume.
        let builtin = display(id: "built-in", origin: CGPoint(x: 5000, y: 5000), builtin: true)
        for pointer in [elsewhere, CGPoint(x: 1500, y: 20), CGPoint(x: 5100, y: 5100)] {
            let result = hold(router, pointer: pointer, displays: [monitor, builtin])
            expect(result == (true, true, true), "matched monitor handles the whole hold regardless of pointer")
        }
        let first = fake.waitForCommands(1)[0]
        expect(first.monitorID == "monitor-a" && first.feature == .volume && first.target > 50,
               "volume command targets the monitor selected by sound output")
        fake.acknowledge(0, accepted: true)
        router.waitForWorkerForTesting()
        router.shutdown()

        func passes(_ uid: String?, _ displays: [HardwareKeyDisplay], _ message: String) {
            let router = HardwareKeyRouter(submit: { _, _ in expect(false, "pass-through must not submit: \(message)") })
            router.setDefaultOutput(uid: uid)
            // The pointer sits over a supported monitor on purpose.
            let result = hold(router, pointer: CGPoint(x: 10, y: 10), displays: displays)
            expect(result == (false, false, false), message)
            router.waitForWorkerForTesting()
            router.shutdown()
        }
        passes("BuiltInSpeakerDevice", [monitor], "built-in output passes the whole sequence through")
        passes("AppleUSBAudioEngine:Unmatched", [monitor], "unmatched UID passes through")
        passes(nil, [monitor], "nil output passes through")
        passes(outputUID, [monitor, display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0), audioUID: outputUID)],
               "duplicate audio UIDs pass through")
        passes(outputUID, [display(volumeSupported: false, audioUID: outputUID)], "unsupported volume passes through")
        passes(outputUID, [display(awake: false, audioUID: outputUID)], "sleeping monitor passes through")
        passes(outputUID, [display(audioUID: nil)], "unstable identity passes through")
        passes(outputUID, [display(builtin: true, audioUID: outputUID)], "built-in display passes through")

        let caseFake = FakeSubmission()
        let caseRouter = HardwareKeyRouter(submit: caseFake.submit)
        caseRouter.setDefaultOutput(uid: outputUID.lowercased())
        expect(hold(caseRouter, pointer: elsewhere, displays: [display(audioUID: outputUID)]) == (true, true, true),
               "case-insensitive UID match is handled")
        _ = caseFake.waitForCommands(1)
        caseFake.acknowledge(0, accepted: true)
        caseRouter.waitForWorkerForTesting()
        caseRouter.shutdown()
    }

    private static func volumeHoldAcrossOutputChanges() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        let monitor = display(audioUID: outputUID)
        router.setDefaultOutput(uid: outputUID)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]), "hold starts on the monitor")
        _ = fake.waitForCommands(1)
        router.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: elsewhere, displays: [monitor]),
               "repeat after the output switched away is still consumed")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: elsewhere, displays: [monitor]),
               "later repeats are still consumed")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor]),
               "key-up is consumed after the output switched away")
        fake.acknowledge(0, accepted: true)
        router.waitForWorkerForTesting()
        expect(fake.commands.count == 1, "no write is submitted once the output switched away")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "next press follows the new output and passes through")
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor])

        // A passed-through key stays passed through even if the output becomes the monitor mid-hold.
        let passFake = FakeSubmission()
        let passRouter = HardwareKeyRouter(submit: passFake.submit)
        passRouter.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(!passRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [monitor]), "hold starts passed through")
        passRouter.setDefaultOutput(uid: outputUID)
        expect(!passRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: .zero, displays: [monitor]),
               "repeat stays pass-through after the output switched to the monitor")
        expect(!passRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: .zero, displays: [monitor]),
               "key-up stays pass-through after the output switched to the monitor")
        expect(passRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [monitor]),
               "next distinct press re-evaluates and targets the monitor")
        _ = passFake.waitForCommands(1)
        passFake.acknowledge(0, accepted: true)
        passRouter.waitForWorkerForTesting()
        passRouter.shutdown()
        router.shutdown()
    }

    private static func volumeBoundariesModifiersAndLatches() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        router.setDefaultOutput(uid: outputUID)

        // Boundary values are consumed without a write.
        let maxed = display(volume: 100, audioUID: outputUID)
        expect(hold(router, pointer: elsewhere, displays: [maxed]) == (true, true, true), "volume at maximum is consumed")
        let floored = display(volume: 0, audioUID: outputUID)
        expect(hold(router, keyType: Int(NX_KEYTYPE_SOUND_DOWN), pointer: elsewhere, displays: [floored]) == (true, true, true),
               "volume at minimum is consumed")
        router.waitForWorkerForTesting()
        expect(fake.commands.isEmpty, "boundary presses submit no write")

        // Disallowed modifiers pass through; Shift+Option takes the fine step.
        let monitor = display(audioUID: outputUID)
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), modifiersEligible: false), pointer: elsewhere, displays: [monitor]),
               "disallowed modifiers pass through")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up, modifiersEligible: false), pointer: elsewhere, displays: [monitor]),
               "disallowed-modifier release passes through")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_DOWN), fine: true), pointer: elsewhere, displays: [monitor]), "fine press is handled")
        expect(fake.waitForCommands(1)[0].target == 48.4375, "fine step is 1.5625")
        fake.acknowledge(0, accepted: true)
        router.waitForWorkerForTesting()
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_DOWN), phase: .up), pointer: elsewhere, displays: [monitor])

        // Brightness follows the pointer and ignores the audio output; latches are independent.
        let both = display(audioUID: outputUID)
        router.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: CGPoint(x: 10, y: 10), displays: [both]),
               "brightness follows the pointer when the output is elsewhere")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: CGPoint(x: 10, y: 10), displays: [both]),
               "volume passes through while the brightness latch is handled")
        router.setDefaultOutput(uid: outputUID)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: CGPoint(x: 10, y: 10), displays: [both]),
               "brightness latch is unaffected by the output change")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: CGPoint(x: 10, y: 10), displays: [both]),
               "volume pass-through latch is unaffected by the output change")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: CGPoint(x: 10, y: 10), displays: [both]),
               "brightness release is consumed")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: CGPoint(x: 10, y: 10), displays: [both]),
               "volume release passes through")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: elsewhere, displays: [both]),
               "brightness outside every display passes through even when the monitor is the output")
        _ = router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: elsewhere, displays: [both])
        _ = fake.waitForCommands(2)
        fake.acknowledge(1, accepted: true)
        router.waitForWorkerForTesting()
        router.shutdown()
    }

    private static func muteBehavior() {
        let loud = display(audioUID: outputUID)
        let silent = display(volume: 0, audioUID: outputUID)

        // a. Mute writes zero once; the held repeat and the key-up add no command.
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        router.setDefaultOutput(uid: outputUID)
        expect(router.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "mute press is handled")
        let muted = fake.waitForCommands(1)[0]
        expect(muted.feature == .volume && muted.target == 0, "mute writes volume zero")
        expect(router.processForTesting(muteKey(repeat: true), pointer: elsewhere, displays: [loud]), "held mute repeat is consumed")
        expect(router.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "mute key-up is consumed")
        fake.acknowledge(0, accepted: true)
        router.waitForWorkerForTesting()
        expect(fake.commands.count == 1, "held mute submits no second command")
        router.shutdown()

        // b. A mute press at zero restores the remembered level.
        let restoreFake = FakeSubmission()
        let restoreRouter = HardwareKeyRouter(submit: restoreFake.submit)
        restoreRouter.setDefaultOutput(uid: outputUID)
        expect(restoreRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "first mute is handled")
        _ = restoreFake.waitForCommands(1)
        expect(restoreRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "first mute release is consumed")
        restoreFake.acknowledge(0, accepted: true)
        restoreRouter.waitForWorkerForTesting()
        expect(restoreRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [silent]), "restoring mute press is handled")
        expect(restoreRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [silent]), "restoring mute release is consumed")
        let restored = restoreFake.waitForCommands(2)[1]
        expect(restored.target == 50, "mute restores the remembered level")
        restoreFake.acknowledge(1, accepted: true)
        restoreRouter.waitForWorkerForTesting()
        restoreRouter.shutdown()

        // c. Volume-up while muted steps from the remembered level; volume-down while muted writes nothing.
        let upFake = FakeSubmission()
        let upRouter = HardwareKeyRouter(submit: upFake.submit)
        upRouter.setDefaultOutput(uid: outputUID)
        expect(upRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "mute before volume-up is handled")
        _ = upFake.waitForCommands(1)
        expect(upRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "mute release before volume-up is consumed")
        upFake.acknowledge(0, accepted: true)
        upRouter.waitForWorkerForTesting()
        expect(upRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [silent]), "volume-up while muted is handled")
        expect(upRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [silent]),
               "volume-up release while muted is consumed")
        let unmuted = upFake.waitForCommands(2)[1]
        expect(unmuted.feature == .volume && unmuted.target == 50 + HardwareKeyPolicy.normalStep,
               "volume-up while muted steps from the remembered level")
        upFake.acknowledge(1, accepted: true)
        upRouter.waitForWorkerForTesting()
        upRouter.shutdown()

        let downFake = FakeSubmission()
        let downRouter = HardwareKeyRouter(submit: downFake.submit)
        downRouter.setDefaultOutput(uid: outputUID)
        expect(downRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "mute before volume-down is handled")
        _ = downFake.waitForCommands(1)
        expect(downRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "mute release before volume-down is consumed")
        downFake.acknowledge(0, accepted: true)
        downRouter.waitForWorkerForTesting()
        expect(downRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_DOWN)), pointer: elsewhere, displays: [silent]), "volume-down while muted is consumed")
        expect(downRouter.processForTesting(key(Int(NX_KEYTYPE_SOUND_DOWN), phase: .up), pointer: elsewhere, displays: [silent]),
               "volume-down release while muted is consumed")
        downRouter.waitForWorkerForTesting()
        expect(downFake.commands.count == 1, "volume-down while muted writes nothing")
        downRouter.shutdown()

        // d. Mute at zero with nothing remembered is consumed without a write.
        let emptyFake = FakeSubmission()
        let emptyRouter = HardwareKeyRouter(submit: emptyFake.submit)
        emptyRouter.setDefaultOutput(uid: outputUID)
        expect(emptyRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [silent]), "mute at zero with nothing remembered is consumed")
        expect(emptyRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [silent]), "mute at zero release is consumed")
        emptyRouter.waitForWorkerForTesting()
        expect(emptyFake.commands.isEmpty, "mute at zero submits no write")
        emptyRouter.shutdown()

        // e. An external level change discards the remembered level, so the next mute writes zero again.
        let externalFake = FakeSubmission()
        let externalRouter = HardwareKeyRouter(submit: externalFake.submit)
        externalRouter.setDefaultOutput(uid: outputUID)
        expect(externalRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "first mute before external change is handled")
        _ = externalFake.waitForCommands(1)
        expect(externalRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "first mute release is consumed")
        externalFake.acknowledge(0, accepted: true)
        externalRouter.waitForWorkerForTesting()
        let changed = display(volume: 30, audioUID: outputUID)
        expect(externalRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [changed]), "mute after external change is handled")
        expect(externalRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [changed]), "mute release after external change is consumed")
        let rewritten = externalFake.waitForCommands(2)[1]
        expect(rewritten.target == 0, "external change discards the remembered level")
        externalFake.acknowledge(1, accepted: true)
        externalRouter.waitForWorkerForTesting()
        externalRouter.shutdown()

        // f. Switching the output away and back forgets the remembered level.
        let roundTripFake = FakeSubmission()
        let roundTripRouter = HardwareKeyRouter(submit: roundTripFake.submit)
        roundTripRouter.setDefaultOutput(uid: outputUID)
        expect(roundTripRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "mute before output round trip is handled")
        _ = roundTripFake.waitForCommands(1)
        expect(roundTripRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "mute release before output round trip is consumed")
        roundTripFake.acknowledge(0, accepted: true)
        roundTripRouter.waitForWorkerForTesting()
        roundTripRouter.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        roundTripRouter.setDefaultOutput(uid: outputUID)
        expect(roundTripRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [silent]), "mute after output round trip is consumed")
        expect(roundTripRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [silent]), "mute release after output round trip is consumed")
        roundTripRouter.waitForWorkerForTesting()
        expect(roundTripFake.commands.count == 1, "output change forgets the remembered level")
        roundTripRouter.shutdown()

        // g. Mute passes through the whole sequence when routing does not select a supported monitor.
        func mutePasses(_ uid: String?, _ displays: [HardwareKeyDisplay], _ message: String) {
            let passRouter = HardwareKeyRouter(submit: { _, _ in expect(false, "pass-through mute must not submit") })
            passRouter.setDefaultOutput(uid: uid)
            let down = passRouter.processForTesting(muteKey(), pointer: .zero, displays: displays)
            let repeated = passRouter.processForTesting(muteKey(repeat: true), pointer: .zero, displays: displays)
            let up = passRouter.processForTesting(muteKey(phase: .up), pointer: .zero, displays: displays)
            expect((down, repeated, up) == (false, false, false), message)
            passRouter.waitForWorkerForTesting()
            passRouter.shutdown()
        }
        mutePasses("BuiltInSpeakerDevice", [loud], "built-in output mute passes through")
        mutePasses(nil, [loud], "nil output mute passes through")
        mutePasses(outputUID, [display(volumeSupported: false, audioUID: outputUID)], "unsupported volume mute passes through")
        mutePasses(outputUID, [display(awake: false, audioUID: outputUID)], "sleeping monitor mute passes through")
        mutePasses(outputUID, [display(audioUID: nil)], "unstable identity mute passes through")

        // h. An output switch cancels a held mute: its repeat and release are consumed, and nothing more is written.
        let cancelFake = FakeSubmission()
        let cancelRouter = HardwareKeyRouter(submit: cancelFake.submit)
        cancelRouter.setDefaultOutput(uid: outputUID)
        expect(cancelRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "held mute starts on the monitor")
        _ = cancelFake.waitForCommands(1)
        cancelRouter.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(cancelRouter.processForTesting(muteKey(repeat: true), pointer: elsewhere, displays: [loud]), "held mute repeat after output switch is consumed")
        expect(cancelRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "held mute release after output switch is consumed")
        cancelFake.acknowledge(0, accepted: true)
        cancelRouter.waitForWorkerForTesting()
        expect(cancelFake.commands.count == 1, "output switch cancels the held mute")
        cancelRouter.shutdown()

        // i. Brightness ignores the mute memory on the same monitor.
        let brightnessFake = FakeSubmission()
        let brightnessRouter = HardwareKeyRouter(submit: brightnessFake.submit)
        brightnessRouter.setDefaultOutput(uid: outputUID)
        expect(brightnessRouter.processForTesting(muteKey(), pointer: elsewhere, displays: [loud]), "mute before brightness is handled")
        _ = brightnessFake.waitForCommands(1)
        expect(brightnessRouter.processForTesting(muteKey(phase: .up), pointer: elsewhere, displays: [loud]), "mute release before brightness is consumed")
        brightnessFake.acknowledge(0, accepted: true)
        brightnessRouter.waitForWorkerForTesting()
        expect(brightnessRouter.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: .zero, displays: [loud]),
               "brightness press after a mute is handled")
        let brightness = brightnessFake.waitForCommands(2)[1]
        expect(brightness.feature == .brightness, "brightness command ignores the mute memory")
        brightnessFake.acknowledge(1, accepted: true)
        brightnessRouter.waitForWorkerForTesting()
        brightnessRouter.shutdown()

        // j. A mute with disallowed modifiers passes through, on press and release.
        let ineligibleFake = FakeSubmission()
        let ineligibleRouter = HardwareKeyRouter(submit: ineligibleFake.submit)
        ineligibleRouter.setDefaultOutput(uid: outputUID)
        let ineligibleDown = HardwareKeyEvent(keyIdentity: Int(NX_KEYTYPE_MUTE), feature: .volume, direction: .decrease,
                                              phase: .down, isRepeat: false, isFineStep: false,
                                              modifiersEligible: false, isMute: true)
        let ineligibleUp = HardwareKeyEvent(keyIdentity: Int(NX_KEYTYPE_MUTE), feature: .volume, direction: .decrease,
                                            phase: .up, isRepeat: false, isFineStep: false,
                                            modifiersEligible: false, isMute: true)
        expect(!ineligibleRouter.processForTesting(ineligibleDown, pointer: elsewhere, displays: [loud]), "mute with disallowed modifiers passes through")
        expect(!ineligibleRouter.processForTesting(ineligibleUp, pointer: elsewhere, displays: [loud]), "mute release with disallowed modifiers passes through")
        ineligibleRouter.waitForWorkerForTesting()
        expect(ineligibleFake.commands.isEmpty, "ineligible mute submits no write")
        ineligibleRouter.shutdown()
    }

    private static func volumeStaleGenerationPassesThrough() {
        let fake = FakeSubmission()
        let router = HardwareKeyRouter(submit: fake.submit)
        router.setDefaultOutput(uid: outputUID)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [display(audioUID: outputUID)]),
               "volume hold starts on generation one")
        _ = fake.waitForCommands(1)
        let replaced = display(generation: 2, audioUID: outputUID)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: elsewhere, displays: [replaced]),
               "stale volume repeat remains consumed")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [replaced]),
               "stale volume hold release remains consumed")
        fake.acknowledge(0, accepted: true)
        router.waitForWorkerForTesting()
        expect(fake.commands.count == 1, "stale generation cancels future volume deltas")
        router.shutdown()
    }

    private static func disableAndShutdownClearOutput() {
        let monitor = display(audioUID: outputUID)
        let router = HardwareKeyRouter(submit: { _, _ in })
        router.setDefaultOutput(uid: outputUID)
        router.apply(enabled: false, displays: [monitor])
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "disabling routing clears the default output")
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor])
        router.shutdown()
        router.setDefaultOutput(uid: outputUID)
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "a stopped router ignores the default output")
    }

    private static func makeRouter(_ permissions: FakePermissions, _ installer: FakeTapInstaller) -> HardwareKeyRouter {
        HardwareKeyRouter(submit: { _, _ in }, permissionProvider: { permissions.value }, tapInstaller: installer.install)
    }

    private static func permissionLossKeepsOutput() {
        let monitor = display(audioUID: outputUID)
        let permissions = FakePermissions(false, true)
        let router = makeRouter(permissions, FakeTapInstaller())
        router.setDefaultOutput(uid: outputUID)
        router.apply(enabled: true, displays: [monitor])
        expect(router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "permission loss does not clear the default output")
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor])
        router.apply(enabled: false, displays: [monitor])
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "disable still clears the default output after permission loss")
        _ = router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor])

        let other = makeRouter(permissions, FakeTapInstaller())
        other.setDefaultOutput(uid: outputUID)
        other.apply(enabled: true, displays: [monitor])
        other.shutdown()
        expect(!other.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "shutdown still clears the default output")
    }

    private static func statusPolicyMatrix() {
        typealias Policy = HardwareKeyStatusPolicy
        for ax in [false, true] {
            for im in [false, true] {
                for installed in [false, true] {
                    for failed in [false, true] {
                        for hadTap in [false, true] {
                            let p = HardwareKeyPermissions(accessibility: ax, inputMonitoring: im)
                            expect(Policy.evaluate(enabled: false, permissions: p, tapInstalled: installed,
                                                   tapCreateFailed: failed, hadActiveTap: hadTap) == .disabled, "disabled wins")
                            let status = Policy.evaluate(enabled: true, permissions: p, tapInstalled: installed,
                                                         tapCreateFailed: failed, hadActiveTap: hadTap)
                            if !ax {
                                expect(status == (hadTap ? .permissionRevoked : .permissionRequired), "missing Accessibility")
                            } else if !im {
                                expect(status == .inputMonitoringRequired, "missing Input Monitoring, tap installed \(installed)")
                            } else if failed {
                                expect(status == .listenerUnavailable, "failed creation with both grants")
                            } else {
                                expect(status == (installed ? .enabled : .listenerUnavailable), "both grants, tap installed \(installed)")
                            }
                        }
                    }
                }
            }
        }
    }

    private static func permissionChangeNotifications() {
        let permissions = FakePermissions(false, false)
        let router = makeRouter(permissions, FakeTapInstaller())
        var received: [HardwareKeyPermissions] = []
        router.onPermissionsChange = { received.append($0); expect(Thread.isMainThread, "callback runs on the main thread") }
        router.apply(enabled: true, displays: [])
        drain()
        expect(received == [permissions.value], "first evaluation publishes once")
        router.apply(enabled: true, displays: [])
        router.refreshPermissions()
        drain()
        expect(received.count == 1, "unchanged permissions are not republished")
        permissions.set(true, false)
        router.refreshPermissions()
        drain()
        expect(received.count == 2 && received[1] == HardwareKeyPermissions(accessibility: true, inputMonitoring: false),
               "a change is published")
        router.shutdown()
    }

    private static func tapRetryAfterFailure() {
        let permissions = FakePermissions(true, true)
        let installer = FakeTapInstaller()
        let router = makeRouter(permissions, installer)
        router.apply(enabled: true, displays: [])
        drain()
        expect(installer.calls == 1 && router.status == .listenerUnavailable, "failed creation reports the listener unavailable")
        router.apply(enabled: true, displays: [])
        router.apply(enabled: true, displays: [])
        drain()
        expect(installer.calls == 1, "a failed creation with unchanged permissions is not retried by snapshots")
        installer.allow = true
        router.refreshPermissions()
        drain()
        expect(installer.calls == 2 && router.status == .enabled, "refresh retries and succeeds")
        router.apply(enabled: true, displays: [])
        expect(installer.calls == 2, "an installed tap is kept")

        // A failure is retried when the grant changes, then stays quiet.
        let flaky = FakeTapInstaller()
        let flakyPermissions = FakePermissions(true, false)
        let flakyRouter = makeRouter(flakyPermissions, flaky)
        flakyRouter.apply(enabled: true, displays: [])
        drain()
        expect(flaky.calls == 1 && flakyRouter.status == .inputMonitoringRequired, "refused tap without Input Monitoring")
        flakyRouter.apply(enabled: true, displays: [])
        expect(flaky.calls == 1, "no retry while permissions are unchanged")
        flaky.allow = true
        flakyPermissions.set(true, true)
        flakyRouter.apply(enabled: true, displays: [])
        drain()
        expect(flaky.calls == 2 && flakyRouter.status == .enabled, "permission change retries the failed tap")

        // Re-enabling after a failure starts from a clean slate.
        let reenable = FakeTapInstaller()
        let reenablePermissions = FakePermissions(true, true)
        let reenableRouter = makeRouter(reenablePermissions, reenable)
        reenableRouter.apply(enabled: true, displays: [])
        reenableRouter.apply(enabled: false, displays: [])
        drain()
        expect(reenableRouter.status == .disabled, "disable reports disabled")
        reenable.allow = true
        reenableRouter.apply(enabled: true, displays: [])
        drain()
        expect(reenable.calls == 2 && reenableRouter.status == .enabled, "re-enable retries and recovers from disabled")

        // refreshPermissions hops to the main thread from any thread.
        let background = FakeTapInstaller()
        let backgroundRouter = makeRouter(FakePermissions(true, true), background)
        backgroundRouter.apply(enabled: true, displays: [])
        background.allow = true
        let done = DispatchSemaphore(value: 0)
        DispatchQueue.global().async { backgroundRouter.refreshPermissions(); done.signal() }
        expect(done.wait(timeout: .now() + 3) == .success, "background refresh returns")
        drain()
        expect(background.calls == 2 && backgroundRouter.status == .enabled, "background refresh retries on the main thread")
        for r in [router, flakyRouter, reenableRouter, backgroundRouter] { r.shutdown() }
        drain()
    }

    private static func inputMonitoringAndRevocationStatus() {
        let permissions = FakePermissions(true, false)
        let installer = FakeTapInstaller()
        installer.allow = true
        let router = makeRouter(permissions, installer)
        router.apply(enabled: true, displays: [])
        drain()
        expect(installer.calls == 1 && router.status == .inputMonitoringRequired,
               "an installed tap still reports missing Input Monitoring")
        permissions.set(true, true)
        router.refreshPermissions()
        drain()
        expect(installer.calls == 2 && router.status == .enabled, "granting Input Monitoring recreates the tap and updates status")
        router.refreshPermissions()
        drain()
        expect(installer.calls == 2 && router.status == .enabled, "a refresh with unchanged permissions does not reinstall the tap")
        permissions.set(false, true)
        router.refreshPermissions()
        drain()
        expect(router.status == .permissionRevoked, "losing Accessibility after an active tap is a revocation")
        router.refreshPermissions()
        drain()
        expect(router.status == .permissionRevoked, "revocation is stable across refreshes")
        permissions.set(true, true)
        router.refreshPermissions()
        drain()
        expect(installer.calls == 3 && router.status == .enabled, "regaining Accessibility reinstalls the tap")
        router.shutdown()

        let fresh = makeRouter(FakePermissions(false, false), FakeTapInstaller())
        fresh.apply(enabled: true, displays: [])
        drain()
        expect(fresh.status == .permissionRequired, "never-granted Accessibility is permission required")
        fresh.shutdown()
    }
}
