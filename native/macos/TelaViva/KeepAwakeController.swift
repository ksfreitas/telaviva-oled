import AppKit
import ApplicationServices
import CoreGraphics
import Foundation
import IOKit
import IOKit.pwr_mgt
import ObjectiveC.runtime

enum KeepAwakeSafety: Equatable {
    case eligible
    case sessionInactive
    case sessionStateUnknown
    case screensaverActive
    case screensaverStateUnknown
    case displayAsleep
    case systemSleeping
    case lidClosed

    static func resolve(sessionActive: Bool, sessionLocked: Bool?, screensaverActive: Bool?,
                        displayAsleep: Bool, systemSleeping: Bool) -> KeepAwakeSafety {
        if systemSleeping { return .systemSleeping }
        if !sessionActive || sessionLocked == true { return .sessionInactive }
        guard sessionLocked == false else { return .sessionStateUnknown }
        if displayAsleep { return .displayAsleep }
        if screensaverActive == true { return .screensaverActive }
        guard screensaverActive == false else { return .screensaverStateUnknown }
        return .eligible
    }

    /// Camera demand follows the unlocked, awake session. The inhibitor has stricter gates below.
    static func cameraMonitoringAllowed(for safety: KeepAwakeSafety) -> Bool {
        switch safety {
        case .eligible, .screensaverActive, .screensaverStateUnknown, .displayAsleep, .lidClosed:
            return true
        case .sessionInactive, .sessionStateUnknown, .systemSleeping:
            return false
        }
    }
}

enum KeepAwakeStatus: Equatable {
    case disabled
    case awaitingPresence
    case active
    case permissionRequired
    case unavailable
    case suspended
}

enum KeepAwakeSessionReconciliation {
    /// The core's generic suspended state means its session gate is inactive. Re-report local state
    /// only for that exact disagreement; other suspension reasons must not cause repeated commands.
    static func sessionValueToResubmit(keepAwakeEnabled: Bool, cameraMode: String?, displayStatus: String?,
                                       localMonitoringAllowed: Bool) -> Bool? {
        guard keepAwakeEnabled, localMonitoringAllowed,
              cameraMode == "suspended", displayStatus == "suspended" else { return nil }
        return localMonitoringAllowed
    }
}

protocol KeepAwakeCancelable: AnyObject { func cancel() }

protocol KeepAwakeScheduling: AnyObject {
    func repeating(every interval: TimeInterval, _ action: @escaping () -> Void) -> KeepAwakeCancelable
    func once(after delay: TimeInterval, _ action: @escaping () -> Void) -> KeepAwakeCancelable
}

protocol KeepAwakeBackend: AnyObject {
    func safetyState() -> KeepAwakeSafety
    func hasPostingPermission() -> Bool
    /// Called only from the explicit user action in Settings.
    func requestPostingPermission() -> Bool
    func acquireAssertions() -> Bool
    func releaseAssertions()
    func postActivity() -> Bool
}

/// Serializes the temporary OS effects and owns a monotonic watchdog independent of SwiftUI snapshots.
/// Backend and scheduler are injectable so expiry, permissions, manual transitions and cleanup can be tested
/// without posting real input or acquiring system assertions.
final class KeepAwakeController {
    private struct Token: Equatable, Comparable {
        let generation: UInt64
        let observationID: UInt64
        static func < (lhs: Token, rhs: Token) -> Bool {
            lhs.generation == rhs.generation
                ? lhs.observationID < rhs.observationID
                : lhs.generation < rhs.generation
        }
    }

