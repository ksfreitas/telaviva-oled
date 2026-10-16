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
    var pending = false
    var tolerance = 0.51
    var failed = false
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

struct HardwareKeyCommand {
    let monitorID: String
    let generation: UInt64
    let feature: HardwareKeyFeature
    let target: Double
    let intentRevision: UInt64
    let authorizationEpoch: UInt64
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

/// Target selection and normalized stepping contain no operating-system or DDC side effects.
enum HardwareKeyPolicy {
    static let defaultStep = 5.0
    static let minStep = 1.0
    static let maxStep = 15.0
    static let fineStep = 1.0

    /// Non-finite input falls back to the default; other input is rounded to whole points and kept in minStep...maxStep.
    static func clampedStep(_ value: Double) -> Double {
        guard value.isFinite else { return defaultStep }
        return min(maxStep, max(minStep, value.rounded()))
    }

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

    /// Fine steps always move one point; normal steps use the configured whole-point step.
    static func stepped(_ value: Double, direction: HardwareKeyEvent.Direction, fine: Bool, step: Double) -> Double {
        let amount = fine ? fineStep : step
        let delta = direction == .increase ? amount : -amount
        return min(100, max(0, value + delta))
    }
}

/// Latches the initial routing decision for each media key until its paired release.
struct HardwareKeySequence {
    private enum HeldKey {
        case passThrough
        case handled(monitorID: String, generation: UInt64, feature: HardwareKeyFeature,
                     direction: HardwareKeyEvent.Direction, fine: Bool, cancelled: Bool)
        case mute(monitorID: String, generation: UInt64, cancelled: Bool)
    }
    private var latches: [Int: HeldKey] = [:]

    mutating func clear() { latches.removeAll(keepingCapacity: true) }

    mutating func process(_ event: HardwareKeyEvent, pointer: CGPoint, enabled: Bool,
                          defaultOutputUID: String?,
                          displays: [HardwareKeyDisplay],
                          valueFor: (HardwareKeyDisplay, HardwareKeyFeature) -> Double?,
                          reserve: (HardwareKeyDisplay, HardwareKeyFeature, HardwareKeyEvent.Direction, Bool, Double) -> Bool,
                          reserveMute: (HardwareKeyDisplay, Double) -> Bool = { _, _ in false }) -> Bool {
        switch event.phase {
        case .up:
            guard let held = latches.removeValue(forKey: event.keyIdentity) else { return false }
            switch held {
            case .handled, .mute: return true
            case .passThrough: return false
            }
        case .down:
            if !event.isRepeat { latches.removeValue(forKey: event.keyIdentity) }
            if let held = latches[event.keyIdentity] {
                // Repeats of a held mute key are consumed without toggling again.
                if case let .mute(monitorID, generation, cancelled) = held {
                    if !cancelled,
                       let display = displays.first(where: { $0.monitorID == monitorID && $0.generation == generation }),
                       !display.isBuiltin,
                       HardwareKeyPolicy.uidsMatch(display.audioDeviceUID, defaultOutputUID),
                       display.volume.supported, display.volume.awake,
                       valueFor(display, .volume) != nil {
                        return true
                    }
                    latches[event.keyIdentity] = .mute(monitorID: monitorID, generation: generation, cancelled: true)
                    return true
                }
                guard case let .handled(monitorID, generation, feature, direction, fine, cancelled) = held else { return false }
                if !cancelled,
                   let display = displays.first(where: { $0.monitorID == monitorID && $0.generation == generation }),
                   !display.isBuiltin,
                   feature != .volume || HardwareKeyPolicy.uidsMatch(display.audioDeviceUID, defaultOutputUID),
                   display.state(for: feature).supported,
                   display.state(for: feature).awake,
                   let value = valueFor(display, feature) {
                    _ = reserve(display, feature, direction, fine, value)
                } else {
                    latches[event.keyIdentity] = .handled(monitorID: monitorID, generation: generation,
                                                          feature: feature, direction: direction, fine: fine, cancelled: true)
                }
                return true
            }
            guard !event.isRepeat, enabled, event.modifiersEligible,
                  let display = event.feature == .volume
                      ? HardwareKeyPolicy.volumeTarget(defaultOutputUID: defaultOutputUID, displays: displays)
                      : HardwareKeyPolicy.target(at: pointer, displays: displays, feature: event.feature),
                  let monitorID = display.monitorID, let generation = display.generation,
                  let value = valueFor(display, event.feature),
                  event.isMute ? reserveMute(display, value)
                               : reserve(display, event.feature, event.direction, event.isFineStep, value) else {
                latches[event.keyIdentity] = .passThrough
                return false
            }
            if event.isMute {
                latches[event.keyIdentity] = .mute(monitorID: monitorID, generation: generation, cancelled: false)
                return true
            }
            latches[event.keyIdentity] = .handled(monitorID: monitorID, generation: generation,
                                                  feature: event.feature, direction: event.direction,
                                                  fine: event.isFineStep, cancelled: false)
            return true
        }
    }
}

private struct HardwareKeyIntentKey: Hashable {
    let monitorID: String
    let generation: UInt64
    let feature: HardwareKeyFeature
}

private struct HardwareKeyIntent {
    var target: Double
    var revision: UInt64
    let authorizationEpoch: UInt64
    var manualRevision: UInt64?
    var lastObserved: Double?
    var scheduled = false
}

/// The router owns a tiny event callback and a bounded, coalescing intent worker.
/// Core/DDC submission is asynchronous and is never called from the event tap callback.
final class HardwareKeyRouter: ObservableObject {
    typealias Submit = (HardwareKeyCommand, @escaping (Bool) -> Void) -> Void
    typealias TapInstaller = () -> (CFMachPort, CFRunLoopSource)?
    /// After one of our hardware writes completes, the readback may still lag; rebases wait this long (ms).
    static let rebaseSettleMs: UInt64 = 1_000
    /// An accepted write whose completion never arrives stops blocking rebases after this long (ms).
    static let outstandingExpiryMs: UInt64 = 10_000

