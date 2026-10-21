import Foundation
import Combine

/// Crash-recovery record in `builtin-protection-session.json`, written before the first dim write (contracts.md).
struct BuiltInProtectionRecord: Codable, Equatable {
    var version = 1
    var started_at: String
    var baseline_percent: Double
}

/// Reads and writes the single recovery record. Writes are atomic, so a crash never leaves a partial file.
final class BuiltInProtectionStore {
    enum Loaded: Equatable {
        case none
        case record(BuiltInProtectionRecord)
        case unreadable
    }

    private let directory: URL
    private let fileURL: URL

    init(directory: URL) {
        self.directory = directory
        self.fileURL = directory.appendingPathComponent("builtin-protection-session.json")
    }

    /// Returns false when the directory or the file cannot be written.
    func save(_ record: BuiltInProtectionRecord) -> Bool {
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(record)
            try data.write(to: fileURL, options: .atomic)
            return true
        } catch {
            return false
        }
    }

    func load() -> Loaded {
        guard FileManager.default.fileExists(atPath: fileURL.path) else { return .none }
        guard let data = try? Data(contentsOf: fileURL),
              let record = try? JSONDecoder().decode(BuiltInProtectionRecord.self, from: data) else {
            return .unreadable
        }
        return .record(record)
    }

    func delete() {
        try? FileManager.default.removeItem(at: fileURL)
    }
}

/// Applies the core's `builtin_protection` decision to the built-in display: a fresh baseline, a dim, a verified
/// restore and crash recovery. Every entry point runs on the main queue, the only queue that uses the backend.
final class BuiltInProtectionController: ObservableObject {
    enum Notice: Equatable {
        case autoBrightnessUnknown
        case autoBrightnessNotChanged
        case restoreFailed
    }

    /// Reserved id of the built-in screen in `protection_disabled` and `screensaver_targets` (the core's BUILTIN_ID).
    static let displayID = "builtin"

    @Published private(set) var notice: Notice?
    /// True from the saved record until a verified restore.
    private(set) var isHolding = false

    /// A readback within this many points counts as applied; the panel steps in whole percents.
    private static let tolerance = 1.0
    /// One dim or restore episode is the first attempt plus two retries; then it parks until the next trigger.
    private static let maxAttempts = 3
    private static let retryDelay: TimeInterval = 1.0
    /// A verified level is trusted for this long while the same decision repeats. Snapshots arrive several times a
    /// second and each read is a main-thread round trip, so this bounds the cost; OS brightening is corrected at most this late.
    private static let verifyInterval: TimeInterval = 5

    private let backend: BuiltInDisplayBackend
    private let scheduler: BuiltInMonitorScheduling
    private let store: BuiltInProtectionStore
    private let holder: BuiltInAutoBrightnessHolding
    private let reportHolding: (Bool) -> Void
    private let now: () -> Date

    private var baseline: Double?
    /// True after this controller asked the holder for automatic brightness, until it released it.
    private var autoHeld = false
    /// Failed dim or restore attempts in the current episode.
    private var retries = 0
    /// Set by `restoreNow` when the app quits. A snapshot or retry that arrives during termination would dim the
    /// screen again after the baseline was restored, so `reconcile` ignores every call once this is true.
    private var stopped = false
    private var retryHandle: BuiltInMonitorCancelable?
    private var lastProtection: CoreBuiltInProtection?
    /// When the dim path last verified the level, and for which decision. Cleared by every failure, restore and new
    /// episode, so only a successful check of an unchanged decision can be trusted without a read.
    private var lastVerifiedAt: Date?
    private var lastVerifiedProtection: CoreBuiltInProtection?

    init(backend: BuiltInDisplayBackend, scheduler: BuiltInMonitorScheduling, store: BuiltInProtectionStore,
         holder: BuiltInAutoBrightnessHolding, reportHolding: @escaping (Bool) -> Void, now: @escaping () -> Date = Date.init) {
        self.backend = backend
        self.scheduler = scheduler
        self.store = store
        self.holder = holder
        self.reportHolding = reportHolding
        self.now = now
    }

    /// A display that is off or asleep can still take brightness changes; a missing or non-adjustable one cannot.
    static func isProtectable(reason: BuiltInUnavailableReason?) -> Bool {
        guard let reason else { return true }
        return reason == .displayOffline
    }

