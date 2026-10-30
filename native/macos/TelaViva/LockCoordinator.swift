import Foundation

/// Turns an observed lock request into at most one native lock per request token and reports each outcome.
/// Main thread only. Log lines never carry the token or any other identifier.
final class LockCoordinator {
    private let locker: SessionLocking
    private let submit: (_ token: UInt64, _ ok: Bool, _ error: String?) -> Void
    private let log: (String) -> Void
    private var lastHandledToken: UInt64?

    init(locker: SessionLocking,
         submit: @escaping (_ token: UInt64, _ ok: Bool, _ error: String?) -> Void,
         log: @escaping (String) -> Void) {
        self.locker = locker
        self.submit = submit
        self.log = log
    }

    /// Acts only on a requested observation with a token different from the last handled one.
    /// The token is remembered before the lock starts, so repeats while it is pending do nothing.
    /// A false request or a nil token does nothing and keeps the last handled token.
    func observe(requested: Bool, token: UInt64?) {
        guard requested, let token, token != lastHandledToken else { return }
        lastHandledToken = token
        locker.lockNow { [submit, log] outcome in
            switch outcome {
            case .locked:
                submit(token, true, nil)
                log("lock confirmed")
            case .unavailable:
                submit(token, false, "unavailable")
                log("lock failed: unavailable")
            case .notConfirmed:
                submit(token, false, "not_confirmed")
                log("lock failed: not confirmed")
            }
        }
    }
}
