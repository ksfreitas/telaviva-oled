import Combine
import Foundation

enum ManualMonitorControlStatus: Equatable {
    case ready
    case pending
    case failed
}

final class ManualCommandAuthorization {
    private let lock = NSLock()
    private var valid = true

    func invalidate() { lock.lock(); valid = false; lock.unlock() }

    func claim() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard valid else { return false }
        valid = false
        return true
    }
}

struct ManualControlCommand: Equatable {
    let monitorID: String
    let generation: UInt64
    let feature: HardwareKeyFeature
    let value: Double
    let revision: UInt64
    let authorization: ManualCommandAuthorization

    static func == (lhs: ManualControlCommand, rhs: ManualControlCommand) -> Bool {
        lhs.monitorID == rhs.monitorID && lhs.generation == rhs.generation && lhs.feature == rhs.feature &&
            lhs.value == rhs.value && lhs.revision == rhs.revision
    }
}

struct ManualSubmissionResult: Equatable {
    let accepted: Bool
    let actionGeneration: UInt64?
}

/// Keeps pointer intent local while a single serialized worker submits bounded updates.
/// A command remains active until the DDC controller reports its real action completion.
final class ManualMonitorControls: ObservableObject {
    typealias Submit = (ManualControlCommand, @escaping (ManualSubmissionResult) -> Void) -> Void

    @Published private(set) var changeToken: UInt64 = 0
    var onIntent: ((String, UInt64, HardwareKeyFeature, Double, UInt64) -> Void)?
    var onAccepted: ((String, UInt64, HardwareKeyFeature, Double, UInt64, Bool) -> Void)?
    var onRejected: ((String, UInt64, HardwareKeyFeature) -> Void)?

    private struct Key: Hashable {
        let monitorID: String
        let generation: UInt64
        let feature: HardwareKeyFeature
    }
    private struct Draft {
        var value: Double
        var revision: UInt64
        var status: ManualMonitorControlStatus
        var editing: Bool
        var rawTarget: Int?
    }
    private struct MonitorKey: Hashable {
        let id: String
        let generation: UInt64
    }
    private struct Active {
        let key: Key
        let command: ManualControlCommand
        var accepted: Bool
        var actionGeneration: UInt64?
    }
    private struct ExternalReceipt {
        let revision: UInt64
        var actionGeneration: UInt64?
    }
    private struct EarlyCompletion {
        let monitorID: String
        let actionID: UInt64
        let actionGeneration: UInt64
        let connectionGeneration: UInt64
        let feature: HardwareKeyFeature
        let success: Bool
        let confirmedValue: Double?
    }

    private let submit: Submit
    private let submissionQueue = DispatchQueue(label: "com.telaviva.manual-monitor-controls", qos: .userInitiated)
    private var drafts: [Key: Draft] = [:]
    private var confirmed: [Key: Double] = [:]
    private var ranges: [Key: (Double, Double)] = [:]
    private var pending: [Key: ManualControlCommand] = [:]
    private var active: [MonitorKey: Active] = [:]
    private var externalReceipts: [Key: ExternalReceipt] = [:]
    private var earlyCompletions: [EarlyCompletion] = []
    private var eligible: Set<Key> = []
    private var currentGeneration: [String: UInt64] = [:]
    private var lastRejected: Key?
    private var revision: UInt64 = 0
    private var drainScheduled = false
    private var stopped = false

    init(submit: @escaping Submit) { self.submit = submit }

    func displayValue(monitor: CoreMonitor, feature: HardwareKeyFeature) -> Double? {
        guard let generation = monitor.generation else { return value(for: monitor, feature: feature) }
        let key = Key(monitorID: monitor.id, generation: generation, feature: feature)
        return drafts[key]?.value ?? value(for: monitor, feature: feature)
    }

