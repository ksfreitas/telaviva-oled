import AppKit
import ApplicationServices
import Combine
import Foundation
import IOKit.hidsystem

enum HardwareKeyFeature: Hashable {
    case brightness
    case volume
}

struct HardwareKeyFeatureState {
    let value: Double?
    let supported: Bool
    let awake: Bool
}

/// Current online display geometry is included even when the display cannot receive a command.
struct HardwareKeyDisplay {
    let displayID: CGDirectDisplayID
    let bounds: CGRect
    let isBuiltin: Bool
    let monitorID: String?
    let generation: UInt64?
    /// CoreAudio device UID of this display's speakers; nil when the identity is not stable.
    let audioDeviceUID: String?
    let brightness: HardwareKeyFeatureState
    let volume: HardwareKeyFeatureState

    func state(for feature: HardwareKeyFeature) -> HardwareKeyFeatureState {
        feature == .brightness ? brightness : volume
    }
}

enum HardwareKeyRouterStatus: Equatable {
    case disabled
    case enabled
    case permissionRequired
    case permissionRevoked
    case listenerUnavailable
    case inputMonitoringRequired
}

/// Accessibility and Input Monitoring are separate TCC grants; the event tap needs both.
struct HardwareKeyPermissions: Equatable {
    var accessibility: Bool
    var inputMonitoring: Bool

    static func current() -> HardwareKeyPermissions {
        HardwareKeyPermissions(accessibility: AXIsProcessTrusted(), inputMonitoring: CGPreflightListenEventAccess())
    }
}

/// Pure mapping from the router's observable state to its published status.
enum HardwareKeyStatusPolicy {
    static func evaluate(enabled: Bool, permissions: HardwareKeyPermissions, tapInstalled: Bool,
                         tapCreateFailed: Bool, hadActiveTap: Bool) -> HardwareKeyRouterStatus {
        guard enabled else { return .disabled }
        guard permissions.accessibility else { return hadActiveTap ? .permissionRevoked : .permissionRequired }
        guard permissions.inputMonitoring else { return .inputMonitoringRequired }
        if tapCreateFailed { return .listenerUnavailable }
        return tapInstalled ? .enabled : .listenerUnavailable
    }
}

/// Decoded from the public NX_SYSDEFINED auxiliary-control-button event payload.
struct HardwareKeyEvent {
    enum Phase { case down, up }
    enum Direction { case increase, decrease }

    let keyIdentity: Int
    let feature: HardwareKeyFeature
    let direction: Direction
    let phase: Phase
    let isRepeat: Bool
    let isFineStep: Bool
    let modifiersEligible: Bool
    /// The mute key toggles the monitor volume instead of stepping it; its feature is .volume.
    var isMute = false

    /// The key name the core's key machine expects: brightness_up, brightness_down, volume_up, volume_down or mute.
    var controlsKey: String {
        if isMute { return "mute" }
        switch (feature, direction) {
        case (.brightness, .increase): return "brightness_up"
        case (.brightness, .decrease): return "brightness_down"
        case (.volume, .increase): return "volume_up"
        case (.volume, .decrease): return "volume_down"
        }
    }

    static func decode(type: CGEventType, subtype: Int, data1: Int64, flags: CGEventFlags) -> HardwareKeyEvent? {
        guard type.rawValue == UInt32(NX_SYSDEFINED) else { return nil }
        guard subtype == Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS) else { return nil }
        let payload = UInt64(bitPattern: data1)
        let key = Int((payload >> 16) & 0xFFFF)
        let feature: HardwareKeyFeature
        let direction: Direction
        var isMute = false
        switch key {
        case Int(NX_KEYTYPE_BRIGHTNESS_UP): feature = .brightness; direction = .increase
        case Int(NX_KEYTYPE_BRIGHTNESS_DOWN): feature = .brightness; direction = .decrease
        case Int(NX_KEYTYPE_SOUND_UP): feature = .volume; direction = .increase
        case Int(NX_KEYTYPE_SOUND_DOWN): feature = .volume; direction = .decrease
        case Int(NX_KEYTYPE_MUTE): feature = .volume; direction = .decrease; isMute = true
        default: return nil // Ordinary function keys.
        }