    /// Launch path. A leftover record means the last run ended while dimmed. Holding starts with the stored baseline,
    /// and the next snapshot without reasons restores it. The level is not read again, because it is the dimmed one.
    func recoverIfNeeded() {
        dispatchPrecondition(condition: .onQueue(.main))
        switch store.load() {
        case .none:
            return
        case .unreadable:
            store.delete()
            DiagnosticLog.core("builtin_recovery unreadable record removed")
        case .record(let record):
            baseline = record.baseline_percent
            isHolding = true
            DiagnosticLog.core("builtin_recovery baseline=\(record.baseline_percent)")
            reportHolding(true)
        }
    }

    /// Applies one decision from the core. Level-triggered, so it is safe to call on every snapshot.
    func reconcile(_ protection: CoreBuiltInProtection) {
        dispatchPrecondition(condition: .onQueue(.main))
        // Quit has begun; a late snapshot must not dim the screen again. See `stopped`.
        guard !stopped else { return }
        // A changed target, option or availability starts a new episode, so attempts from an older one cannot park it
        // early. A lid close or display wake changes availability and so gets its own attempts.
        if let previous = lastProtection,
           previous.targetPercent != protection.targetPercent
            || previous.holdAutoBrightness != protection.holdAutoBrightness
            || previous.available != protection.available {
            retries = 0
            forgetVerification()
        }
        lastProtection = protection
        if protection.protectable, protection.available, !protection.reasons.isEmpty, let requested = protection.targetPercent {
            dim(to: requested, decision: protection)
        } else if isHolding {
            restore(available: protection.available)
        }
    }

    /// Quit path. One synchronous write and no timers. A failed write keeps the record for the next launch.
    func restoreNow() {
        // First statement, and set even when not holding, so no snapshot after this point can dim the screen again.
        stopped = true
        dispatchPrecondition(condition: .onQueue(.main))
        forgetVerification()
        guard isHolding, let base = baseline else { return }
        let restored = backend.writeLevel(base)
        // Automatic brightness goes back either way; the stored record is what restores the level later.
        releaseAutoBrightness()
        guard restored else {
            DiagnosticLog.core("builtin_restore_failed quit baseline=\(base)")
            return
        }
        finishRestore(to: base)
    }

    /// Starts holding on the first dim, then matches automatic brightness and the level to the decision.
    private func dim(to requested: Double, decision: CoreBuiltInProtection) {
        if !isHolding {
            guard startHolding() else { return }
        }
        guard let base = baseline else { return }
        // The owner goes first, so the system cannot move the level after the write.
        syncAutoBrightness(holdAutoBrightness: decision.holdAutoBrightness)
        // Never above the baseline: a target higher than the baseline must not raise the level.
        dimLevel(to: min(base, requested), baseline: base, decision: decision)
    }

    /// Reads the baseline and saves the record before anything changes. Returns false when there is nothing safe to restore to.
    private func startHolding() -> Bool {
        guard let level = backend.readLevel(), level.isFinite else {
            DiagnosticLog.core("builtin_dim skipped: baseline unreadable")
            return false
        }
        let startedAt = ISO8601DateFormatter().string(from: now())
        guard store.save(BuiltInProtectionRecord(started_at: startedAt, baseline_percent: level)) else {
            DiagnosticLog.core("builtin_dim skipped: record unwritable")
            return false
        }
        baseline = level
        isHolding = true
        notice = nil
        reportHolding(true)
        return true
    }

    /// Turns automatic brightness off with the option. A refused or unknown setting is shown, and the dim still happens.
    private func syncAutoBrightness(holdAutoBrightness: Bool) {
        if holdAutoBrightness, !autoHeld {
            autoHeld = true
            switch holder.acquire() {
            case .unknown: notice = .autoBrightnessUnknown
            case .notChanged: notice = .autoBrightnessNotChanged
            case .suspended, .alreadyOff: break
            }
        } else if !holdAutoBrightness {
            releaseAutoBrightness()
        }
    }

    private func releaseAutoBrightness() {
        guard autoHeld else { return }
        holder.release()
        autoHeld = false
    }