    func status(monitor: CoreMonitor, feature: HardwareKeyFeature) -> ManualMonitorControlStatus {
        guard let generation = monitor.generation else { return .ready }
        let key = Key(monitorID: monitor.id, generation: generation, feature: feature)
        return drafts[key]?.status ?? (lastRejected == key ? .failed : .ready)
    }

    func setValue(_ value: Double, monitor: CoreMonitor, feature: HardwareKeyFeature) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !stopped, let generation = monitor.generation, value.isFinite else { return }
        let normalized = min(100, max(0, value))
        let key = Key(monitorID: monitor.id, generation: generation, feature: feature)
        guard let capability = featureValue(monitor, feature), capability.supported else { return }
        guard monitor.display_state != "asleep", monitor.identity_status == "stable",
              currentGeneration[monitor.id] == generation else { return }
        let width = (capability.max ?? 100) - (capability.min ?? 0)
        let rawTarget = Int(((capability.min ?? 0) + width * normalized / 100).rounded())
        let priorDraft = drafts[key]
        if priorDraft?.rawTarget == rawTarget, priorDraft?.status == .pending {
            if var draft = drafts[key] { draft.value = normalized; drafts[key] = draft; changeToken &+= 1 }
            return
        }
        if pending[key] == nil, active[MonitorKey(id: monitor.id, generation: generation)] == nil,
           priorDraft?.rawTarget == rawTarget { return }
        guard drafts[key] != nil || drafts.count < 128 else {
            lastRejected = key
            onRejected?(monitor.id, generation, feature)
            changeToken &+= 1
            return
        }