        let (modifiers, fine) = modifierState(flags)
        let state = (payload >> 8) & 0xFF
        guard state == 0x0A || state == 0x0B else { return nil }
        return HardwareKeyEvent(keyIdentity: key, feature: feature, direction: direction,
                                phase: state == 0x0A ? .down : .up,
                                isRepeat: (payload & 1) != 0, isFineStep: fine,
                                // Mute is handled only without any modifier; Shift+Option is not a mute combination.
                                modifiersEligible: modifiers.isEmpty || (fine && !isMute),
                                isMute: isMute)
    }

    /// Apple Silicon keyboards send brightness as ordinary key events with key codes 144 (up) and 145 (down),
    /// not as NX_SYSDEFINED media events. Only the key code, autorepeat flag and modifier flags are read.
    static func decodeKeyboard(type: CGEventType, keyCode: Int64, isRepeat: Bool, flags: CGEventFlags) -> HardwareKeyEvent? {
        guard type == .keyDown || type == .keyUp else { return nil }
        let identity: Int
        let direction: Direction
        switch keyCode {
        case 144: identity = Int(NX_KEYTYPE_BRIGHTNESS_UP); direction = .increase
        case 145: identity = Int(NX_KEYTYPE_BRIGHTNESS_DOWN); direction = .decrease
        default: return nil // Every other key is left untouched.
        }
        let (modifiers, fine) = modifierState(flags)
        return HardwareKeyEvent(keyIdentity: identity, feature: .brightness, direction: direction,
                                phase: type == .keyDown ? .down : .up,
                                isRepeat: isRepeat, isFineStep: fine,
                                modifiersEligible: modifiers.isEmpty || fine)
    }

    /// Shared by both decoders. fn (.maskSecondaryFn) is deliberately outside the mask because brightness keys always carry it.
    private static func modifierState(_ flags: CGEventFlags) -> (modifiers: CGEventFlags, fine: Bool) {
        let modifierMask: CGEventFlags = [.maskShift, .maskAlternate, .maskCommand, .maskControl, .maskAlphaShift, .maskNumericPad, .maskHelp]
        let modifiers = flags.intersection(modifierMask)
        return (modifiers, modifiers == [.maskShift, .maskAlternate])
    }
}

/// Target selection contains no operating-system or DDC side effects.
enum HardwareKeyPolicy {
    static func target(at point: CGPoint, displays: [HardwareKeyDisplay], feature: HardwareKeyFeature) -> HardwareKeyDisplay? {
        let hits = displays.filter { $0.bounds.contains(point) }
        guard hits.count == 1, let display = hits.first, !display.isBuiltin,
              display.monitorID != nil, display.generation != nil else { return nil }
        let state = display.state(for: feature)
        guard state.supported, state.awake, let value = state.value,
              value.isFinite, (0...100).contains(value) else { return nil }
        return display
    }

    /// Volume follows the Mac's default sound output, never the pointer or a display name.
    /// Any ambiguity (no output, duplicate UIDs, unstable identity) selects nothing.
    static func volumeTarget(defaultOutputUID: String?, displays: [HardwareKeyDisplay]) -> HardwareKeyDisplay? {
        let matches = displays.filter { uidsMatch($0.audioDeviceUID, defaultOutputUID) }
        guard matches.count == 1, let display = matches.first, !display.isBuiltin,
              display.monitorID != nil, display.generation != nil else { return nil }
        let state = display.volume
        guard state.supported, state.awake, let value = state.value,
              value.isFinite, (0...100).contains(value) else { return nil }
        return display
    }

    /// Case-insensitive equality; a missing or empty UID never matches.
    static func uidsMatch(_ lhs: String?, _ rhs: String?) -> Bool {
        guard let lhs, let rhs, !lhs.isEmpty, !rhs.isEmpty else { return false }
        return lhs.caseInsensitiveCompare(rhs) == .orderedSame
    }
}