    private let backend: KeepAwakeBackend
    private let scheduler: KeepAwakeScheduling
    private let now: () -> TimeInterval
    private let wallNowMilliseconds: () -> UInt64
    private let lock = NSRecursiveLock()
    private var pulseWorker: KeepAwakeCancelable?
    private var expiryWorker: KeepAwakeCancelable?
    private var enabled = false
    private var sessionEligible = false
    private var latestToken: Token?
    private var revokedThrough: Token?
    private var requiredGeneration: UInt64?
    private var highestObservedGeneration: UInt64 = 0
    private var sessionWasInactive = true
    private var safetyVetoActive = false
    private var hasReportedSessionState = false
    private var sessionVetoGeneration: UInt64?
    private var wasEnabledAtVeto = false
    // Core observation_id is the captured_at_ms timestamp for accepted positive observations.
    private var requiredObservationAfterMilliseconds: UInt64?
    private var highestObservationTimestampMilliseconds: UInt64 = 0
    private var deadline: TimeInterval?
    private var assertionsHeld = false
    private var stopped = false
    private(set) var status: KeepAwakeStatus = .disabled
    var onStatusChange: ((KeepAwakeStatus) -> Void)?

    init(backend: KeepAwakeBackend = MacKeepAwakeBackend(),
         scheduler: KeepAwakeScheduling = DispatchKeepAwakeScheduler(),
         now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         wallNowMilliseconds: @escaping () -> UInt64 = { UInt64(Date().timeIntervalSince1970 * 1_000) }) {
        self.backend = backend
        self.scheduler = scheduler
        self.now = now
        self.wallNowMilliseconds = wallNowMilliseconds
    }

    /// A true session report permits camera monitoring, but only evidence from a generation newer than
    /// the last suspended generation can reactivate the native effect.
    func setSessionEligible(_ eligible: Bool) {
        lock.lock(); defer { lock.unlock() }
        guard !stopped, sessionEligible != eligible else { return }
        sessionEligible = eligible
        if !eligible {
            wasEnabledAtVeto = enabled
            sessionVetoGeneration = highestObservedGeneration
            if enabled { requireNextGeneration() } else { requiredGeneration = nil }
            sessionWasInactive = true
            safetyVetoActive = true
            hasReportedSessionState = true
            revokeCurrentToken()
            revokeLease()
        } else if sessionWasInactive {
            if enabled {
                if hasReportedSessionState {
                    requireGenerationAfterResume(from: sessionVetoGeneration ?? highestObservedGeneration)
                } else {
                    requireNextGeneration()
                }
            } else {
                requiredGeneration = nil
            }
            sessionVetoGeneration = nil
            wasEnabledAtVeto = false
            sessionWasInactive = false
            hasReportedSessionState = true
        }
        reconcile(postImmediately: false)
    }

    /// Snapshot TTL is remaining time. Repeated delivery of the same token can only shorten its local deadline.
    func update(enabled nextEnabled: Bool, requested: Bool, generation: UInt64,
                observationID: UInt64, validForMilliseconds: UInt64) {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        enabled = nextEnabled
        highestObservedGeneration = max(highestObservedGeneration, generation)
        highestObservationTimestampMilliseconds = max(highestObservationTimestampMilliseconds, observationID)
        let token = Token(generation: generation, observationID: observationID)
        guard enabled, requested, validForMilliseconds > 0 else {
            revokeCurrentToken()
            revokeLease()
            reconcile(postImmediately: false)
            return
        }

        guard sessionEligible,
              token.generation >= highestObservedGeneration,
              requiredGeneration.map({ token.generation >= $0 }) ?? true,
              requiredObservationAfterMilliseconds.map({ token.observationID > $0 }) ?? true,
              token > (revokedThrough ?? Token(generation: 0, observationID: 0)) else {
            reconcile(postImmediately: false)
            return
        }
        if let current = latestToken {
            guard token >= current else { reconcile(postImmediately: false); return }
            if token == current {
                guard let deadline else { reconcile(postImmediately: false); return }
                let shortened = min(deadline, now() + min(Double(validForMilliseconds) / 1_000, 315))
                self.deadline = shortened
                scheduleExpiry(for: current, at: shortened)
                reconcile(postImmediately: false)
                return
            }
        }
        latestToken = token
        let duration = min(Double(validForMilliseconds) / 1_000, 315)
        deadline = now() + duration
        scheduleExpiry(for: token, at: deadline!)
        reconcile(postImmediately: true)
    }