    @Published private(set) var status: HardwareKeyRouterStatus = .disabled
    var onStatusChange: ((HardwareKeyRouterStatus) -> Void)?
    /// Always invoked on the main queue, once on the first evaluation and then only when the value changes.
    var onPermissionsChange: ((HardwareKeyPermissions) -> Void)?
    var onReservedTarget: ((HardwareKeyCommand) -> Void)?

    private let submit: Submit
    private let permissionProvider: () -> HardwareKeyPermissions
    private let customTapInstaller: TapInstaller?
    private let stateLock = NSLock()
    private let workerQueue = DispatchQueue(label: "com.codebit.telaviva.hardware-key-intents", qos: .utility)
    private var enabled = false
    private var stopped = false
    private var displays: [HardwareKeyDisplay] = []
    private var defaultOutputUID: String?
    /// Configured whole-point steps; written by setSteps and read in reserveStep under stateLock.
    private var brightnessStep = HardwareKeyPolicy.defaultStep
    private var volumeStep = HardwareKeyPolicy.defaultStep
    private var sequence = HardwareKeySequence()
    private var intents: [HardwareKeyIntentKey: HardwareKeyIntent] = [:]
    /// Volume level to restore on the next mute press; in memory only and bounded to 128 entries.
    private var muteMemory: [HardwareKeyIntentKey: Double] = [:]
    private var inFlightKeys = Set<HardwareKeyIntentKey>()
    /// Our accepted hardware writes per key (actionGeneration -> acceptance time in ms); bounded to 128 in total.
    private var outstandingActions: [HardwareKeyIntentKey: [UInt64: UInt64]] = [:]
    /// Rebases of a key stay blocked until this time in ms; set when one of our writes for that key completes.
    private var rebaseBlockedUntil: [HardwareKeyIntentKey: UInt64] = [:]
    /// Completions that arrived before their acceptance was registered; bounded to 64 entries.
    private var earlyCompleted: [HardwareKeyIntentKey: Set<UInt64>] = [:]
    /// Millisecond clock, injected by tests so that the settle window and expiry need no real sleeping.
    private let nowMs: () -> UInt64
    private var intentRevision: UInt64 = 0
    private var tap: CFMachPort?
    private var tapSource: CFRunLoopSource?
    private var tapReenableAttempted = false
    private var tapCreationFailed = false
    private var lastPermissions: HardwareKeyPermissions?

