// The only file that names the private lock symbol. It is loaded with dlopen/dlsym and fails closed when missing (same isolation rule as ADR 0007).

import Foundation

enum LockOutcome: Equatable {
    /// The lock was requested and the session reported locked.
    case locked
    /// The private symbol is missing or could not be loaded; nothing was called.
    case unavailable
    /// The symbol ran, but the session did not report locked within the confirmation window.
    case notConfirmed
}

protocol SessionLocking {
    /// Completes exactly once, on the main queue.
    func lockNow(completion: @escaping (LockOutcome) -> Void)
}

/// Calls the private lock entry point and confirms it by polling the session lock state.
/// Main thread only.
final class SessionLocker: SessionLocking {
    private let loadSymbol: () -> (@convention(c) () -> Void)?
    private let isLocked: () -> Bool?
    private let confirmWithin: TimeInterval
    private let pollInterval: TimeInterval

    init(loadSymbol: @escaping () -> (@convention(c) () -> Void)? = SessionLocker.loadLoginSymbol,
         isLocked: @escaping () -> Bool?,
         confirmWithin: TimeInterval = 3,
         pollInterval: TimeInterval = 0.1) {
        self.loadSymbol = loadSymbol
        self.isLocked = isLocked
        self.confirmWithin = confirmWithin
        self.pollInterval = pollInterval
    }

    /// Resolves the private login entry point, or returns nil when the framework or symbol is absent.
    /// The handle lives for the whole process, so it is intentionally never closed.
    static func loadLoginSymbol() -> (@convention(c) () -> Void)? {
        guard let handle = dlopen("/System/Library/PrivateFrameworks/login.framework/login", RTLD_LAZY) else {
            return nil
        }
        guard let symbol = dlsym(handle, "SACLockScreenImmediate") else { return nil }
        return unsafeBitCast(symbol, to: (@convention(c) () -> Void).self)
    }

    func lockNow(completion: @escaping (LockOutcome) -> Void) {
        guard let lockScreen = loadSymbol() else {
            DispatchQueue.main.async { completion(.unavailable) }
            return
        }
        lockScreen()
        let confirmation = LockConfirmation(startedAt: Date(),
                                            confirmWithin: confirmWithin,
                                            pollInterval: pollInterval,
                                            isLocked: isLocked,
                                            completion: completion)
        // The first poll runs on the next main-queue turn, so completion never re-enters the caller.
        DispatchQueue.main.async { confirmation.poll() }
    }
}

/// Polls the lock state on the main queue until it reports locked or the confirmation window ends.
/// Completes exactly once: every path either returns or delivers the outcome.
private final class LockConfirmation {
    private let startedAt: Date
    private let confirmWithin: TimeInterval
    private let pollInterval: TimeInterval
    private let isLocked: () -> Bool?
    private let completion: (LockOutcome) -> Void

    init(startedAt: Date,
         confirmWithin: TimeInterval,
         pollInterval: TimeInterval,
         isLocked: @escaping () -> Bool?,
         completion: @escaping (LockOutcome) -> Void) {
        self.startedAt = startedAt
        self.confirmWithin = confirmWithin
        self.pollInterval = pollInterval
        self.isLocked = isLocked
        self.completion = completion
    }

    func poll() {
        if isLocked() == true {
            completion(.locked)
        } else if Date().timeIntervalSince(startedAt) >= confirmWithin {
            completion(.notConfirmed)
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + pollInterval) { self.poll() }
        }
    }
}