    /// Updates a local safety veto synchronously; a queued snapshot cannot make an unsafe pulse eligible.
    func safetyDidChange() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        reconcile(postImmediately: false)
    }

    func requestPostingPermissionFromUserAction() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        _ = backend.requestPostingPermission()
        reconcile(postImmediately: true)
    }

    func shutdown() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        stopped = true
        cancelWorkers()
        revokeLease()
        publish(.disabled)
    }

    private func tick() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        pulse()
    }

    private func reconcile(postImmediately: Bool) {
        guard enabled else {
            revokeLease()
            publish(.disabled)
            return
        }
        guard sessionEligible, backend.safetyState() == .eligible else {
            enterSafetyVeto()
            revokeCurrentToken()
            revokeLease()
            publish(.suspended)
            return
        }
        leaveSafetyVeto()
        guard let deadline, deadline > now(), latestToken != nil else {
            if deadline != nil { revokeCurrentToken(); revokeLease() }
            publish(.awaitingPresence)
            return
        }
        guard backend.hasPostingPermission() else {
            stopEffects()
            publish(.permissionRequired)
            return
        }
        if postImmediately || !assertionsHeld { pulse() }
    }

    private var lastPulseAt: TimeInterval?

    private func acquireAssertions() -> Bool { backend.acquireAssertions() }

    private func revokeLease() {
        cancelWorkers()
        deadline = nil
        latestToken = nil
        lastPulseAt = nil
        if assertionsHeld {
            assertionsHeld = false
            backend.releaseAssertions()
        }
    }

    private func revokeCurrentToken() {
        if let latestToken { revokedThrough = max(revokedThrough ?? latestToken, latestToken) }
    }

    private func requireNextGeneration() {
        let observed = max(highestObservedGeneration, latestToken?.generation ?? 0)
        guard observed < UInt64.max else { return }
        requiredGeneration = max(requiredGeneration ?? 0, observed + 1)
    }

    private func enterSafetyVeto() {
        guard !safetyVetoActive else { return }
        safetyVetoActive = true
    }

    private func leaveSafetyVeto() {
        guard safetyVetoActive else { return }
        requiredObservationAfterMilliseconds = max(
            requiredObservationAfterMilliseconds ?? 0,
            wallNowMilliseconds(),
            highestObservationTimestampMilliseconds
        )
        safetyVetoActive = false
    }

    private func requireGenerationAfterResume(from baseline: UInt64) {
        let observed = max(baseline, latestToken?.generation ?? 0)
        guard observed < UInt64.max - 1 else { return }
        let increment: UInt64 = wasEnabledAtVeto ? 2 : 1
        requiredGeneration = max(requiredGeneration ?? 0, observed + increment)
    }

    private func publish(_ next: KeepAwakeStatus) {
        guard status != next else { return }
        status = next
        onStatusChange?(next)
    }

    private func pulse() {
        guard !stopped else { return }
        guard enabled else { revokeLease(); publish(.disabled); return }
        guard sessionEligible, backend.safetyState() == .eligible else {
            enterSafetyVeto(); revokeCurrentToken(); revokeLease(); publish(.suspended); return
        }
        leaveSafetyVeto()
        guard let deadline, deadline > now(), latestToken != nil else {
            if deadline != nil { revokeCurrentToken() }
            revokeLease(); publish(.awaitingPresence); return
        }
        guard backend.hasPostingPermission() else {
            stopEffects(); publish(.permissionRequired); return
        }
        guard assertionsHeld || acquireAssertions() else {
            stopEffects(); publish(.unavailable); return
        }
        assertionsHeld = true
        guard backend.safetyState() == .eligible, self.deadline.map({ $0 > now() }) == true else {
            enterSafetyVeto(); revokeCurrentToken(); revokeLease(); publish(.suspended); return
        }
        guard backend.postActivity() else {
            stopEffects(); publish(.unavailable); return
        }
        lastPulseAt = now()
        startPulseWorker()
        publish(.active)
    }

    private func startPulseWorker() {
        guard pulseWorker == nil else { return }
        pulseWorker = scheduler.repeating(every: 1) { [weak self] in self?.tick() }
    }

    private func scheduleExpiry(for token: Token, at due: TimeInterval) {
        expiryWorker?.cancel()
        expiryWorker = scheduler.once(after: max(0, due - now())) { [weak self] in
            guard let self else { return }
            self.lock.lock(); defer { self.lock.unlock() }
            guard !self.stopped, self.latestToken == token, let currentDeadline = self.deadline else { return }
            if currentDeadline > self.now() {
                self.scheduleExpiry(for: token, at: currentDeadline)
                return
            }
            self.revokeCurrentToken()
            self.revokeLease()
            self.publish(self.enabled ? .awaitingPresence : .disabled)
        }
    }

    private func stopEffects() {
        pulseWorker?.cancel(); pulseWorker = nil
        lastPulseAt = nil
        if assertionsHeld { assertionsHeld = false; backend.releaseAssertions() }
    }

    private func cancelWorkers() {
        pulseWorker?.cancel(); pulseWorker = nil
        expiryWorker?.cancel(); expiryWorker = nil
    }
}

