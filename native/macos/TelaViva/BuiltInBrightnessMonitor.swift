import Foundation

protocol BuiltInMonitorCancelable {
    func cancel()
}

protocol BuiltInMonitorScheduling {
    /// Calls `block` every `interval` seconds until the returned handle is canceled.
    func repeating(interval: TimeInterval, leeway: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable
    /// Calls `block` once, `after` seconds from now, unless the returned handle is canceled first.
    func once(after: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable
}

struct BuiltInDisplayReport: Equatable {
    var available: Bool
    /// Whole percent, or nil when no level is known.
    var brightness: Int?
    var calibrating: Bool
    var reason: BuiltInUnavailableReason?
}

/// Polls the built-in display level and reports whole-percent changes.
///
/// Reads happen only while enabled, available, awake and not calibrating. A failed read keeps
/// the last good value and never counts as zero. Backend reads run under `lock`; `submit` and
/// scheduler calls run after it is released, because scheduler callbacks arrive on its queue.
/// Each planned report gets a sequence number under `lock`; `deliver` drops any report older than
/// the last one submitted, so a stale report never overwrites a newer one.
final class BuiltInBrightnessMonitor {
    private static let pollInterval: TimeInterval = 1.0
    private static let pollLeeway: TimeInterval = 0.5
    private static let debounceInterval: TimeInterval = 0.25

    /// Work decided under `lock` and carried out after it is released.
    private struct Plan {
        var report: BuiltInDisplayReport?
        var reportSeq = 0
        var cancel: [BuiltInMonitorCancelable] = []
        var armTimer: Int?
        var scheduleDebounce: Int?
    }

    private let backend: BuiltInDisplayBackend
    private let scheduler: BuiltInMonitorScheduling
    private let submit: (BuiltInDisplayReport) -> Void
    private let lock = NSLock()
    /// Serializes delivery. Never take `lock` while holding it.
    private let deliveryLock = NSLock()

    // Guarded by lock.
    private var started = false
    private var enabled = false
    private var calibrating = false
    /// Last known availability; nil until started.
    private var status: BuiltInDisplayStatus?
    /// Last level sent to the core.
    private var lastReported: Int?
    /// Last good level read, waiting for the debounce.
    private var latest: Int?
    private var tokenCounter = 0
    /// A registration whose token is no longer current is ignored or canceled.
    private var timerToken: Int?
    private var timerHandle: BuiltInMonitorCancelable?
    private var debounceToken: Int?
    private var debounceHandle: BuiltInMonitorCancelable?
    private var reportSequence = 0

    // Guarded by deliveryLock.
    /// Sequence number of the last report submitted.
    private var lastDeliveredSeq = 0

    init(backend: BuiltInDisplayBackend, scheduler: BuiltInMonitorScheduling, submit: @escaping (BuiltInDisplayReport) -> Void) {
        self.backend = backend
        self.scheduler = scheduler
        self.submit = submit
    }

    func start() {
        update { plan in
            guard !started else { return }
            started = true
            applyStatusLocked(backend.status(), plan: &plan)
        }
    }

    func stop() {
        update { plan in
            started = false
            status = nil
            disarmLocked(&plan)
        }
    }

    func setEnabled(_ enabled: Bool) {
        update { plan in
            guard self.enabled != enabled else { return }
            self.enabled = enabled
            if !enabled {
                disarmLocked(&plan)
            } else if monitoringLocked {
                armTimerLocked(&plan)
                publishLocked(&plan)
            }
        }
    }

    func setCalibrating(_ calibrating: Bool) {
        update { plan in
            guard self.calibrating != calibrating else { return }
            self.calibrating = calibrating
            guard started else { return }
            if calibrating {
                disarmLocked(&plan)
                plan.report = reportLocked(brightness: lastReported)
            } else if monitoringLocked {
                armTimerLocked(&plan)
                publishLocked(&plan)
            } else {
                plan.report = reportLocked(brightness: nil)
            }
        }
    }

    func refreshAvailability() {
        update { plan in
            guard started else { return }
            applyStatusLocked(backend.status(), plan: &plan)
        }
    }

    /// Applies a fresh availability reading and reports only when availability or the reason changed.
    /// Returns true when it changed.
    @discardableResult
    private func applyStatusLocked(_ new: BuiltInDisplayStatus, plan: inout Plan) -> Bool {
        if let current = status, current.available == new.available, current.reason == new.reason { return false }
        status = new
        guard new.available else {
            disarmLocked(&plan)
            plan.report = reportLocked(brightness: nil)
            return true
        }
        if monitoringLocked {
            armTimerLocked(&plan)
            publishLocked(&plan)
        } else {
            plan.report = reportLocked(brightness: nil)
        }
        return true
    }

    private var monitoringLocked: Bool {
        started && enabled && !calibrating && (status?.available ?? false)
    }

    private func armTimerLocked(_ plan: inout Plan) {
        guard timerToken == nil else { return }
        let token = nextTokenLocked()
        timerToken = token
        plan.armTimer = token
    }

    private func disarmLocked(_ plan: inout Plan) {
        timerToken = nil
        if let timerHandle { plan.cancel.append(timerHandle) }
        timerHandle = nil
        cancelDebounceLocked(&plan)
    }

    private func scheduleDebounceLocked(_ plan: inout Plan) {
        cancelDebounceLocked(&plan)
        let token = nextTokenLocked()
        debounceToken = token
        plan.scheduleDebounce = token
    }

    private func cancelDebounceLocked(_ plan: inout Plan) {
        debounceToken = nil
        if let debounceHandle { plan.cancel.append(debounceHandle) }
        debounceHandle = nil
    }

    private func nextTokenLocked() -> Int {
        tokenCounter += 1
        return tokenCounter
    }

    /// Reads the level now and reports it at once. A failed read or an asleep display reports the last good value.
    private func publishLocked(_ plan: inout Plan) {
        if !backend.isAsleep(), let value = readRoundedLocked() {
            cancelDebounceLocked(&plan)
            latest = value
            lastReported = value
        }
        plan.report = reportLocked(brightness: lastReported)
    }

    /// Level rounded to a whole percent, or nil when the read fails. A failure is never reported as zero.
    private func readRoundedLocked() -> Int? {
        guard let level = backend.readLevel(), level.isFinite else { return nil }
        return Int(min(max(level, 0), 100).rounded())
    }

    private func reportLocked(brightness: Int?) -> BuiltInDisplayReport {
        guard let status, status.available else {
            return BuiltInDisplayReport(available: false, brightness: nil, calibrating: calibrating, reason: status?.reason)
        }
        return BuiltInDisplayReport(available: true, brightness: brightness, calibrating: calibrating, reason: nil)
    }

    /// Checks availability before reading, so a lost display is reported and its timer canceled.
    private func tick(token: Int) {
        update { plan in
            guard timerToken == token else { return }
            if applyStatusLocked(backend.status(), plan: &plan) { return }
            guard monitoringLocked, !backend.isAsleep(),
                  let value = readRoundedLocked() else { return }
            latest = value
            if value == lastReported {
                cancelDebounceLocked(&plan)
            } else {
                scheduleDebounceLocked(&plan)
            }
        }
    }

    private func debounceFired(token: Int) {
        update { plan in
            guard debounceToken == token, let value = latest else { return }
            debounceToken = nil
            debounceHandle = nil
            guard monitoringLocked, value != lastReported else { return }
            lastReported = value
            plan.report = reportLocked(brightness: value)
        }
    }

    /// Runs `body` under the lock, then carries out the plan it produced outside the lock.
    private func update(_ body: (inout Plan) -> Void) {
        var plan = Plan()
        lock.lock()
        body(&plan)
        if plan.report != nil {
            reportSequence += 1
            plan.reportSeq = reportSequence
        }
        lock.unlock()
        execute(plan)
    }

    private func execute(_ plan: Plan) {
        plan.cancel.forEach { $0.cancel() }
        if let token = plan.armTimer {
            let handle = scheduler.repeating(interval: Self.pollInterval, leeway: Self.pollLeeway) { [weak self] in
                self?.tick(token: token)
            }
            adoptTimer(handle, token: token)
        }
        if let token = plan.scheduleDebounce {
            let handle = scheduler.once(after: Self.debounceInterval) { [weak self] in
                self?.debounceFired(token: token)
            }
            adoptDebounce(handle, token: token)
        }
        if let report = plan.report {
            deliver(report, seq: plan.reportSeq)
        }
    }

    /// Submits `report` unless a newer one was already submitted. Check and submit are atomic under `deliveryLock`.
    func deliver(_ report: BuiltInDisplayReport, seq: Int) {
        deliveryLock.lock()
        defer { deliveryLock.unlock() }
        guard seq > lastDeliveredSeq else { return }
        lastDeliveredSeq = seq
        submit(report)
    }

    private func adoptTimer(_ handle: BuiltInMonitorCancelable, token: Int) {
        lock.lock()
        let current = timerToken == token
        if current { timerHandle = handle }
        lock.unlock()
        if !current { handle.cancel() }
    }

    private func adoptDebounce(_ handle: BuiltInMonitorCancelable, token: Int) {
        lock.lock()
        let current = debounceToken == token
        if current { debounceHandle = handle }
        lock.unlock()
        if !current { handle.cancel() }
    }
}

/// Serial utility queue; repeats use a DispatchSourceTimer and one-shots a DispatchWorkItem.
final class DispatchBuiltInScheduler: BuiltInMonitorScheduling {
    private let queue = DispatchQueue(label: "com.telaviva.builtin-brightness", qos: .utility)

    func repeating(interval: TimeInterval, leeway: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + interval, repeating: interval, leeway: .milliseconds(Int(leeway * 1000)))
        timer.setEventHandler { block() }
        timer.resume()
        return DispatchCancelable(action: { timer.cancel() })
    }

    func once(after: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable {
        let item = DispatchWorkItem { block() }
        queue.asyncAfter(deadline: .now() + after, execute: item)
        return DispatchCancelable(action: { item.cancel() })
    }
}

private struct DispatchCancelable: BuiltInMonitorCancelable {
    let action: () -> Void

    func cancel() {
        action()
    }
}