/// The monitor a key event was resolved to, and the connection generation it was resolved against.
struct HardwareKeyOwner: Equatable { let monitorID: String; let generation: UInt64 }

/// The core's key machine as the router sees it. Called synchronously from the event tap.
protocol HardwareKeyControlling: AnyObject {
    /// Returns true when the key event must be swallowed.
    func key(_ key: String, down: Bool, isRepeat: Bool, fine: Bool, owner: HardwareKeyOwner?) -> Bool
}

/// Resolves the owner of each key event and keeps the owner latched for the held key until its release.
/// Performs no operating-system or core calls.
struct HardwareKeyOwnerResolver {
    /// Owners of the keys currently held, by keyIdentity.
    private var held: [Int: HardwareKeyOwner] = [:]

    mutating func clear() { held.removeAll(keepingCapacity: true) }

    /// The owner to send with `event`; nil means "no eligible target".
    mutating func owner(for event: HardwareKeyEvent, pointer: CGPoint, enabled: Bool, defaultOutputUID: String?,
                        displays: [HardwareKeyDisplay], followedBrightness: Set<String>) -> HardwareKeyOwner? {
        switch event.phase {
        case .up:
            // A release carries the owner its press latched, so the core can finish that press.
            return held[event.keyIdentity]
        case .down where event.isRepeat:
            // A repeat keeps the press's owner while it is still valid; a repeat without a press has none.
            guard let owner = held[event.keyIdentity],
                  Self.isValid(owner, feature: event.feature, defaultOutputUID: defaultOutputUID, displays: displays) else { return nil }
            return owner
        case .down:
            held.removeValue(forKey: event.keyIdentity)
            guard enabled, event.modifiersEligible else { return nil }
            let target = event.feature == .volume
                ? HardwareKeyPolicy.volumeTarget(defaultOutputUID: defaultOutputUID, displays: displays)
                : HardwareKeyPolicy.target(at: pointer, displays: displays, feature: event.feature)
            guard let target, let monitorID = target.monitorID, let generation = target.generation else { return nil }
            // A followed monitor keeps its brightness keys on macOS; volume and mute are never followed.
            if event.feature == .brightness && followedBrightness.contains(monitorID) { return nil }
            return HardwareKeyOwner(monitorID: monitorID, generation: generation)
        }
    }

    /// Records the core's answer for `event`. A consumed press latches its owner; a declined repeat and any release unlatch.
    mutating func record(_ event: HardwareKeyEvent, owner: HardwareKeyOwner?, consumed: Bool) {
        switch event.phase {
        case .down where !event.isRepeat:
            if consumed, let owner { held[event.keyIdentity] = owner } else { held.removeValue(forKey: event.keyIdentity) }
        case .down:
            if !consumed { held.removeValue(forKey: event.keyIdentity) }
        case .up:
            held.removeValue(forKey: event.keyIdentity)
        }
    }

    /// A held owner stays valid while its exact monitor and generation remain eligible for the feature;
    /// volume additionally needs the monitor to still be the default sound output.
    private static func isValid(_ owner: HardwareKeyOwner, feature: HardwareKeyFeature, defaultOutputUID: String?,
                                displays: [HardwareKeyDisplay]) -> Bool {
        guard let display = displays.first(where: { $0.monitorID == owner.monitorID && $0.generation == owner.generation }),
              !display.isBuiltin else { return false }
        let state = display.state(for: feature)
        guard state.supported, state.awake, state.value?.isFinite == true else { return false }
        return feature != .volume || HardwareKeyPolicy.uidsMatch(display.audioDeviceUID, defaultOutputUID)
    }
}

/// The router owns a tiny event callback. Each key event makes one synchronous call into the core's
/// leaf-locked key machine, and the router never writes to a monitor itself.
final class HardwareKeyRouter: ObservableObject {
    typealias TapInstaller = () -> (CFMachPort, CFRunLoopSource)?