private final class DispatchKeepAwakeScheduler: KeepAwakeScheduling {
    func repeating(every interval: TimeInterval, _ action: @escaping () -> Void) -> KeepAwakeCancelable {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "com.telaviva.keep-awake-worker", qos: .utility))
        timer.schedule(deadline: .now() + interval, repeating: interval, leeway: .milliseconds(100))
        timer.setEventHandler(handler: action)
        timer.resume()
        return DispatchKeepAwakeToken(timer: timer)
    }

    func once(after delay: TimeInterval, _ action: @escaping () -> Void) -> KeepAwakeCancelable {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "com.telaviva.keep-awake-expiry", qos: .utility))
        timer.schedule(deadline: .now() + delay, leeway: .milliseconds(10))
        timer.setEventHandler(handler: action)
        timer.resume()
        return DispatchKeepAwakeToken(timer: timer)
    }
}

private final class DispatchKeepAwakeToken: KeepAwakeCancelable {
    private let lock = NSLock()
    private var timer: DispatchSourceTimer?
    init(timer: DispatchSourceTimer) { self.timer = timer }
    func cancel() {
        lock.lock(); defer { lock.unlock() }
        timer?.setEventHandler {}
        timer?.cancel()
        timer = nil
    }
}

/// macOS energy assertions plus a zero-displacement mouse event. The latter's efficacy against
/// screensavers and automatic locks is a physical acceptance question, not implied by a successful post.
final class MacKeepAwakeBackend: KeepAwakeBackend {
    private let lock = NSLock()
    private var displayAssertion: IOPMAssertionID = 0
    private var systemAssertion: IOPMAssertionID = 0
    private var saverOverride: Bool?
    private var displayAsleep = false
    private var systemSleeping = false
    private var lidIsClosed = false
    private var sessionIsActive = true
    private var lockStateOverride: Bool?

    func setScreensaverActive(_ active: Bool) { lock.lock(); saverOverride = active; lock.unlock() }
    func setDisplayAsleep(_ asleep: Bool) { lock.lock(); displayAsleep = asleep; lock.unlock() }
    func setSystemSleeping(_ sleeping: Bool) { lock.lock(); systemSleeping = sleeping; lock.unlock() }
    func setLidClosed(_ closed: Bool) { lock.lock(); lidIsClosed = closed; lock.unlock() }
    func setSessionActive(_ active: Bool) { lock.lock(); sessionIsActive = active; lock.unlock() }
    func setSessionLocked(_ locked: Bool) { lock.lock(); lockStateOverride = locked; lock.unlock() }

