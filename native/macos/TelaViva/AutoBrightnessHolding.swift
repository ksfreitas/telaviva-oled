import Foundation

enum AutoBrightnessAcquireOutcome: Equatable { case suspended, alreadyOff, unknown, notChanged }

/// The system's automatic brightness as one holder sees it. The shared owner (added later) implements it.
protocol BuiltInAutoBrightnessHolding: AnyObject {
    @discardableResult func acquire() -> AutoBrightnessAcquireOutcome
    func release()
}

/// Used until the shared owner exists: never touches the setting.
final class NoAutoBrightnessHolder: BuiltInAutoBrightnessHolding {
    func acquire() -> AutoBrightnessAcquireOutcome { .alreadyOff }

    func release() {}
}