    init(submit: @escaping Submit,
         permissionProvider: @escaping () -> HardwareKeyPermissions = HardwareKeyPermissions.current,
         tapInstaller: TapInstaller? = nil,
         nowMs: @escaping () -> UInt64 = { DispatchTime.now().uptimeNanoseconds / 1_000_000 }) {
        self.submit = submit
        self.permissionProvider = permissionProvider
        self.customTapInstaller = tapInstaller
        self.nowMs = nowMs
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
        reconcileDisplaysLocked(nextDisplays.count <= 64 ? nextDisplays : [])
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

    /// Stores the default sound output UID read off the event-tap path; nil means pass-through.
    func setDefaultOutput(uid: String?) {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard !stopped else { return }
        let previous = defaultOutputUID
        defaultOutputUID = uid
        if uid != previous { muteMemory.removeAll() } // Remembered levels belong to the previous sound output.
    }

    /// Stores the configured key steps, clamped to whole points in 1...15 (non-finite falls back to the default).
    /// Intents and the mute memory are kept, so a change applies from the next press onward.
    func setSteps(brightness: Double, volume: Double) {
        stateLock.lock()
        defer { stateLock.unlock() }
        guard !stopped else { return }
        brightnessStep = HardwareKeyPolicy.clampedStep(brightness)
        volumeStep = HardwareKeyPolicy.clampedStep(volume)
    }

    /// Call immediately before the matching manual core command is submitted.
    func manualControlChanged(monitorID: String, generation: UInt64, feature: HardwareKeyFeature, value: Double, revision: UInt64) {
        guard value.isFinite, (0...100).contains(value) else { return }
        stateLock.lock()
        defer { stateLock.unlock() }
        guard enabled, !stopped else { return }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        let observed = displays.first { $0.monitorID == monitorID && $0.generation == generation }?.state(for: feature).value
        intentRevision &+= 1
        if intents[key] != nil || intents.count < 128 {
            // Replacing the authorization epoch also invalidates a command already handed to DDC.
            intents[key] = HardwareKeyIntent(target: value, revision: intentRevision,
                                            authorizationEpoch: intentRevision, manualRevision: revision,
                                            lastObserved: observed)
        }
    }

    /// An asynchronous acceptance may rebase only the exact slider intent that created it.
    func manualControlAccepted(monitorID: String, generation: UInt64, feature: HardwareKeyFeature,
                               value: Double, revision: UInt64) {
        stateLock.lock()
        defer { stateLock.unlock() }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        guard var intent = intents[key], intent.manualRevision == revision,
              intent.target == value else { return }
        intent.lastObserved = displays.first { $0.monitorID == monitorID && $0.generation == generation }?.state(for: feature).value
        intents[key] = intent
    }

    /// Called when the core accepts one of our hardware writes. Until that write completes, the readback of
    /// its key is our own write in progress, so it must not rebase the key's intent.
    func hardwareActionAccepted(monitorID: String, generation: UInt64, feature: HardwareKeyFeature, actionGeneration: UInt64) {
        stateLock.lock()
        defer { stateLock.unlock() }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        // The completion arrived first, so this acceptance is already settled and registers nothing.
        if earlyCompleted[key]?.remove(actionGeneration) != nil {
            if earlyCompleted[key]?.isEmpty == true { earlyCompleted.removeValue(forKey: key) }
            return
        }
        registerOutstandingLocked(key, actionGeneration: actionGeneration, now: nowMs())
    }

    /// Called when the core reports that one of our hardware writes finished, successfully or not. The readback
    /// can still lag behind the final value for a moment, so rebases stay blocked for a short settle window.
    func hardwareActionCompleted(monitorID: String, generation: UInt64, feature: HardwareKeyFeature, actionGeneration: UInt64) {
        stateLock.lock()
        defer { stateLock.unlock() }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        if outstandingActions[key]?.removeValue(forKey: actionGeneration) != nil {
            if outstandingActions[key]?.isEmpty == true { outstandingActions.removeValue(forKey: key) }
        } else {
            // The acceptance has not been registered yet; remember this completion so that it cancels the acceptance.
            if earlyCompleted.values.reduce(0, { $0 + $1.count }) >= 64 { earlyCompleted.removeAll() }
            earlyCompleted[key, default: []].insert(actionGeneration)
        }
        rebaseBlockedUntil[key] = nowMs() + HardwareKeyRouter.rebaseSettleMs
    }

    /// Records an acceptance. At the 128-entry bound the oldest acceptance is dropped: first for this key, then overall.
    private func registerOutstandingLocked(_ key: HardwareKeyIntentKey, actionGeneration: UInt64, now: UInt64) {
        if (outstandingActions[key]?.count ?? 0) >= 128 { dropOldestOutstandingLocked(in: key) }
        if outstandingActions.values.reduce(0, { $0 + $1.count }) >= 128 { dropOldestOutstandingLocked(in: nil) }
        outstandingActions[key, default: [:]][actionGeneration] = now
    }

    private func dropOldestOutstandingLocked(in key: HardwareKeyIntentKey?) {
        var oldest: (key: HardwareKeyIntentKey, action: UInt64, at: UInt64)?
        for (candidate, actions) in outstandingActions where key == nil || key == candidate {
            for (action, at) in actions where at < (oldest?.at ?? UInt64.max) {
                oldest = (candidate, action, at)
            }
        }
        guard let oldest else { return }
        outstandingActions[oldest.key]?.removeValue(forKey: oldest.action)
        if outstandingActions[oldest.key]?.isEmpty == true { outstandingActions.removeValue(forKey: oldest.key) }
    }

    func isCommandCurrent(_ command: HardwareKeyCommand) -> Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        let key = HardwareKeyIntentKey(monitorID: command.monitorID, generation: command.generation, feature: command.feature)
        guard enabled, !stopped, let intent = intents[key],
              intent.authorizationEpoch == command.authorizationEpoch,
              intent.revision == command.intentRevision, intent.target == command.target,
              let display = displays.first(where: { $0.monitorID == key.monitorID && $0.generation == key.generation }) else { return false }
        let state = display.state(for: key.feature)
        return !display.isBuiltin && state.supported && state.awake && !state.failed
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

    /// Deterministic seam used by the helper tests; it runs the same sequence and intent path
    /// without creating an event tap, prompting for permission, or touching a physical display.
    @discardableResult
    func processForTesting(_ event: HardwareKeyEvent, pointer: CGPoint,
                           displays nextDisplays: [HardwareKeyDisplay]) -> Bool {
        stateLock.lock()
        guard !stopped else { stateLock.unlock(); return false }
        enabled = true
        reconcileDisplaysLocked(nextDisplays.count <= 64 ? nextDisplays : [])
        let handled = sequence.process(event, pointer: pointer, enabled: true, defaultOutputUID: defaultOutputUID,
                                       displays: displays,
                                       valueFor: { [unowned self] display, feature in self.currentIntentValue(display: display, feature: feature) },
                                       reserve: { [unowned self] display, feature, direction, fine, value in
                                           self.reserveStep(display: display, feature: feature, direction: direction, fine: fine, value: value)
                                       },
                                       reserveMute: { [unowned self] display, value in self.reserveMute(display: display, value: value) })
        stateLock.unlock()
        return handled
    }

    func waitForWorkerForTesting() { workerQueue.sync {} }

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
        sequence.clear()
        intents.removeAll(keepingCapacity: true)
        outstandingActions.removeAll(keepingCapacity: true)
        rebaseBlockedUntil.removeAll(keepingCapacity: true)
        earlyCompleted.removeAll(keepingCapacity: true)
        muteMemory.removeAll(keepingCapacity: true)
        if clearOutput { defaultOutputUID = nil }
    }