    func safetyState() -> KeepAwakeSafety {
        lock.lock(); defer { lock.unlock() }
        let locked = Self.resolveGuardState(override: lockStateOverride, live: Self.sessionLockState)
        let saverActive = Self.resolveGuardState(override: saverOverride, live: Self.screenSaverIsRunning)
        // Lid state is logged but does not veto an awake clamshell session: the camera driver
        // excludes the built-in camera and can select an external camera instead.
        return Self.resolveSafetyState(sessionActive: sessionIsActive, sessionLocked: locked,
                                       screensaverActive: saverActive,
                                       displayAsleep: displayAsleep || Self.anyDisplayAsleep(),
                                       systemSleeping: systemSleeping)
    }

    /// Read-only, local guard evidence for diagnosing why the camera or inhibitor is paused.
    /// This does not post input or acquire power assertions.
    func diagnosticState() -> String {
        lock.lock()
        let sessionActive = sessionIsActive
        let lockOverride = lockStateOverride
        let saverOverride = saverOverride
        let reportedDisplayAsleep = displayAsleep
        let systemSleeping = systemSleeping
        let lidClosed = lidIsClosed
        lock.unlock()
        let lockQuery = Self.sessionLockState()
        let saverQuery = Self.screenSaverIsRunning()
        let locked = Self.resolveGuardState(override: lockOverride, live: { lockQuery })
        let saverActive = Self.resolveGuardState(override: saverOverride, live: { saverQuery })
        let displayIsAsleep = reportedDisplayAsleep || Self.anyDisplayAsleep()
        let safety = Self.resolveSafetyState(sessionActive: sessionActive, sessionLocked: locked,
                                             screensaverActive: saverActive,
                                             displayAsleep: displayIsAsleep,
                                             systemSleeping: systemSleeping)
        let cameraAllowed = KeepAwakeSafety.cameraMonitoringAllowed(for: safety)
        let postingAllowed = CGPreflightPostEventAccess()
        return "session_active=\(sessionActive) session_locked=\(Self.boolText(locked)) " +
            "lock_notification=\(Self.boolText(lockOverride)) lock_query=\(Self.boolText(lockQuery)) " +
            "screensaver_active=\(Self.boolText(saverActive)) saver_notification=\(Self.boolText(saverOverride)) " +
            "saver_query=\(Self.boolText(saverQuery)) display_asleep=\(displayIsAsleep) " +
            "system_sleeping=\(systemSleeping) lid_closed=\(lidClosed) safety=\(Self.safetyText(safety)) " +
            "camera_monitoring=\(cameraAllowed) posting_permission=\(postingAllowed)"
    }

    func hasPostingPermission() -> Bool { CGPreflightPostEventAccess() }

    func requestPostingPermission() -> Bool { CGRequestPostEventAccess() }

