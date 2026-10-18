import AppKit
import Foundation
import IOKit.hidsystem

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("HardwareKeyRouterTests: \(message)") }
}

private func display(id: String? = "monitor-a", generation: UInt64? = 1, origin: CGPoint = .zero,
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

private func muteKey(phase: HardwareKeyEvent.Phase = .down, repeat isRepeat: Bool = false,
                     modifiersEligible: Bool = true) -> HardwareKeyEvent {
    HardwareKeyEvent(keyIdentity: Int(NX_KEYTYPE_MUTE), feature: .volume, direction: .decrease, phase: phase,
                     isRepeat: isRepeat, isFineStep: false, modifiersEligible: modifiersEligible, isMute: true)
}

private let outputUID = "1E6D8877-0000-0000-0722-0104B5462878"
private let elsewhere = CGPoint(x: 5000, y: 5000)
private let onMonitor = CGPoint(x: 10, y: 10)
private let onOther = CGPoint(x: 2010, y: 10)
private let monitorA = HardwareKeyOwner(monitorID: "monitor-a", generation: 1)
private let monitorB = HardwareKeyOwner(monitorID: "monitor-b", generation: 1)

/// One key as the fake core received it.
private struct ControlsCall: Equatable {
    let key: String
    let down: Bool
    let isRepeat: Bool
    let fine: Bool
    let owner: HardwareKeyOwner?
}

/// Records every key the router sends to the core and answers each one with `answer`.
private final class FakeControls: HardwareKeyControlling {
    var answer = true
    private(set) var calls: [ControlsCall] = []

    func key(_ key: String, down: Bool, isRepeat: Bool, fine: Bool, owner: HardwareKeyOwner?) -> Bool {
        calls.append(ControlsCall(key: key, down: down, isRepeat: isRepeat, fine: fine, owner: owner))
        return answer
    }
}

/// Resolves one event through the pure resolver. The defaults are a pointer on the first monitor and the matched output.
private func resolve(_ resolver: inout HardwareKeyOwnerResolver, _ event: HardwareKeyEvent, displays: [HardwareKeyDisplay],
                     pointer: CGPoint = onMonitor, enabled: Bool = true, output: String? = outputUID,
                     followed: Set<String> = []) -> HardwareKeyOwner? {
    resolver.owner(for: event, pointer: pointer, enabled: enabled, defaultOutputUID: output,
                   displays: displays, followedBrightness: followed)
}

/// Routes one event through the router and returns the owner the fake core received with it.
private func routed(_ router: HardwareKeyRouter, _ controls: FakeControls, _ event: HardwareKeyEvent,
                    pointer: CGPoint = elsewhere, displays: [HardwareKeyDisplay]) -> HardwareKeyOwner? {
    let before = controls.calls.count
    _ = router.processForTesting(event, pointer: pointer, displays: displays)
    expect(controls.calls.count == before + 1, "the key reaches the core exactly once")
    return controls.calls.last?.owner
}

/// Routes a full down, repeat and up sequence and returns the owner the core received with each phase.
private func holdOwners(_ router: HardwareKeyRouter, _ controls: FakeControls, keyType: Int = Int(NX_KEYTYPE_SOUND_UP),
                        pointer: CGPoint, displays: [HardwareKeyDisplay]) -> [HardwareKeyOwner?] {
    let isMute = keyType == Int(NX_KEYTYPE_MUTE)
    let down = isMute ? muteKey() : key(keyType)
    let repeated = isMute ? muteKey(repeat: true) : key(keyType, repeat: true)
    let up = isMute ? muteKey(phase: .up) : key(keyType, phase: .up)
    return [routed(router, controls, down, pointer: pointer, displays: displays),
            routed(router, controls, repeated, pointer: pointer, displays: displays),
            routed(router, controls, up, pointer: pointer, displays: displays)]
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
        keyboardKeysReachTheCore()
        controlsKeyNames()
        targetPolicy()
        volumeTargetPolicy()
        firstPressResolvesOwner()
        ineligibleFirstPressesResolveNoOwner()
        heldOwnerSurvivesPointerMovement()
        heldOwnerDropsWhenInvalid()
        orphanRepeatsResolveNoOwner()
        releaseSendsHeldOwnerAndClearsIt()
        ownerRememberedOnlyWhenConsumed()
        volumeFollowsSoundOutput()
        volumeHoldAcrossOutputChanges()
        staleGenerationReleaseKeepsHeldOwner()
        modifiersAndFineStep()
        brightnessAndVolumeLatchesAreIndependent()
        muteOwnership()
        disableAndShutdownClearOutput()
        permissionLossKeepsOutput()
        routerForwardsToCore()
        volumeOwnerMonitorFollowsOutput()
        statusPolicyMatrix()
        permissionChangeNotifications()
        tapRetryAfterFailure()
        inputMonitoringAndRevocationStatus()
        lateSnapshotsCannotRestartStoppedRouter()
        followedBrightnessPassesThrough()
        followedVolumeAndMuteUnchanged()
        unfollowedBrightnessStillHandled()
        emptyFollowedSetRestoresBrightness()
        followedChangeKeepsInFlightPress()
        print("Hardware-key router tests passed")
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
    }

    /// Keyboard brightness reaches the core with its key name, and its owner follows the pointer like the media key.
    private static func keyboardKeysReachTheCore() {
        let fn: CGEventFlags = [.maskSecondaryFn]
        func keyboard(_ type: CGEventType, repeat isRepeat: Bool = false) -> HardwareKeyEvent {
            guard let event = HardwareKeyEvent.decodeKeyboard(type: type, keyCode: 144, isRepeat: isRepeat, flags: fn) else {
                fatalError("HardwareKeyRouterTests: keyboard brightness key did not decode")
            }
            return event
        }
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        let monitor = display()
        expect(routed(router, controls, keyboard(.keyDown), pointer: onMonitor, displays: [monitor]) == monitorA,
               "keyboard press resolves the brightness of the monitor under the pointer")
        expect(controls.calls.last == ControlsCall(key: "brightness_up", down: true, isRepeat: false, fine: false, owner: monitorA),
               "keyboard press is forwarded as brightness_up")
        expect(routed(router, controls, keyboard(.keyDown, repeat: true), pointer: elsewhere, displays: [monitor]) == monitorA,
               "keyboard autorepeat keeps the owner")
        expect(routed(router, controls, keyboard(.keyUp), pointer: elsewhere, displays: [monitor]) == monitorA,
               "keyboard release sends the owner")
        expect(routed(router, controls, keyboard(.keyDown), pointer: elsewhere, displays: [monitor]) == nil,
               "keyboard press outside every display resolves no owner")
        router.shutdown()
    }

    /// The core receives the five key names, and mute is named "mute" whatever its feature.
    private static func controlsKeyNames() {
        expect(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)).controlsKey == "brightness_up", "brightness up is brightness_up")
        expect(key(Int(NX_KEYTYPE_BRIGHTNESS_DOWN)).controlsKey == "brightness_down", "brightness down is brightness_down")
        expect(key(Int(NX_KEYTYPE_SOUND_UP)).controlsKey == "volume_up", "sound up is volume_up")
        expect(key(Int(NX_KEYTYPE_SOUND_DOWN)).controlsKey == "volume_down", "sound down is volume_down")
        expect(muteKey().controlsKey == "mute", "mute press is mute")
        expect(muteKey(phase: .up).controlsKey == "mute", "mute release is mute")
        let keyboardDown = HardwareKeyEvent.decodeKeyboard(type: .keyDown, keyCode: 145, isRepeat: false, flags: [])
        expect(keyboardDown?.controlsKey == "brightness_down", "keyboard key code 145 is brightness_down")
        let keyboardUp = HardwareKeyEvent.decodeKeyboard(type: .keyUp, keyCode: 144, isRepeat: false, flags: [])
        expect(keyboardUp?.controlsKey == "brightness_up", "keyboard key code 144 release is brightness_up")
    }

    private static func targetPolicy() {
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

    /// A first press resolves the pointer's monitor for brightness and the default output's monitor for volume and mute.
    private static func firstPressResolvesOwner() {
        var resolver = HardwareKeyOwnerResolver()
        let monitor = display(audioUID: outputUID)
        let other = display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0), audioUID: "OtherSpeakerDevice")
        let both = [monitor, other]
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: both, pointer: onMonitor) == monitorA,
               "brightness press resolves the monitor under the pointer")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_DOWN)), displays: both, pointer: onOther) == monitorB,
               "brightness down resolves the other monitor under its pointer")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: both, pointer: elsewhere) == nil,
               "brightness press outside every display resolves nothing")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: both, pointer: elsewhere) == monitorA,
               "volume press resolves the default output's monitor, whatever the pointer")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_DOWN)), displays: both, pointer: onOther) == monitorA,
               "volume down ignores the pointer as well")
        expect(resolve(&resolver, muteKey(), displays: both, pointer: elsewhere) == monitorA,
               "mute resolves the default output's monitor")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: both, pointer: elsewhere, output: "OtherSpeakerDevice") == monitorB,
               "volume follows the sound output to the other monitor")
    }

    /// Modifiers, disabled routing, built-in, ambiguity, missing output and unusable state each resolve no owner.
    private static func ineligibleFirstPressesResolveNoOwner() {
        var resolver = HardwareKeyOwnerResolver()
        let monitor = display(audioUID: outputUID)
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), modifiersEligible: false), displays: [monitor]) == nil,
               "disallowed modifiers resolve no brightness owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP), modifiersEligible: false), displays: [monitor]) == nil,
               "disallowed modifiers resolve no volume owner")
        expect(resolve(&resolver, muteKey(modifiersEligible: false), displays: [monitor]) == nil,
               "disallowed modifiers resolve no mute owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [monitor], enabled: false) == nil,
               "disabled routing resolves no volume owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [monitor], enabled: false) == nil,
               "disabled routing resolves no brightness owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(builtin: true)]) == nil,
               "built-in display resolves no brightness owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [display(builtin: true, audioUID: outputUID)]) == nil,
               "built-in display resolves no volume owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(), display(id: "monitor-b")]) == nil,
               "overlapping displays resolve no brightness owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [display(id: "monitor-b", audioUID: outputUID), monitor]) == nil,
               "duplicate output UIDs resolve no volume owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [monitor], output: nil) == nil,
               "no default output resolves no volume owner")
        expect(resolve(&resolver, muteKey(), displays: [monitor], output: nil) == nil,
               "no default output resolves no mute owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [monitor], output: "BuiltInSpeakerDevice") == nil,
               "an unmatched output resolves no volume owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(brightnessSupported: false)]) == nil,
               "unsupported brightness resolves no owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(awake: false)]) == nil,
               "sleeping display resolves no brightness owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(brightness: nil)]) == nil,
               "unknown brightness resolves no owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(brightness: 101)]) == nil,
               "out-of-range brightness resolves no owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(id: nil)]) == nil,
               "a display without a monitor ID resolves no owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [display(generation: nil)]) == nil,
               "a display without a generation resolves no owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [display(volumeSupported: false, audioUID: outputUID)]) == nil,
               "unsupported volume resolves no owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [display(awake: false, audioUID: outputUID)]) == nil,
               "sleeping display resolves no volume owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [display(audioUID: nil)]) == nil,
               "unstable identity resolves no volume owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [monitor], pointer: elsewhere) == nil,
               "a pointer in a gap resolves no brightness owner")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), displays: [monitor], followed: ["monitor-a"]) == nil,
               "a followed monitor resolves no brightness owner")
    }

    /// Brightness repeats keep their latched owner however the pointer moves; volume repeats ignore the pointer too.
    private static func heldOwnerSurvivesPointerMovement() {
        let monitor = display(audioUID: outputUID)
        let other = display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0))
        var resolver = HardwareKeyOwnerResolver()
        let press = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        let pressOwner = resolve(&resolver, press, displays: [monitor, other], pointer: onMonitor)
        expect(pressOwner == monitorA, "press latches the monitor under the pointer")
        resolver.record(press, owner: pressOwner, consumed: true)
        let repeated = key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true)
        expect(resolve(&resolver, repeated, displays: [monitor, other], pointer: onOther) == monitorA,
               "repeat keeps the latched owner when the pointer moves to another monitor")
        expect(resolve(&resolver, repeated, displays: [monitor, other], pointer: elsewhere) == monitorA,
               "repeat keeps the latched owner outside every display")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), displays: [monitor, other], pointer: elsewhere) == monitorA,
               "release sends the latched owner")

        var volume = HardwareKeyOwnerResolver()
        let volumeDown = key(Int(NX_KEYTYPE_SOUND_UP))
        let volumeOwner = resolve(&volume, volumeDown, displays: [monitor], pointer: elsewhere)
        expect(volumeOwner == monitorA, "volume press latches the default output's monitor")
        volume.record(volumeDown, owner: volumeOwner, consumed: true)
        expect(resolve(&volume, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), displays: [monitor], pointer: onOther) == monitorA,
               "volume repeat ignores the pointer")
    }

    /// A repeat drops to no owner when its display is gone, its generation changed, it sleeps, or (volume) the output changed.
    private static func heldOwnerDropsWhenInvalid() {
        let monitor = display(audioUID: outputUID)
        let brightnessDown = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        let brightnessRepeat = key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true)
        let volumeDown = key(Int(NX_KEYTYPE_SOUND_UP))
        let volumeRepeat = key(Int(NX_KEYTYPE_SOUND_UP), repeat: true)

        /// A resolver that has latched the owner of `press` on `monitor`.
        func held(_ press: HardwareKeyEvent) -> HardwareKeyOwnerResolver {
            var resolver = HardwareKeyOwnerResolver()
            let owner = resolve(&resolver, press, displays: [monitor])
            resolver.record(press, owner: owner, consumed: true)
            return resolver
        }
        /// The owner a repeat resolves to after `press`, with the given displays and output.
        func repeatOwner(_ repeated: HardwareKeyEvent, after press: HardwareKeyEvent, displays: [HardwareKeyDisplay],
                         output: String? = outputUID) -> HardwareKeyOwner? {
            var resolver = held(press)
            return resolve(&resolver, repeated, displays: displays, pointer: elsewhere, output: output)
        }

        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(brightness: 70, audioUID: outputUID)]) == monitorA,
               "brightness repeat keeps the owner when the value changes")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [monitor], output: "BuiltInSpeakerDevice") == monitorA,
               "brightness repeat ignores the sound output")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(id: "monitor-b")]) == nil,
               "brightness repeat drops when its monitor is gone")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: []) == nil,
               "brightness repeat with no displays resolves nothing")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(generation: 2)]) == nil,
               "brightness repeat drops when the generation changed")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(awake: false)]) == nil,
               "brightness repeat drops when the monitor sleeps")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(brightnessSupported: false)]) == nil,
               "brightness repeat drops when brightness becomes unsupported")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(brightness: nil)]) == nil,
               "brightness repeat drops when the value is unknown")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(brightness: Double.nan)]) == nil,
               "brightness repeat drops when the value is not finite")
        expect(repeatOwner(brightnessRepeat, after: brightnessDown, displays: [display(builtin: true)]) == nil,
               "brightness repeat drops when the display becomes built-in")

        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [monitor], output: outputUID) == monitorA,
               "volume repeat keeps the owner while the output matches")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [monitor], output: "BuiltInSpeakerDevice") == nil,
               "volume repeat drops when the default output changes")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [monitor], output: nil) == nil,
               "volume repeat drops when there is no default output")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [display(generation: 2, audioUID: outputUID)]) == nil,
               "volume repeat drops when the generation changed")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: []) == nil,
               "volume repeat drops when the monitor is gone")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [display(awake: false, audioUID: outputUID)]) == nil,
               "volume repeat drops when the monitor sleeps")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [display(volumeSupported: false, audioUID: outputUID)]) == nil,
               "volume repeat drops when volume becomes unsupported")
        expect(repeatOwner(volumeRepeat, after: volumeDown, displays: [display(volume: nil, audioUID: outputUID)]) == nil,
               "volume repeat drops when the value is unknown")
    }

    /// A repeat with no press (for example after a release, or a press that was never latched) resolves nothing.
    private static func orphanRepeatsResolveNoOwner() {
        let monitor = display(audioUID: outputUID)
        var resolver = HardwareKeyOwnerResolver()
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), displays: [monitor]) == nil,
               "a brightness repeat without its press resolves nothing")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), displays: [monitor]) == nil,
               "a volume repeat without its press resolves nothing")
        let press = key(Int(NX_KEYTYPE_SOUND_UP))
        let owner = resolve(&resolver, press, displays: [monitor])
        resolver.record(press, owner: owner, consumed: true)
        let release = key(Int(NX_KEYTYPE_SOUND_UP), phase: .up)
        let released = resolve(&resolver, release, displays: [monitor])
        resolver.record(release, owner: released, consumed: true)
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), displays: [monitor]) == nil,
               "a repeat after the release resolves nothing")
    }

    /// A release sends the latched owner even when its display has gone, and then the latch is cleared.
    private static func releaseSendsHeldOwnerAndClearsIt() {
        let monitor = display(audioUID: outputUID)
        var resolver = HardwareKeyOwnerResolver()
        let press = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        let pressOwner = resolve(&resolver, press, displays: [monitor], pointer: onMonitor)
        resolver.record(press, owner: pressOwner, consumed: true)
        let release = key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up)
        let released = resolve(&resolver, release, displays: [], pointer: elsewhere)
        expect(released == monitorA, "release sends the held owner even when its display is gone")
        resolver.record(release, owner: released, consumed: true)
        expect(resolve(&resolver, release, displays: [monitor]) == nil, "a second release finds nothing held")
        expect(resolve(&resolver, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), displays: [monitor]) == nil,
               "a release without a press resolves nothing")
    }

    /// Only a press the core consumed latches its owner; a declined press or repeat latches nothing.
    private static func ownerRememberedOnlyWhenConsumed() {
        let monitor = display(audioUID: outputUID)
        let press = key(Int(NX_KEYTYPE_BRIGHTNESS_UP))
        let repeated = key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true)
        let release = key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up)

        var declined = HardwareKeyOwnerResolver()
        let declinedOwner = resolve(&declined, press, displays: [monitor], pointer: onMonitor)
        expect(declinedOwner == monitorA, "the declined press still resolves its owner")
        declined.record(press, owner: declinedOwner, consumed: false)
        expect(resolve(&declined, repeated, displays: [monitor]) == nil, "an unconsumed press is not latched")
        expect(resolve(&declined, release, displays: [monitor]) == nil, "an unconsumed press releases nothing")

        var noOwner = HardwareKeyOwnerResolver()
        let noOwnerResult = resolve(&noOwner, press, displays: [], pointer: elsewhere)
        noOwner.record(press, owner: noOwnerResult, consumed: true)
        expect(resolve(&noOwner, repeated, displays: [monitor]) == nil, "a consumed press without an owner latches nothing")

        var dropped = HardwareKeyOwnerResolver()
        let droppedOwner = resolve(&dropped, press, displays: [monitor], pointer: onMonitor)
        dropped.record(press, owner: droppedOwner, consumed: true)
        let declinedRepeat = resolve(&dropped, repeated, displays: [monitor])
        expect(declinedRepeat == monitorA, "the repeat resolves the latched owner")
        dropped.record(repeated, owner: declinedRepeat, consumed: false)
        expect(resolve(&dropped, release, displays: [monitor]) == nil, "a declined repeat unlatches the key")
    }

    /// Volume follows the default output whatever the pointer does, and a changed output drops the held owner's repeats.
    private static func volumeFollowsSoundOutput() {
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        let monitor = display(audioUID: outputUID)
        router.setDefaultOutput(uid: outputUID)
        // The pointer is in a gap, far away, or over the built-in display: it is irrelevant for volume.
        let builtin = display(id: "built-in", origin: CGPoint(x: 5000, y: 5000), builtin: true)
        for pointer in [elsewhere, CGPoint(x: 1500, y: 20), CGPoint(x: 5100, y: 5100)] {
            expect(holdOwners(router, controls, pointer: pointer, displays: [monitor, builtin]) == [monitorA, monitorA, monitorA],
                   "matched monitor owns the whole volume hold regardless of pointer")
        }
        router.shutdown()

        func passes(_ uid: String?, _ displays: [HardwareKeyDisplay], _ message: String) {
            let passControls = FakeControls()
            let passRouter = HardwareKeyRouter(controls: passControls)
            passRouter.setDefaultOutput(uid: uid)
            // The pointer sits over a supported monitor on purpose.
            expect(holdOwners(passRouter, passControls, pointer: onMonitor, displays: displays) == [nil, nil, nil], message)
        }
        passes("BuiltInSpeakerDevice", [monitor], "built-in output resolves no volume owner")
        passes("AppleUSBAudioEngine:Unmatched", [monitor], "unmatched UID resolves no volume owner")
        passes(nil, [monitor], "nil output resolves no volume owner")
        passes(outputUID, [monitor, display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0), audioUID: outputUID)],
               "duplicate audio UIDs resolve no volume owner")
        passes(outputUID, [display(volumeSupported: false, audioUID: outputUID)], "unsupported volume resolves no volume owner")
        passes(outputUID, [display(awake: false, audioUID: outputUID)], "sleeping monitor resolves no volume owner")
        passes(outputUID, [display(audioUID: nil)], "unstable identity resolves no volume owner")
        passes(outputUID, [display(builtin: true, audioUID: outputUID)], "built-in display resolves no volume owner")

        let caseControls = FakeControls()
        let caseRouter = HardwareKeyRouter(controls: caseControls)
        caseRouter.setDefaultOutput(uid: outputUID.lowercased())
        expect(holdOwners(caseRouter, caseControls, pointer: elsewhere, displays: [display(audioUID: outputUID)]) == [monitorA, monitorA, monitorA],
               "case-insensitive UID match owns the hold")
        caseRouter.shutdown()
    }

    /// The output switches away mid-hold: repeats drop to no owner, the release still sends the held owner, and the next press follows the new output.
    private static func volumeHoldAcrossOutputChanges() {
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        let monitor = display(audioUID: outputUID)
        router.setDefaultOutput(uid: outputUID)
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]) == monitorA,
               "hold starts on the monitor")
        router.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: elsewhere, displays: [monitor]) == nil,
               "repeat after the output switched away resolves no owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: elsewhere, displays: [monitor]) == nil,
               "later repeats resolve no owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor]) == monitorA,
               "release still sends the held owner after the output switched away")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]) == nil,
               "next press follows the new output and resolves nothing")
        _ = routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [monitor])

        // A press declined for lack of an owner stays unowned even if the output becomes the monitor mid-hold.
        let passControls = FakeControls()
        let passRouter = HardwareKeyRouter(controls: passControls)
        passRouter.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(routed(passRouter, passControls, key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [monitor]) == nil,
               "hold starts unowned")
        passRouter.setDefaultOutput(uid: outputUID)
        expect(routed(passRouter, passControls, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: .zero, displays: [monitor]) == nil,
               "repeat stays unowned after the output switched to the monitor")
        expect(routed(passRouter, passControls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: .zero, displays: [monitor]) == nil,
               "release stays unowned after the output switched to the monitor")
        expect(routed(passRouter, passControls, key(Int(NX_KEYTYPE_SOUND_UP)), pointer: .zero, displays: [monitor]) == monitorA,
               "next distinct press re-evaluates and targets the monitor")
        passRouter.shutdown()
        router.shutdown()
    }

    /// A display replaced by a new generation mid-hold: the repeat resolves no owner, and the release still sends the held one.
    private static func staleGenerationReleaseKeepsHeldOwner() {
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [display(audioUID: outputUID)]) == monitorA,
               "volume hold starts on generation one")
        let replaced = display(generation: 2, audioUID: outputUID)
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: elsewhere, displays: [replaced]) == nil,
               "stale volume repeat resolves no owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: elsewhere, displays: [replaced]) == monitorA,
               "stale volume release still sends the held owner")
        router.shutdown()
    }

    /// Ineligible modifiers resolve no owner; Shift+Option keeps the owner and reaches the core as the fine flag.
    private static func modifiersAndFineStep() {
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        let monitor = display(audioUID: outputUID)
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), modifiersEligible: false), displays: [monitor]) == nil,
               "disallowed modifiers resolve no volume owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up, modifiersEligible: false), displays: [monitor]) == nil,
               "disallowed-modifier release resolves no volume owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_DOWN), fine: true), displays: [monitor]) == monitorA,
               "fine press keeps the volume owner")
        expect(controls.calls.last == ControlsCall(key: "volume_down", down: true, isRepeat: false, fine: true, owner: monitorA),
               "the fine flag reaches the core")
        _ = routed(router, controls, key(Int(NX_KEYTYPE_SOUND_DOWN), phase: .up), displays: [monitor])
        router.shutdown()
    }

    /// Brightness follows the pointer and ignores the sound output; the brightness and volume latches are independent.
    private static func brightnessAndVolumeLatchesAreIndependent() {
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        let both = display(audioUID: outputUID)
        router.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: onMonitor, displays: [both]) == monitorA,
               "brightness follows the pointer when the output is elsewhere")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), pointer: onMonitor, displays: [both]) == nil,
               "volume resolves no owner while the output is elsewhere")
        router.setDefaultOutput(uid: outputUID)
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: onMonitor, displays: [both]) == monitorA,
               "brightness latch is unaffected by the output change")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), repeat: true), pointer: onMonitor, displays: [both]) == nil,
               "volume latch stays unowned after the output changes")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: onMonitor, displays: [both]) == monitorA,
               "brightness release sends its owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), pointer: onMonitor, displays: [both]) == nil,
               "volume release sends no owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: elsewhere, displays: [both]) == nil,
               "brightness outside every display resolves no owner even when the monitor is the output")
        _ = routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: elsewhere, displays: [both])
        router.shutdown()
    }

    /// Mute is owned by the default output's monitor and is never followed; it resolves no owner in the same
    /// cases in which volume does.
    private static func muteOwnership() {
        let loud = display(audioUID: outputUID)
        let silent = display(volume: 0, audioUID: outputUID)
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        expect(routed(router, controls, muteKey(), displays: [loud]) == monitorA, "mute press resolves the default output's monitor")
        expect(controls.calls.last == ControlsCall(key: "mute", down: true, isRepeat: false, fine: false, owner: monitorA),
               "mute press is forwarded as mute")
        expect(routed(router, controls, muteKey(repeat: true), displays: [loud]) == monitorA, "held mute repeat keeps the owner")
        expect(routed(router, controls, muteKey(phase: .up), displays: [loud]) == monitorA, "mute release sends the owner")
        expect(routed(router, controls, muteKey(phase: .up), displays: [loud]) == nil, "a second mute release has no owner")
        expect(routed(router, controls, muteKey(), displays: [silent]) == monitorA,
               "mute at zero still resolves the owner; the core decides the write")
        _ = routed(router, controls, muteKey(phase: .up), displays: [silent])
        router.setFollowedBrightnessDisplays(["monitor-a"])
        expect(routed(router, controls, muteKey(), displays: [loud]) == monitorA, "a followed monitor never drops mute")
        _ = routed(router, controls, muteKey(phase: .up), displays: [loud])
        router.shutdown()

        let changeControls = FakeControls()
        let changeRouter = HardwareKeyRouter(controls: changeControls)
        changeRouter.setDefaultOutput(uid: outputUID)
        expect(routed(changeRouter, changeControls, muteKey(), displays: [loud]) == monitorA, "held mute starts on the monitor")
        changeRouter.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(routed(changeRouter, changeControls, muteKey(repeat: true), displays: [loud]) == nil,
               "held mute repeat after the output switched resolves no owner")
        expect(routed(changeRouter, changeControls, muteKey(phase: .up), displays: [loud]) == monitorA,
               "held mute release still sends the owner")
        expect(routed(changeRouter, changeControls, muteKey(), displays: [loud]) == nil,
               "the next mute press follows the new output")
        _ = routed(changeRouter, changeControls, muteKey(phase: .up), displays: [loud])
        changeRouter.shutdown()

        func mutePasses(_ uid: String?, _ displays: [HardwareKeyDisplay], _ message: String) {
            let passControls = FakeControls()
            let passRouter = HardwareKeyRouter(controls: passControls)
            passRouter.setDefaultOutput(uid: uid)
            expect(holdOwners(passRouter, passControls, keyType: Int(NX_KEYTYPE_MUTE), pointer: .zero, displays: displays) == [nil, nil, nil],
                   message)
        }
        mutePasses("BuiltInSpeakerDevice", [loud], "built-in output mute resolves no owner")
        mutePasses(nil, [loud], "nil output mute resolves no owner")
        mutePasses(outputUID, [display(volumeSupported: false, audioUID: outputUID)], "unsupported volume mute resolves no owner")
        mutePasses(outputUID, [display(awake: false, audioUID: outputUID)], "sleeping monitor mute resolves no owner")
        mutePasses(outputUID, [display(audioUID: nil)], "unstable identity mute resolves no owner")
        mutePasses(outputUID, [display(builtin: true, audioUID: outputUID)], "built-in display mute resolves no owner")

        let ineligibleControls = FakeControls()
        let ineligibleRouter = HardwareKeyRouter(controls: ineligibleControls)
        ineligibleRouter.setDefaultOutput(uid: outputUID)
        expect(routed(ineligibleRouter, ineligibleControls, muteKey(modifiersEligible: false), displays: [loud]) == nil,
               "mute with disallowed modifiers resolves no owner")
        expect(routed(ineligibleRouter, ineligibleControls, muteKey(phase: .up, modifiersEligible: false), displays: [loud]) == nil,
               "mute release with disallowed modifiers resolves no owner")
        ineligibleRouter.shutdown()
    }

    private static func disableAndShutdownClearOutput() {
        let monitor = display(audioUID: outputUID)
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        router.apply(enabled: false, displays: [monitor])
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [monitor]) == nil,
               "disabling routing clears the default output")
        _ = routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), displays: [monitor])
        router.shutdown()
        router.setDefaultOutput(uid: outputUID)
        let before = controls.calls.count
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "a stopped router passes the key through")
        expect(controls.calls.count == before, "a stopped router does not reach the core")
    }

    private static func makeRouter(_ permissions: FakePermissions, _ installer: FakeTapInstaller,
                                   _ controls: HardwareKeyControlling = FakeControls()) -> HardwareKeyRouter {
        HardwareKeyRouter(controls: controls, permissionProvider: { permissions.value }, tapInstaller: installer.install)
    }

    private static func permissionLossKeepsOutput() {
        let monitor = display(audioUID: outputUID)
        let permissions = FakePermissions(false, true)
        let controls = FakeControls()
        let router = makeRouter(permissions, FakeTapInstaller(), controls)
        router.setDefaultOutput(uid: outputUID)
        router.apply(enabled: true, displays: [monitor])
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [monitor]) == monitorA,
               "permission loss does not clear the default output")
        _ = routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), displays: [monitor])
        router.apply(enabled: false, displays: [monitor])
        expect(routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP)), displays: [monitor]) == nil,
               "disable still clears the default output after permission loss")
        _ = routed(router, controls, key(Int(NX_KEYTYPE_SOUND_UP), phase: .up), displays: [monitor])

        let other = makeRouter(permissions, FakeTapInstaller(), controls)
        other.setDefaultOutput(uid: outputUID)
        other.apply(enabled: true, displays: [monitor])
        other.shutdown()
        expect(!other.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "shutdown still clears the default output")
    }

    /// The router passes the key name, phase, repeat and fine flags and the owner to the core, and returns the core's answer.
    private static func routerForwardsToCore() {
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        let monitor = display(audioUID: outputUID)
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: onMonitor, displays: [monitor]),
               "a press the core consumes is consumed")
        expect(controls.calls.last == ControlsCall(key: "brightness_up", down: true, isRepeat: false, fine: false, owner: monitorA),
               "the press is forwarded with its name, phase and owner")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: elsewhere, displays: [monitor]),
               "a repeat the core consumes is consumed")
        expect(controls.calls.last == ControlsCall(key: "brightness_up", down: true, isRepeat: true, fine: false, owner: monitorA),
               "the repeat is forwarded with the latched owner")
        expect(router.processForTesting(key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: elsewhere, displays: [monitor]),
               "a release the core consumes is consumed")
        expect(controls.calls.last == ControlsCall(key: "brightness_up", down: false, isRepeat: false, fine: false, owner: monitorA),
               "the release is forwarded with the latched owner")

        controls.answer = false
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_DOWN), fine: true), pointer: elsewhere, displays: [monitor]),
               "a press the core declines passes through")
        expect(controls.calls.last == ControlsCall(key: "volume_down", down: true, isRepeat: false, fine: true, owner: monitorA),
               "the fine volume press is forwarded with the fine flag")
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_DOWN), phase: .up), pointer: elsewhere, displays: [monitor]),
               "the release of a declined press passes through")
        expect(controls.calls.last == ControlsCall(key: "volume_down", down: false, isRepeat: false, fine: false, owner: nil),
               "a declined press latches nothing, so its release carries no owner")

        controls.answer = true
        expect(router.processForTesting(muteKey(), pointer: elsewhere, displays: [monitor]), "mute press is consumed")
        expect(controls.calls.last == ControlsCall(key: "mute", down: true, isRepeat: false, fine: false, owner: monitorA),
               "mute is forwarded as mute with the monitor as owner")
        router.shutdown()
    }

    /// The volume owner follows the default output and the displays, and is nil while routing is off or stopped.
    private static func volumeOwnerMonitorFollowsOutput() {
        let installer = FakeTapInstaller()
        installer.allow = true
        let router = makeRouter(FakePermissions(true, true), installer)
        let monitor = display(audioUID: outputUID)
        expect(router.volumeOwnerMonitorID() == nil, "no volume owner before routing is enabled")
        router.setDefaultOutput(uid: outputUID)
        router.apply(enabled: true, displays: [monitor])
        expect(router.volumeOwnerMonitorID() == "monitor-a", "the default output's monitor owns volume")
        router.setDefaultOutput(uid: outputUID.lowercased())
        expect(router.volumeOwnerMonitorID() == "monitor-a", "the output UID match is case-insensitive")
        router.setDefaultOutput(uid: "BuiltInSpeakerDevice")
        expect(router.volumeOwnerMonitorID() == nil, "another default output owns no monitor")
        router.setDefaultOutput(uid: outputUID)
        router.apply(enabled: true, displays: [display(volume: nil, audioUID: outputUID)])
        expect(router.volumeOwnerMonitorID() == nil, "an unknown volume value owns no monitor")
        router.apply(enabled: true, displays: [monitor, display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0), audioUID: outputUID)])
        expect(router.volumeOwnerMonitorID() == nil, "duplicate output UIDs own no monitor")
        router.apply(enabled: true, displays: [monitor])
        expect(router.volumeOwnerMonitorID() == "monitor-a", "the monitor owns volume again once the duplicate is gone")
        router.apply(enabled: false, displays: [monitor])
        expect(router.volumeOwnerMonitorID() == nil, "disabled routing owns no monitor")
        router.shutdown()
        expect(router.volumeOwnerMonitorID() == nil, "a stopped router owns no monitor")
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

    /// A late snapshot cannot re-enable a shut-down router, and a stopped router never reaches the core.
    private static func lateSnapshotsCannotRestartStoppedRouter() {
        let monitor = display(audioUID: outputUID)
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        router.shutdown()
        router.apply(enabled: true, displays: [monitor])
        let before = controls.calls.count
        expect(!router.processForTesting(key(Int(NX_KEYTYPE_SOUND_UP)), pointer: elsewhere, displays: [monitor]),
               "late snapshots cannot restart a stopped router")
        expect(controls.calls.count == before, "a stopped router does not reach the core")
        drain()
    }

    /// Brightness keys on a followed monitor resolve no owner on press, repeat and release; without the set they are owned.
    private static func followedBrightnessPassesThrough() {
        let monitor = display()
        let control = FakeControls()
        let controlRouter = HardwareKeyRouter(controls: control)
        expect(holdOwners(controlRouter, control, keyType: Int(NX_KEYTYPE_BRIGHTNESS_UP), pointer: onMonitor, displays: [monitor])
                == [monitorA, monitorA, monitorA],
               "control: brightness hold is owned before any set is installed")
        controlRouter.shutdown()

        let followed = FakeControls()
        let router = HardwareKeyRouter(controls: followed)
        router.setFollowedBrightnessDisplays(["monitor-a"])
        expect(holdOwners(router, followed, keyType: Int(NX_KEYTYPE_BRIGHTNESS_UP), pointer: onMonitor, displays: [monitor]) == [nil, nil, nil],
               "brightness down, repeat and up on a followed monitor resolve no owner")
        expect(holdOwners(router, followed, keyType: Int(NX_KEYTYPE_BRIGHTNESS_DOWN), pointer: onMonitor, displays: [monitor]) == [nil, nil, nil],
               "brightness-down on a followed monitor resolves no owner")
        router.shutdown()
    }

    /// Volume and mute on the same followed monitor keep their owner, exactly as before the set existed.
    private static func followedVolumeAndMuteUnchanged() {
        let monitor = display(audioUID: outputUID)
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setDefaultOutput(uid: outputUID)
        router.setFollowedBrightnessDisplays(["monitor-a"])
        expect(holdOwners(router, controls, keyType: Int(NX_KEYTYPE_SOUND_UP), pointer: onMonitor, displays: [monitor]) == [monitorA, monitorA, monitorA],
               "followed volume up still resolves the monitor")
        expect(holdOwners(router, controls, keyType: Int(NX_KEYTYPE_SOUND_DOWN), pointer: onMonitor, displays: [monitor]) == [monitorA, monitorA, monitorA],
               "followed volume down still resolves the monitor")
        expect(holdOwners(router, controls, keyType: Int(NX_KEYTYPE_MUTE), pointer: onMonitor, displays: [monitor]) == [monitorA, monitorA, monitorA],
               "followed mute still resolves the monitor")
        router.shutdown()
    }

    /// A brightness key on a monitor outside the set is still owned, while the followed one next to it resolves none.
    private static func unfollowedBrightnessStillHandled() {
        let followed = display()
        let other = display(id: "monitor-b", origin: CGPoint(x: 2000, y: 0))
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setFollowedBrightnessDisplays(["monitor-a"])
        expect(holdOwners(router, controls, keyType: Int(NX_KEYTYPE_BRIGHTNESS_UP), pointer: onMonitor, displays: [followed, other]) == [nil, nil, nil],
               "followed monitor under the pointer resolves no brightness owner")
        expect(holdOwners(router, controls, keyType: Int(NX_KEYTYPE_BRIGHTNESS_DOWN), pointer: onOther, displays: [followed, other]) == [monitorB, monitorB, monitorB],
               "unfollowed monitor under the pointer keeps its brightness owner")
        router.shutdown()
    }

    /// Clearing the followed set restores brightness ownership on the next press.
    private static func emptyFollowedSetRestoresBrightness() {
        let monitor = display()
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        router.setFollowedBrightnessDisplays(["monitor-a"])
        expect(holdOwners(router, controls, keyType: Int(NX_KEYTYPE_BRIGHTNESS_UP), pointer: onMonitor, displays: [monitor]) == [nil, nil, nil],
               "followed brightness resolves no owner before the set is cleared")
        router.setFollowedBrightnessDisplays([])
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: onMonitor, displays: [monitor]) == monitorA,
               "an empty set restores brightness ownership")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: onMonitor, displays: [monitor]) == monitorA,
               "the restored press is released with its owner")
        router.shutdown()
    }

    /// A set change mid-press keeps the latched owner; the new set decides only the next key-down.
    private static func followedChangeKeepsInFlightPress() {
        let monitor = display()
        let controls = FakeControls()
        let router = HardwareKeyRouter(controls: controls)
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: onMonitor, displays: [monitor]) == monitorA,
               "press is owned before the set changes")
        router.setFollowedBrightnessDisplays(["monitor-a"])
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), repeat: true), pointer: onMonitor, displays: [monitor]) == monitorA,
               "held repeat keeps its owner after the set changes")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: onMonitor, displays: [monitor]) == monitorA,
               "release of the owned press still sends the owner")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP)), pointer: onMonitor, displays: [monitor]) == nil,
               "the next key-down follows the new set and resolves nothing")
        expect(routed(router, controls, key(Int(NX_KEYTYPE_BRIGHTNESS_UP), phase: .up), pointer: onMonitor, displays: [monitor]) == nil,
               "release of the unowned press resolves nothing")
        router.shutdown()
    }
}