    /// Drops acceptances older than outstandingExpiryMs, settle windows that have ended, and state of displays that are gone.
    private func pruneRebaseStateLocked(now: UInt64) {
        for key in Array(outstandingActions.keys) {
            let live = (outstandingActions[key] ?? [:]).filter { now <= $0.value + HardwareKeyRouter.outstandingExpiryMs }
            outstandingActions[key] = live.isEmpty ? nil : live
        }
        rebaseBlockedUntil = rebaseBlockedUntil.filter { $0.value > now }
        let keys = Set(outstandingActions.keys).union(rebaseBlockedUntil.keys).union(earlyCompleted.keys)
        for key in keys where !displays.contains(where: { $0.monitorID == key.monitorID && $0.generation == key.generation }) {
            forgetRebaseStateLocked(key)
        }
    }

    private func forgetRebaseStateLocked(_ key: HardwareKeyIntentKey) {
        outstandingActions.removeValue(forKey: key)
        rebaseBlockedUntil.removeValue(forKey: key)
        earlyCompleted.removeValue(forKey: key)
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
        // This mutex protects only bounded value copies/intent updates; no core or device call
        // holds it. A handled key-up must never leak to macOS due to snapshot contention.
        stateLock.lock()
        let result = sequence.process(decoded, pointer: pointer, enabled: enabled, defaultOutputUID: defaultOutputUID,
                                      displays: displays,
                                      valueFor: { [unowned self] display, feature in self.currentIntentValue(display: display, feature: feature) },
                                      reserve: { [unowned self] display, feature, direction, fine, value in
                                          self.reserveStep(display: display, feature: feature, direction: direction, fine: fine, value: value)
                                      },
                                      reserveMute: { [unowned self] display, value in self.reserveMute(display: display, value: value) })
        stateLock.unlock()
        return result ? nil : Unmanaged.passUnretained(event)
    }