    @Published private(set) var status: HardwareKeyRouterStatus = .disabled
    var onStatusChange: ((HardwareKeyRouterStatus) -> Void)?
    /// Always invoked on the main queue, once on the first evaluation and then only when the value changes.
    var onPermissionsChange: ((HardwareKeyPermissions) -> Void)?

    private let controls: HardwareKeyControlling
    private let permissionProvider: () -> HardwareKeyPermissions
    private let customTapInstaller: TapInstaller?
    private let stateLock = NSLock()
    private var enabled = false
    private var stopped = false
    private var displays: [HardwareKeyDisplay] = []
    private var defaultOutputUID: String?
    /// Monitors whose brightness keys pass through to macOS; written by setFollowedBrightnessDisplays under stateLock.
    private var followedBrightnessDisplays = Set<String>()
    /// Owners latched by held keys; cleared on disable, permission loss and shutdown.
    private var resolver = HardwareKeyOwnerResolver()
    private var tap: CFMachPort?
    private var tapSource: CFRunLoopSource?
    private var tapReenableAttempted = false
    private var tapCreationFailed = false
    private var lastPermissions: HardwareKeyPermissions?

    init(controls: HardwareKeyControlling,
         permissionProvider: @escaping () -> HardwareKeyPermissions = HardwareKeyPermissions.current,
         tapInstaller: TapInstaller? = nil) {
        self.controls = controls
        self.permissionProvider = permissionProvider
        self.customTapInstaller = tapInstaller
    }