    func acquireAssertions() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard displayAssertion == 0, systemAssertion == 0 else { return displayAssertion != 0 && systemAssertion != 0 }
        var display: IOPMAssertionID = 0
        let displayResult = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                                                        IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                                        "TelaViva: face detected" as CFString, &display)
        guard displayResult == kIOReturnSuccess else { return false }
        var system: IOPMAssertionID = 0
        let systemResult = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep as CFString,
                                                       IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                                       "TelaViva: face detected" as CFString, &system)
        guard systemResult == kIOReturnSuccess else {
            IOPMAssertionRelease(display)
            return false
        }
        displayAssertion = display
        systemAssertion = system
        return true
    }

    func releaseAssertions() {
        lock.lock(); defer { lock.unlock() }
        if displayAssertion != 0 { IOPMAssertionRelease(displayAssertion); displayAssertion = 0 }
        if systemAssertion != 0 { IOPMAssertionRelease(systemAssertion); systemAssertion = 0 }
    }

    func postActivity() -> Bool {
        guard safetyState() == .eligible, CGPreflightPostEventAccess() else { return false }
        guard let cursorEvent = CGEvent(source: nil) else { return false }
        let point = cursorEvent.location
        guard let event = CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: point, mouseButton: .left) else {
            return false
        }
        event.post(tap: .cghidEventTap)
        return true
    }

    private static func sessionLockState() -> Bool? {
        guard let raw = CGSessionCopyCurrentDictionary() as? [String: Any],
              let onConsole = raw["kCGSSessionOnConsoleKey"] as? Bool,
              let loginDone = raw["kCGSessionLoginDoneKey"] as? Bool else { return nil }
        guard onConsole, loginDone else { return true }
        if let lockState = raw["CGSSessionScreenIsLocked"] as? Bool { return lockState }
        // IOConsoleLocked is an undocumented IOKit property. Its absence or wrong type fails closed.
        let root = IORegistryGetRootEntry(kIOMainPortDefault)
        guard root != 0 else { return nil }
        defer { IOObjectRelease(root) }
        let value = IORegistryEntryCreateCFProperty(root, "IOConsoleLocked" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
        return value as? Bool
    }

    /// A positive local notification vetoes immediately. A cached false only clears that veto when the
    /// live OS query is known false; stale notifications never substitute for the current guard state.
    static func resolveGuardState(override: Bool?, live: () -> Bool?) -> Bool? {
        if override == true { return true }
        return live()
    }

    static func resolveSafetyState(sessionActive: Bool, sessionLocked: Bool?, screensaverActive: Bool?,
                                   displayAsleep: Bool, systemSleeping: Bool) -> KeepAwakeSafety {
        KeepAwakeSafety.resolve(sessionActive: sessionActive, sessionLocked: sessionLocked,
                                screensaverActive: screensaverActive, displayAsleep: displayAsleep,
                                systemSleeping: systemSleeping)
    }

    private static func boolText(_ value: Bool?) -> String {
        value.map(String.init) ?? "unknown"
    }

    private static func safetyText(_ safety: KeepAwakeSafety) -> String {
        switch safety {
        case .eligible: return "eligible"
        case .sessionInactive: return "session_inactive"
        case .sessionStateUnknown: return "session_state_unknown"
        case .screensaverActive: return "screensaver_active"
        case .screensaverStateUnknown: return "screensaver_state_unknown"
        case .displayAsleep: return "display_asleep"
        case .systemSleeping: return "system_sleeping"
        case .lidClosed: return "lid_closed"
        }
    }

    /// Isolated read-only query through ScreenSaver.framework's runtime selector. This method is not a
    /// documented SDK contract; missing class/selector or an unexpected return type yields unknown.
    private static let screenSaverIsRunning: (() -> Bool?) = { () -> (() -> Bool?) in
        guard Bundle(path: "/System/Library/Frameworks/ScreenSaver.framework")?.load() == true,
              let controllerClass = NSClassFromString("ScreenSaverController") else { return { nil } }
        let factorySelector = NSSelectorFromString("controller")
        let runningSelector = NSSelectorFromString("screenSaverIsRunning")
        guard let factoryMethod = class_getClassMethod(controllerClass, factorySelector),
              let runningMethod = class_getInstanceMethod(controllerClass, runningSelector),
              returnType(factoryMethod) == "@",
              returnType(runningMethod) == "B" else { return { nil } }
        let factory = unsafeBitCast(method_getImplementation(factoryMethod),
                                    to: (@convention(c) (AnyObject, Selector) -> Unmanaged<AnyObject>?).self)
        let isRunning = unsafeBitCast(method_getImplementation(runningMethod),
                                      to: (@convention(c) (AnyObject, Selector) -> Bool).self)
        guard let controller = factory(controllerClass, factorySelector)?.takeUnretainedValue() else { return { nil } }
        return { isRunning(controller, runningSelector) }
    }()

    private static func returnType(_ method: Method) -> String {
        let type = method_copyReturnType(method)
        defer { free(type) }
        return String(cString: type)
    }

    private static func anyDisplayAsleep() -> Bool {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return true }
        var displays = [CGDirectDisplayID](repeating: 0, count: Int(count))
        var actual = count
        guard CGGetActiveDisplayList(count, &displays, &actual) == .success, actual > 0 else { return true }
        return displays.prefix(Int(actual)).contains { CGDisplayIsAsleep($0) != 0 }
    }
}