    private func reconcileDisplaysLocked(_ next: [HardwareKeyDisplay]) {
        displays = next
        let now = nowMs()
        pruneRebaseStateLocked(now: now)
        // A remembered level survives only while its exact monitor and generation stay eligible.
        for key in Array(muteMemory.keys) {
            guard let display = displays.first(where: { $0.monitorID == key.monitorID && $0.generation == key.generation }),
                  !display.isBuiltin else { muteMemory.removeValue(forKey: key); continue }
            let state = display.state(for: key.feature)
            if !(state.supported && state.awake && !state.failed && (state.value?.isFinite ?? false)) {
                muteMemory.removeValue(forKey: key)
            }
        }
        for key in Array(intents.keys) {
            guard let display = displays.first(where: { $0.monitorID == key.monitorID && $0.generation == key.generation }),
                  !display.isBuiltin else { intents.removeValue(forKey: key); forgetRebaseStateLocked(key); continue }
            let state = display.state(for: key.feature)
            guard state.supported, state.awake, !state.failed, let observed = state.value, observed.isFinite else {
                intents.removeValue(forKey: key)
                forgetRebaseStateLocked(key)
                continue
            }
            guard var intent = intents[key], !intent.scheduled, !state.pending else { continue }
            // While our own write is outstanding or settling, the readback is not an external change: leave the intent and lastObserved alone.
            let outstanding = outstandingActions[key]?.count ?? 0
            let blockedUntil = rebaseBlockedUntil[key] ?? 0
            if outstanding > 0 || now < blockedUntil {
                continue
            }
            if let previous = intent.lastObserved, observed != previous,
               abs(intent.target - observed) > state.tolerance {
                intentRevision &+= 1
                intents[key] = HardwareKeyIntent(target: observed, revision: intentRevision,
                                                authorizationEpoch: intentRevision, manualRevision: nil,
                                                lastObserved: observed)
            } else {
                intent.lastObserved = observed
                intents[key] = intent
            }
        }
    }

    private func currentIntentValue(display: HardwareKeyDisplay, feature: HardwareKeyFeature) -> Double? {
        guard let monitorID = display.monitorID, let generation = display.generation else { return nil }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        if let intent = intents[key] { return intent.target }
        return display.state(for: feature).value
    }

    private func reserveStep(display: HardwareKeyDisplay, feature: HardwareKeyFeature,
                             direction: HardwareKeyEvent.Direction, fine: Bool, value: Double) -> Bool {
        guard let monitorID = display.monitorID, let generation = display.generation else { return false }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        // Read directly: the tap callback already holds stateLock here.
        let step = feature == .volume ? volumeStep : brightnessStep
        if feature == .volume, let remembered = muteMemory[key] {
            if value != 0 {
                muteMemory.removeValue(forKey: key) // Changed elsewhere since the mute.
            } else if direction == .decrease {
                return true // Still muted: consume without a write and keep the level.
            } else {
                // Restore the remembered level, then step up from it, as one coalesced write.
                let next = HardwareKeyPolicy.stepped(remembered, direction: direction, fine: fine, step: step)
                guard reserveWrite(display: display, feature: feature, value: value, next: next) else { return false }
                muteMemory.removeValue(forKey: key)
                return true
            }
        }
        return reserveWrite(display: display, feature: feature, value: value,
                            next: HardwareKeyPolicy.stepped(value, direction: direction, fine: fine, step: step))
    }