    /// Re-reads permissions and retries a failed tap creation. Callable from any thread; there is
    /// no polling, so the UI calls this when the app becomes active or after a request.
    func refreshPermissions() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.refreshPermissions() }
            return
        }
        evaluate(forceRetry: true)
    }

    /// Called only from an explicit user action. Shows the macOS Accessibility prompt.
    func requestAccessibilityFromUserAction() {
        guard Thread.isMainThread, !stopped else { return }
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
        refreshPermissions()
    }

    /// Called only from an explicit user action. Requests the Input Monitoring grant.
    func requestInputMonitoringFromUserAction() {
        guard Thread.isMainThread, !stopped else { return }
        _ = CGRequestListenEventAccess()
        refreshPermissions()
    }

    /// Called only from an explicit user action. Requests whichever permission is missing.
    @discardableResult
    func requestPermissionFromUserAction() -> Bool {
        guard Thread.isMainThread, !stopped else { return false }
        let before = permissionProvider()
        if !before.accessibility { requestAccessibilityFromUserAction() }
        if !before.inputMonitoring { requestInputMonitoringFromUserAction() }
        let after = permissionProvider()
        return after.accessibility && after.inputMonitoring
    }

    func apply(enabled nextEnabled: Bool, displays nextDisplays: [HardwareKeyDisplay]) {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.apply(enabled: nextEnabled, displays: nextDisplays) }
            return
        }
        stateLock.lock()
        guard !stopped else { stateLock.unlock(); return }
        enabled = nextEnabled
        displays = nextDisplays.count <= 64 ? nextDisplays : []
        stateLock.unlock()
        evaluate(forceRetry: false)
    }

    /// Single decision path shared by snapshots and permission refreshes. Main thread only.
    private func evaluate(forceRetry: Bool) {
        let permissions = permissionProvider()
        publish(permissions, forceRetry: forceRetry)
        stateLock.lock()
        let isEnabled = enabled
        let isStopped = stopped
        stateLock.unlock()
        guard !isStopped else { return }
        guard isEnabled else {
            clearPending(clearOutput: true)
            stopTap()
            tapCreationFailed = false
            setStatus(.disabled)
            return
        }
        guard permissions.accessibility else {
            let hadActiveTap = tap != nil || status == .permissionRevoked
            clearPending() // The default output UID survives permission loss; only disable/shutdown clear it.
            stopTap()
            setStatus(HardwareKeyStatusPolicy.evaluate(enabled: true, permissions: permissions, tapInstalled: false,
                                                       tapCreateFailed: tapCreationFailed, hadActiveTap: hadActiveTap))
            return
        }
        ensureTap(permissions)
    }

    private func publish(_ permissions: HardwareKeyPermissions, forceRetry: Bool) {
        if permissions != lastPermissions {
            // A changed grant is the only automatic reason to retry a failed tap creation.
            if lastPermissions != nil { tapCreationFailed = false }
            // A tap created before Input Monitoring was granted may never receive events; recreate it.
            if let previous = lastPermissions, !previous.inputMonitoring, permissions.inputMonitoring { stopTap() }
            lastPermissions = permissions
            DispatchQueue.main.async { [weak self] in self?.onPermissionsChange?(permissions) }
        }
        if forceRetry { tapCreationFailed = false }
    }

    /// Stores the default sound output UID read off the event-tap path; nil means volume keys have no owner.
    func setDefaultOutput(uid: String?) {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard !stopped else { return }
        defaultOutputUID = uid
    }

    /// Stores the monitors whose brightness keys pass through to macOS. Only the next key-down sees a change;
    /// a press already latched keeps its owner.
    func setFollowedBrightnessDisplays(_ monitorIDs: Set<String>) {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard !stopped else { return }
        followedBrightnessDisplays = monitorIDs
    }

    /// The monitor that volume and mute keys resolve to right now, or nil when routing is off or no target is eligible.
    func volumeOwnerMonitorID() -> String? {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard enabled, !stopped else { return nil }
        return HardwareKeyPolicy.volumeTarget(defaultOutputUID: defaultOutputUID, displays: displays)?.monitorID
    }

    func shutdown() {
        stateLock.lock()
        enabled = false
        stopped = true
        clearPendingLocked(clearOutput: true)
        stateLock.unlock()
        stopTap()
        setStatus(.disabled)
    }

    /// Deterministic seam used by the helper tests; it runs the same owner and core path as the tap callback
    /// without creating an event tap, prompting for permission, or touching a physical display.
    @discardableResult
    func processForTesting(_ event: HardwareKeyEvent, pointer: CGPoint,
                           displays nextDisplays: [HardwareKeyDisplay]) -> Bool {
        stateLock.lock()
        guard !stopped else { stateLock.unlock(); return false }
        enabled = true
        displays = nextDisplays.count <= 64 ? nextDisplays : []
        stateLock.unlock()
        return route(event, pointer: pointer)
    }

    private func ensureTap(_ permissions: HardwareKeyPermissions) {
        stateLock.lock()
        let shouldStart = enabled && !stopped
        stateLock.unlock()
        guard shouldStart else { return }
        if tap == nil, !tapCreationFailed {
            // The OS may refuse the tap without Input Monitoring; the policy then reports that.
            if let (created, source) = (customTapInstaller ?? installSystemTap)() {
                tap = created
                tapSource = source
                CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
                CGEvent.tapEnable(tap: created, enable: true)
            } else {
                tapCreationFailed = true
                clearPending()
            }
        }
        setStatus(HardwareKeyStatusPolicy.evaluate(enabled: true, permissions: permissions, tapInstalled: tap != nil,
                                                   tapCreateFailed: tapCreationFailed, hadActiveTap: false))
    }

    private func installSystemTap() -> (CFMachPort, CFRunLoopSource)? {
        let eventType = CGEventType(rawValue: UInt32(NX_SYSDEFINED))!
        let mask = (CGEventMask(1) << eventType.rawValue) |
            (CGEventMask(1) << CGEventType.keyDown.rawValue) |
            (CGEventMask(1) << CGEventType.keyUp.rawValue)
        guard let created = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap,
                                               options: .defaultTap, eventsOfInterest: mask,
                                               callback: Self.eventTapCallback,
                                               userInfo: Unmanaged.passUnretained(self).toOpaque()) else { return nil }
        return (created, CFMachPortCreateRunLoopSource(kCFAllocatorDefault, created, 0))
    }

    private func stopTap() {
        if let tap {
            CGEvent.tapEnable(tap: tap, enable: false)
            CFMachPortInvalidate(tap)
        }
        if let tapSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), tapSource, .commonModes) }
        tap = nil
        tapSource = nil
        tapReenableAttempted = false
    }

    private func clearPending(clearOutput: Bool = false) {
        stateLock.lock(); clearPendingLocked(clearOutput: clearOutput); stateLock.unlock()
    }

    private func clearPendingLocked(clearOutput: Bool = false) {
        resolver.clear()
        if clearOutput { defaultOutputUID = nil }
    }

    private func setStatus(_ next: HardwareKeyRouterStatus) {
        DispatchQueue.main.async { [weak self] in
            guard let self, self.status != next else { return }
            self.status = next
            self.onStatusChange?(next)
        }
    }

    private func handle(_ event: CGEvent) -> Unmanaged<CGEvent>? {
        if event.type == .tapDisabledByTimeout || event.type == .tapDisabledByUserInput {
            clearPending()
            if !stopped, enabled, !tapReenableAttempted, AXIsProcessTrusted() {
                tapReenableAttempted = true
                if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            } else {
                clearPending()
                stopTap()
                tapCreationFailed = true
                setStatus(.listenerUnavailable)
            }
            return Unmanaged.passUnretained(event)
        }
        guard AXIsProcessTrusted() else {
            clearPending()
            stopTap()
            setStatus(.permissionRevoked)
            return Unmanaged.passUnretained(event)
        }
        let decoded: HardwareKeyEvent
        if event.type == .keyDown || event.type == .keyUp {
            // Apple Silicon keyboards send brightness as plain key codes 144/145, not NX_SYSDEFINED events.
            // This tap sees every keystroke: only the key code, autorepeat flag and modifier flags are read.
            guard let keyboardEvent = HardwareKeyEvent.decodeKeyboard(type: event.type,
                                                                      keyCode: event.getIntegerValueField(.keyboardEventKeycode),
                                                                      isRepeat: event.getIntegerValueField(.keyboardEventAutorepeat) != 0,
                                                                      flags: event.flags) else { return Unmanaged.passUnretained(event) }
            decoded = keyboardEvent
        } else {
            guard let nativeEvent = NSEvent(cgEvent: event),
                  let systemEvent = HardwareKeyEvent.decode(type: event.type,
                                                             subtype: Int(nativeEvent.subtype.rawValue),
                                                             data1: Int64(nativeEvent.data1),
                                                             flags: event.flags) else { return Unmanaged.passUnretained(event) }
            decoded = systemEvent
        }
        let pointer = CGEvent(source: nil)?.location ?? CGPoint(x: CGFloat.infinity, y: CGFloat.infinity)
        // A consumed key is swallowed and a declined one reaches macOS unchanged, exactly as the core answered.
        return route(decoded, pointer: pointer) ? nil : Unmanaged.passUnretained(event)
    }

    /// The key path shared by the tap callback and the test seam. The owner is resolved under stateLock, the core is
    /// called outside it, and the core's answer is recorded under stateLock again. Returns the consume decision.
    private func route(_ event: HardwareKeyEvent, pointer: CGPoint) -> Bool {
        stateLock.lock()
        let owner = resolver.owner(for: event, pointer: pointer, enabled: enabled, defaultOutputUID: defaultOutputUID,
                                   displays: displays, followedBrightness: followedBrightnessDisplays)
        stateLock.unlock()
        let consume = controls.key(event.controlsKey, down: event.phase == .down, isRepeat: event.isRepeat,
                                   fine: event.isFineStep, owner: owner)
        stateLock.lock()
        resolver.record(event, owner: owner, consumed: consume)
        stateLock.unlock()
        return consume
    }

    private static let eventTapCallback: CGEventTapCallBack = { _, type, event, userInfo in
        guard let userInfo else { return Unmanaged.passUnretained(event) }
        let router = Unmanaged<HardwareKeyRouter>.fromOpaque(userInfo).takeUnretainedValue()
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput { event.type = type }
        return router.handle(event)
    }
}
