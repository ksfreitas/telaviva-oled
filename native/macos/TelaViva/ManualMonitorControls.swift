import Combine
import Foundation

enum ManualMonitorControlStatus: Equatable {
    case ready
    case pending
    case failed
}

/// The part of the core's controls machine the popup needs. ControlsDriver conforms.
protocol ManualControlsDriving: AnyObject {
    @discardableResult func slider(monitorID: String, feature: HardwareKeyFeature, value: Double) -> ControlsDraft?
    func queryDraft(monitorID: String, feature: HardwareKeyFeature) -> ControlsDraft
}

/// Thin UI binding over the core's controls machine. The core owns drafts, coalescing and the single in-flight
/// write; this type forwards slider values and maps the core's drafts for display. Main queue only, except
/// `draftChanged()`.
final class ManualMonitorControls: ObservableObject {
    @Published private(set) var changeToken: UInt64 = 0
    private let driver: ManualControlsDriving
    private var stopped = false

    init(driver: ManualControlsDriving) { self.driver = driver }

    /// The core's draft when it has one; nil tells the view to show the confirmed value.
    func displayValue(monitor: CoreMonitor, feature: HardwareKeyFeature) -> Double? {
        guard monitor.generation != nil else { return nil }
        let draft = driver.queryDraft(monitorID: monitor.id, feature: feature)
        return draft.hasDraft ? draft.draft : nil
    }

    func status(monitor: CoreMonitor, feature: HardwareKeyFeature) -> ManualMonitorControlStatus {
        guard monitor.generation != nil else { return .ready }
        switch driver.queryDraft(monitorID: monitor.id, feature: feature).status {
        case .ready: return .ready
        case .pending: return .pending
        case .failed: return .failed
        }
    }

    func setValue(_ value: Double, monitor: CoreMonitor, feature: HardwareKeyFeature) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !stopped, monitor.generation != nil, value.isFinite else { return }
        driver.slider(monitorID: monitor.id, feature: feature, value: min(100, max(0, value)))
        changeToken &+= 1
    }

    /// The core has no editing flag, so this only requests a repaint.
    func setEditing(_ editing: Bool, monitor: CoreMonitor, feature: HardwareKeyFeature) {
        dispatchPrecondition(condition: .onQueue(.main))
        changeToken &+= 1
    }

    /// Repaint request from the driver's draft_changed effect. Callable from any thread.
    func draftChanged() {
        if Thread.isMainThread {
            changeToken &+= 1
        } else {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.changeToken &+= 1
            }
        }
    }

    /// Stops forwarding slider values. The core owns its own shutdown.
    func shutdown() {
        dispatchPrecondition(condition: .onQueue(.main))
        stopped = true
        changeToken &+= 1
    }
}