    /// Initial mute press: mute remembers the level and writes 0; the next press restores it.
    private func reserveMute(display: HardwareKeyDisplay, value: Double) -> Bool {
        guard let monitorID = display.monitorID, let generation = display.generation else { return false }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: .volume)
        if value != 0 { muteMemory.removeValue(forKey: key) } // Stale memory when the level changed elsewhere.
        if value > 0 {
            guard muteMemory.count < 128,
                  reserveWrite(display: display, feature: .volume, value: value, next: 0) else { return false }
            muteMemory[key] = value
            return true
        }
        guard let remembered = muteMemory[key] else { return true } // At 0 with nothing to restore.
        guard reserveWrite(display: display, feature: .volume, value: value, next: remembered) else { return false }
        muteMemory.removeValue(forKey: key)
        return true
    }

    private func reserveWrite(display: HardwareKeyDisplay, feature: HardwareKeyFeature,
                              value: Double, next: Double) -> Bool {
        guard let monitorID = display.monitorID, let generation = display.generation else { return false }
        let key = HardwareKeyIntentKey(monitorID: monitorID, generation: generation, feature: feature)
        guard intents[key] != nil || intents.count < 128 else { return false }
        guard inFlightKeys.contains(key) || inFlightKeys.count < 128 else { return false }
        guard next != value else { return true }
        intentRevision &+= 1
        var intent = intents[key] ?? HardwareKeyIntent(target: value, revision: intentRevision,
                                                      authorizationEpoch: intentRevision, manualRevision: nil,
                                                      lastObserved: display.state(for: feature).value)
        intent.target = next
        intent.revision = intentRevision
        let needsWorker = !intent.scheduled && !inFlightKeys.contains(key)
        intent.scheduled = true
        intents[key] = intent
        let reserved = HardwareKeyCommand(monitorID: monitorID, generation: generation, feature: feature,
                                          target: next, intentRevision: intent.revision,
                                          authorizationEpoch: intent.authorizationEpoch)
        workerQueue.async { [weak self] in self?.onReservedTarget?(reserved) }
        if needsWorker { workerQueue.async { [weak self] in self?.flush(key) } }
        return true
    }

    private func flush(_ key: HardwareKeyIntentKey) {
        stateLock.lock()
        guard enabled, let intent = intents[key],
              let display = displays.first(where: { $0.monitorID == key.monitorID && $0.generation == key.generation }),
              !display.isBuiltin, display.state(for: key.feature).supported, display.state(for: key.feature).awake else {
            intents.removeValue(forKey: key)
            stateLock.unlock()
            return
        }
        guard !inFlightKeys.contains(key), inFlightKeys.count < 128 else { stateLock.unlock(); return }
        inFlightKeys.insert(key)
        let command = HardwareKeyCommand(monitorID: key.monitorID, generation: key.generation,
                                         feature: key.feature, target: intent.target,
                                         intentRevision: intent.revision,
                                         authorizationEpoch: intent.authorizationEpoch)
        stateLock.unlock()
        submit(command) { [weak self] accepted in self?.workerQueue.async { self?.complete(key, revision: command.intentRevision, epoch: command.authorizationEpoch, accepted: accepted) } }
    }

    private func complete(_ key: HardwareKeyIntentKey, revision: UInt64, epoch: UInt64, accepted: Bool) {
        stateLock.lock()
        inFlightKeys.remove(key)
        guard var intent = intents[key] else { stateLock.unlock(); return }
        if intent.authorizationEpoch != epoch {
            let shouldFlush = enabled && !stopped && intent.scheduled
            stateLock.unlock()
            if shouldFlush { flush(key) }
            return
        }
        guard accepted || intent.revision != revision else {
            // The consumed event stays consumed; a later key starts from the latest published snapshot.
            intents.removeValue(forKey: key)
            stateLock.unlock()
            return
        }
        if intent.revision != revision {
            intent.scheduled = true
            intents[key] = intent
            stateLock.unlock()
            flush(key)
        } else {
            intent.scheduled = false
            intents[key] = intent
            stateLock.unlock()
        }
    }

    private static let eventTapCallback: CGEventTapCallBack = { _, type, event, userInfo in
        guard let userInfo else { return Unmanaged.passUnretained(event) }
        let router = Unmanaged<HardwareKeyRouter>.fromOpaque(userInfo).takeUnretainedValue()
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput { event.type = type }
        return router.handle(event)
    }
}