        revision &+= 1
        if let running = active[MonitorKey(id: monitor.id, generation: generation)], running.key == key, !running.accepted {
            running.command.authorization.invalidate()
        }
        let command = ManualControlCommand(monitorID: monitor.id, generation: generation, feature: feature,
                                           value: normalized, revision: revision, authorization: ManualCommandAuthorization())
        drafts[key] = Draft(value: normalized, revision: revision, status: .pending,
                            editing: priorDraft?.editing ?? false, rawTarget: rawTarget)
        pending[key] = command
        onIntent?(monitor.id, generation, feature, normalized, revision)
        changeToken &+= 1
        scheduleDrain()
    }

    func setEditing(_ editing: Bool, monitor: CoreMonitor, feature: HardwareKeyFeature) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard let generation = monitor.generation else { return }
        let key = Key(monitorID: monitor.id, generation: generation, feature: feature)
        if var draft = drafts[key] { draft.editing = editing; drafts[key] = draft }
        changeToken &+= 1
    }

    /// Mirrors a keyboard target; the router owns its submission, so this only cancels
    /// an unsent slider target and changes the popup's local value.
    @discardableResult
    func adoptExternalIntent(monitorID: String, generation: UInt64, feature: HardwareKeyFeature,
                             value: Double, sourceToken: UInt64? = nil) -> UInt64? {
        dispatchPrecondition(condition: .onQueue(.main))
        let key = Key(monitorID: monitorID, generation: generation, feature: feature)
        guard !stopped, value.isFinite, (0...100).contains(value), currentGeneration[monitorID] == generation,
              eligible.contains(key), drafts[key] != nil || drafts.count < 128 else { return nil }
        pending.removeValue(forKey: key)
        if let running = active[MonitorKey(id: monitorID, generation: generation)], running.key == key, !running.accepted {
            running.command.authorization.invalidate()
        }
        revision &+= 1
        drafts[key] = Draft(value: value, revision: revision, status: .pending, editing: false, rawTarget: nil)
        externalReceipts[key] = ExternalReceipt(revision: revision, actionGeneration: nil)
        _ = sourceToken
        changeToken &+= 1
        return revision
    }

    func bindExternalReceipt(monitorID: String, generation: UInt64, feature: HardwareKeyFeature,
                             revision receiptRevision: UInt64, actionGeneration: UInt64) {
        dispatchPrecondition(condition: .onQueue(.main))
        let key = Key(monitorID: monitorID, generation: generation, feature: feature)
        guard var receipt = externalReceipts[key], receipt.revision == receiptRevision else { return }
        receipt.actionGeneration = actionGeneration
        externalReceipts[key] = receipt
    }

    func rejectExternalIntent(monitorID: String, generation: UInt64, feature: HardwareKeyFeature, revision rejectedRevision: UInt64) {
        dispatchPrecondition(condition: .onQueue(.main))
        let key = Key(monitorID: monitorID, generation: generation, feature: feature)
        guard externalReceipts[key]?.revision == rejectedRevision else { return }
        externalReceipts.removeValue(forKey: key)
        if var draft = drafts[key], draft.revision == rejectedRevision {
            draft.value = confirmed[key] ?? draft.value
            draft.status = .failed
            drafts[key] = draft
        }
        changeToken &+= 1
    }

    func apply(_ snapshot: CoreSnapshot) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !stopped else { return }
        let monitors = snapshot.monitors ?? []
        var seen = Set<String>()
        for monitor in monitors {
            guard let generation = monitor.generation else { continue }
            seen.insert(monitor.id)
            if currentGeneration[monitor.id] != generation {
                if let old = currentGeneration[monitor.id] {
                    active[MonitorKey(id: monitor.id, generation: old)]?.command.authorization.invalidate()
                    drafts = drafts.filter { $0.key.monitorID != monitor.id || $0.key.generation != old }
                    pending = pending.filter { $0.key.monitorID != monitor.id || $0.key.generation != old }
                    active = active.filter { $0.key.id != monitor.id || $0.key.generation != old }
                    confirmed = confirmed.filter { $0.key.monitorID != monitor.id || $0.key.generation != old }
                    ranges = ranges.filter { $0.key.monitorID != monitor.id || $0.key.generation != old }
                    externalReceipts = externalReceipts.filter { $0.key.monitorID != monitor.id || $0.key.generation != old }
                    eligible = eligible.filter { $0.monitorID != monitor.id || $0.generation != old }
                    earlyCompletions.removeAll { $0.monitorID == monitor.id && $0.connectionGeneration == old }
                }
                currentGeneration[monitor.id] = generation
            }
            for feature in [HardwareKeyFeature.brightness, .volume] {
                let key = Key(monitorID: monitor.id, generation: generation, feature: feature)
                guard monitor.display_state != "asleep", monitor.identity_status == "stable",
                      let value = featureValue(monitor, feature), value.supported, let confirmedValue = value.value else {
                    eligible.remove(key)
                    drafts.removeValue(forKey: key); pending.removeValue(forKey: key); confirmed.removeValue(forKey: key)
                    ranges.removeValue(forKey: key)
                    if let running = active[MonitorKey(id: monitor.id, generation: generation)], running.key == key {
                        running.command.authorization.invalidate()
                        active.removeValue(forKey: MonitorKey(id: monitor.id, generation: generation))
                    }
                    externalReceipts.removeValue(forKey: key)
                    earlyCompletions.removeAll { $0.monitorID == monitor.id && $0.connectionGeneration == generation }
                    continue
                }
                confirmed[key] = confirmedValue
                ranges[key] = (value.min ?? 0, value.max ?? 100)
                eligible.insert(key)
                if drafts[key] == nil { continue }
                let targetOutstanding = pending[key] != nil || externalReceipts[key] != nil ||
                    active[MonitorKey(id: monitor.id, generation: generation)]?.key == key
                if !targetOutstanding, var draft = drafts[key] {
                    if draft.status == .failed { draft.value = confirmedValue; drafts[key] = draft }
                    else { drafts.removeValue(forKey: key) }
                }
            }
        }
        for id in Array(currentGeneration.keys) where !seen.contains(id) {
            if let old = currentGeneration.removeValue(forKey: id) {
                active.filter { $0.key.id == id }.values.forEach { $0.command.authorization.invalidate() }
                drafts = drafts.filter { $0.key.monitorID != id }
                pending = pending.filter { $0.key.monitorID != id }
                active = active.filter { $0.key.id != id }
                confirmed = confirmed.filter { $0.key.monitorID != id }
                ranges = ranges.filter { $0.key.monitorID != id }
                externalReceipts = externalReceipts.filter { $0.key.monitorID != id }
                eligible = eligible.filter { $0.monitorID != id }
                earlyCompletions.removeAll { $0.monitorID == id }
                _ = old
            }
        }
        changeToken &+= 1
        scheduleDrain()
    }

    /// Called only after the DDC worker has submitted the actual hardware_result.
    func actionCompleted(monitorID: String, actionID: UInt64, actionGeneration: UInt64, connectionGeneration: UInt64,
                         feature: HardwareKeyFeature, success: Bool, confirmedValue: Double? = nil) {
        let completion = EarlyCompletion(monitorID: monitorID, actionID: actionID, actionGeneration: actionGeneration,
                                         connectionGeneration: connectionGeneration, feature: feature,
                                         success: success, confirmedValue: confirmedValue)
        let update: () -> Void = { [weak self] in
            guard let self else { return }
            self.completeOnMain(completion)
        }
        if Thread.isMainThread { update() } else { DispatchQueue.main.async(execute: update) }
    }

    private func completeOnMain(_ completion: EarlyCompletion) {
            guard !stopped else { return }
            let monitorID = completion.monitorID
            let actionID = completion.actionID
            let actionGeneration = completion.actionGeneration
            let connectionGeneration = completion.connectionGeneration
            let feature = completion.feature
            let success = completion.success
            let confirmedValue = completion.confirmedValue
            let monitorKey = MonitorKey(id: monitorID, generation: connectionGeneration)
            let externalKey = Key(monitorID: monitorID, generation: connectionGeneration, feature: feature)
            if let receipt = self.externalReceipts[externalKey], receipt.actionGeneration == actionGeneration,
               self.active[monitorKey]?.actionGeneration != actionGeneration {
                self.externalReceipts.removeValue(forKey: externalKey)
                let range = self.ranges[externalKey] ?? (0, 100)
                if let confirmedValue, confirmedValue.isFinite {
                    let width = range.1 - range.0
                    self.confirmed[externalKey] = width > 0 ? (confirmedValue - range.0) * 100 / width : 0
                }
                if var draft = self.drafts[externalKey], draft.revision == receipt.revision {
                    if success, let value = self.confirmed[externalKey] { draft.value = value; draft.status = .ready }
                    else { draft.value = self.confirmed[externalKey] ?? draft.value; draft.status = .failed }
                    self.drafts[externalKey] = draft
                }
                self.changeToken &+= 1
                return
            }
            if let running = self.active[monitorKey], running.key.feature == feature, running.actionGeneration == nil {
                if earlyCompletions.count < 128 { earlyCompletions.append(completion) }
                return
            }
            guard let running = self.active[monitorKey], running.key.feature == feature,
                  running.actionGeneration == actionGeneration else { _ = actionID; return }
            _ = actionID
            self.active.removeValue(forKey: monitorKey)
            let key = running.key
            if let confirmedValue, confirmedValue.isFinite {
                let range = self.ranges[key] ?? (0, 100)
                let width = range.1 - range.0
                self.confirmed[key] = width > 0 ? (confirmedValue - range.0) * 100 / width : 0
            }
            if self.pending[key] != nil {
                if var draft = self.drafts[key] { draft.status = .pending; self.drafts[key] = draft }
            } else if var draft = self.drafts[key], draft.revision == running.command.revision {
                if success, let value = self.confirmed[key] { draft.value = value; draft.status = .ready }
                else { draft.value = self.confirmed[key] ?? draft.value; draft.status = .failed }
                draft.rawTarget = nil
                self.drafts[key] = draft
            }
            self.changeToken &+= 1
            self.scheduleDrain()
    }

    func protectionBrightnessMaximum(monitor: CoreMonitor) -> Double? {
        guard let generation = monitor.generation else { return monitor.brightness?.value }
        let key = Key(monitorID: monitor.id, generation: generation, feature: .brightness)
        guard let confirmedValue = confirmed[key] ?? monitor.brightness?.value else { return nil }
        if drafts[key]?.status == .pending, let draft = drafts[key], draft.value < confirmedValue { return draft.value }
        return confirmedValue
    }

    func shutdown() {
        dispatchPrecondition(condition: .onQueue(.main))
        stopped = true
        active.values.forEach { $0.command.authorization.invalidate() }
        pending.removeAll()
        externalReceipts.removeAll()
        earlyCompletions.removeAll()
        drafts.removeAll()
        changeToken &+= 1
    }

    // Deterministic observation for the native regression harness.
    var pendingCountForTesting: Int { pending.count }
    var activeCountForTesting: Int { active.count }
    func waitForWorkerForTesting() { submissionQueue.sync {} }

    private func scheduleDrain() {
        guard !stopped, !drainScheduled else { return }
        drainScheduled = true
        DispatchQueue.main.async { [weak self] in self?.drain() }
    }

    private func drain() {
        dispatchPrecondition(condition: .onQueue(.main))
        drainScheduled = false
        guard !stopped else { return }
        let candidate = pending.values.sorted { $0.revision < $1.revision }.first { command in
            active[MonitorKey(id: command.monitorID, generation: command.generation)] == nil
        }
        guard let command = candidate else { return }
        let key = Key(monitorID: command.monitorID, generation: command.generation, feature: command.feature)
        let monitorKey = MonitorKey(id: command.monitorID, generation: command.generation)
        guard pending[key]?.revision == command.revision else { scheduleDrain(); return }
        pending.removeValue(forKey: key)
        active[monitorKey] = Active(key: key, command: command, accepted: false)
        let submit = self.submit
        submissionQueue.async { [weak self] in
            submit(command) { [weak self] result in
                let apply: () -> Void = { [weak self] in
                    guard let self else { return }
                    self.accepted(command, result: result)
                }
                if Thread.isMainThread { apply() } else { DispatchQueue.main.async(execute: apply) }
            }
        }
        changeToken &+= 1
    }

    private func accepted(_ command: ManualControlCommand, result: ManualSubmissionResult) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !stopped else { return }
        let monitorKey = MonitorKey(id: command.monitorID, generation: command.generation)
        guard var running = active[monitorKey], running.command.revision == command.revision else { return }
        onAccepted?(command.monitorID, command.generation, command.feature, command.value, command.revision, result.accepted)
        if result.accepted {
            running.accepted = true
            running.actionGeneration = result.actionGeneration
            active[monitorKey] = running
            if let actionGeneration = result.actionGeneration {
                let matched = earlyCompletions.first { $0.monitorID == command.monitorID &&
                    $0.connectionGeneration == command.generation && $0.feature == command.feature &&
                    $0.actionGeneration == actionGeneration }
                earlyCompletions.removeAll { $0.monitorID == command.monitorID && $0.connectionGeneration == command.generation }
                if let matched { completeOnMain(matched) }
            }
        } else {
            active.removeValue(forKey: monitorKey)
            earlyCompletions.removeAll { $0.monitorID == command.monitorID && $0.connectionGeneration == command.generation }
            let key = running.key
            if pending[key] == nil, var draft = drafts[key], draft.revision == command.revision {
                draft.value = confirmed[key] ?? draft.value
                draft.status = .failed
                draft.rawTarget = nil
                drafts[key] = draft
            }
            scheduleDrain()
        }
        changeToken &+= 1
    }

    private func value(for monitor: CoreMonitor, feature: HardwareKeyFeature) -> Double? {
        featureValue(monitor, feature)?.value
    }

    private func featureValue(_ monitor: CoreMonitor, _ feature: HardwareKeyFeature) -> VcpValue? {
        feature == .brightness ? monitor.brightness : monitor.volume
    }
}