    /// Writes the target when the level differs. The readback decides: the setter is unverified on hardware, so its status is not trusted.
    /// A level verified for the same decision within `verifyInterval` is trusted without a read, because snapshots
    /// repeat the decision several times a second and each read is a main-thread round trip to the display.
    private func dimLevel(to target: Double, baseline base: Double, decision: CoreBuiltInProtection) {
        if let verifiedAt = lastVerifiedAt, let verified = lastVerifiedProtection, Self.isSameDim(verified, decision) {
            let elapsed = now().timeIntervalSince(verifiedAt)
            // A negative elapsed time means the wall clock moved back, so the old verification is not trusted.
            if elapsed >= 0, elapsed < Self.verifyInterval { return }
        }
        if let current = backend.readLevel(), abs(current - target) <= Self.tolerance {
            // The level already matches, so the episode is over. Clearing the count lets a later drift start with fresh
            // attempts instead of staying parked.
            retries = 0
            cancelRetry()
            markVerified(decision)
            return
        }
        // Parked: this episode has used its attempts. Later snapshots neither write nor log until a changed target,
        // option or availability starts a new episode (see `reconcile`).
        guard retries < Self.maxAttempts else { return }
        _ = backend.writeLevel(target)
        guard let readBack = backend.readLevel(), abs(readBack - target) <= Self.tolerance else {
            // A failed check must not be trusted by the next snapshot, so the window is closed before anything else.
            forgetVerification()
            // Capped like a restore, so a setter that never takes effect parks instead of retrying every second forever.
            retries += 1
            guard retries < Self.maxAttempts else {
                // Parked with no timer pending. Later snapshots stay quiet until a new episode starts.
                cancelRetry()
                DiagnosticLog.core("builtin_dim_failed attempts=\(retries)")
                return
            }
            DiagnosticLog.core("builtin_dim unverified attempt=\(retries)")
            scheduleRetry()
            return
        }
        retries = 0
        cancelRetry()
        markVerified(decision)
        DiagnosticLog.core("builtin_dim baseline=\(base) target=\(target)")
    }

    /// Two decisions are the same dim when they agree on everything the dim path acts on.
    private static func isSameDim(_ lhs: CoreBuiltInProtection, _ rhs: CoreBuiltInProtection) -> Bool {
        lhs.reasons == rhs.reasons
            && lhs.targetPercent == rhs.targetPercent
            && lhs.holdAutoBrightness == rhs.holdAutoBrightness
            && lhs.available == rhs.available
    }

    /// Records a verified level for `decision`, which opens the window that `dimLevel` honours.
    private func markVerified(_ decision: CoreBuiltInProtection) {
        lastVerifiedAt = now()
        lastVerifiedProtection = decision
    }

    /// Closes the verification window, so the next dim reads the level again instead of trusting an older readback.
    private func forgetVerification() {
        lastVerifiedAt = nil
        lastVerifiedProtection = nil
    }

    /// Writes the baseline back and verifies it by readback. Nothing is written while the display is unavailable.
    private func restore(available: Bool) {
        // A restore ends any verified dim. Without this, a display that goes unavailable and comes back within the
        // window would be trusted at its old level, because the decision would match the verified one again.
        forgetVerification()
        guard available, let base = baseline else { return }
        _ = backend.writeLevel(base)
        if let readBack = backend.readLevel(), abs(readBack - base) <= Self.tolerance {
            finishRestore(to: base)
            return
        }
        retries += 1
        guard retries < Self.maxAttempts else {
            notice = .restoreFailed
            DiagnosticLog.core("builtin_restore_failed attempts=\(retries)")
            return
        }
        DiagnosticLog.core("builtin_restore unverified attempt=\(retries)")
        scheduleRetry()
    }

    /// Ends a verified restore: gives automatic brightness back, deletes the record and reports the end of holding.
    private func finishRestore(to base: Double) {
        releaseAutoBrightness()
        store.delete()
        isHolding = false
        baseline = nil
        retries = 0
        forgetVerification()
        cancelRetry()
        if notice == .restoreFailed { notice = nil }
        reportHolding(false)
        DiagnosticLog.core("builtin_restore baseline=\(base)")
    }

    /// One retry after `retryDelay` that reconciles the last decision again. The scheduler may fire off the main queue.
    private func scheduleRetry() {
        cancelRetry()
        // A retry only follows a failed check, but the firing must verify on its own, never through the window.
        forgetVerification()
        retryHandle = scheduler.once(after: Self.retryDelay) { [weak self] in
            if Thread.isMainThread {
                self?.runRetry()
            } else {
                DispatchQueue.main.async { self?.runRetry() }
            }
        }
    }

    private func runRetry() {
        guard let protection = lastProtection else { return }
        reconcile(protection)
    }

    private func cancelRetry() {
        retryHandle?.cancel()
        retryHandle = nil
    }
}
