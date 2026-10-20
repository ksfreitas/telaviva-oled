import Combine
import Foundation

enum InferenceDeviceSelection {
    // Enabled after the M4 Pro hardware/host validation; each session still validates locally.
    // Evidence: specs/archived/inference-device-selection/validation/macos-coreml.md.
    static let automaticValidated = true
    // The first CoreML inference compiles lazily and measured up to 242 ms for the GPU option. The budget is 500 ms to leave headroom for a cold compile; Windows uses the same value.
    static let latencyBudgetMilliseconds: Double = 500

    struct Option: Equatable, Identifiable {
        let id: String
        let kind: String
        let labelKey: String
    }

    static let options = [
        Option(id: "auto", kind: "auto", labelKey: "inference.auto"),
        Option(id: "ane", kind: "ane", labelKey: "inference.ane"),
        Option(id: "gpu", kind: "gpu", labelKey: "inference.gpu"),
        Option(id: "cpu", kind: "cpu", labelKey: "inference.cpu")
    ]

    enum State: String, Equatable {
        case ok
        case autoCPU = "auto_cpu"
        case unavailable
        case failed
    }

    enum FailureReason: String, Equatable {
        case registerFailed = "register_failed"
        case sessionFailed = "session_failed"
        case selfTestMismatch = "self_test_mismatch"
        case selfTestSlow = "self_test_slow"
        case runtimeError = "runtime_error"
    }

    struct Candidate: Equatable {
        let provider: Int32
        let deviceIndex: UInt32
        let activeId: String
        let activeLabel: String
    }

    struct Value: Equatable {
        var requestedId: String
        var activeId: String
        var activeLabel: String
        var state: State
        var reason: FailureReason?
    }

    struct Resolution: Equatable {
        let candidate: Candidate
        let report: Value
        let viaAutomatic: Bool

        func fallingBack(reason: FailureReason) -> Resolution {
            let state: State = report.state == .unavailable
                ? .unavailable : (viaAutomatic ? .autoCPU : .failed)
            let cpu = Candidate(provider: 0, deviceIndex: 0, activeId: "cpu", activeLabel: "CPU")
            return Resolution(candidate: cpu,
                              report: Value(requestedId: report.requestedId, activeId: "cpu",
                                            activeLabel: "CPU", state: state, reason: reason),
                              viaAutomatic: viaAutomatic)
        }
    }

    /// The single retained policy value used by detector creation. It survives idle release, so a
    /// CPU fallback cannot accidentally re-resolve the user's accelerated preference on recreation.
    struct Session: Equatable {
        let automaticValidated: Bool
        private(set) var resolution: Resolution
        private(set) var decision: SessionDecision

        init(preference: String, automaticValidated: Bool = InferenceDeviceSelection.automaticValidated) {
            self.automaticValidated = automaticValidated
            self.resolution = InferenceDeviceSelection.resolve(requestedId: preference,
                                                               automaticValidated: automaticValidated)
            self.decision = SessionDecision(preference: preference, automaticValidated: automaticValidated)
        }

        var preference: String { resolution.report.requestedId }
        var candidate: Candidate {
            decision.activeProvider == 0 ? cpuCandidate : resolution.candidate
        }

        mutating func preferenceChanged(to value: String) {
            resolution = InferenceDeviceSelection.resolve(requestedId: value,
                                                           automaticValidated: automaticValidated)
            decision = SessionDecision(preference: value, automaticValidated: automaticValidated)
        }

        mutating func candidateAccepted() { decision = decision.afterValidation() }

        mutating func candidateFailed(_ reason: FailureReason) {
            resolution = resolution.fallingBack(reason: reason)
            decision = decision.afterCandidateFailure()
        }

        mutating func runtimeFailed() {
            resolution = resolution.fallingBack(reason: .runtimeError)
            decision = decision.afterRuntimeError()
        }

        mutating func idleReleased() { decision = decision.afterIdleRelease() }
    }

    /// Owns the currently selected detector and the retained decision. Its injected functions make
    /// provider creation, validation and frame retry testable without loading ONNX Runtime.
    final class DetectorHost<Handle> {
        private(set) var session: Session
        private(set) var handle: Handle?
        private let create: (Candidate) -> Handle?
        private let destroy: (Handle) -> Void
        private let selectedProvider: (Handle) -> Int32
        private let validate: (Handle) -> FailureReason?
        private let publish: (Value, Bool) -> Void

        init(session: Session, create: @escaping (Candidate) -> Handle?, destroy: @escaping (Handle) -> Void,
             selectedProvider: @escaping (Handle) -> Int32, validate: @escaping (Handle) -> FailureReason?,
             publish: @escaping (Value, Bool) -> Void) {
            self.session = session
            self.create = create
            self.destroy = destroy
            self.selectedProvider = selectedProvider
            self.validate = validate
            self.publish = publish
        }

        func setPreference(_ value: String) {
            guard session.preference != value else { return }
            release()
            session.preferenceChanged(to: value)
            publish(session.resolution.report, false)
        }

        func ensure() -> Bool {
            if handle != nil { return true }
            let resolution = session.resolution
            publish(resolution.report, false)
            let candidate = session.candidate
            guard let created = create(candidate) else {
                return candidate.provider == 0 ? failCPUCreate() : fallback(.sessionFailed)
            }
            if candidate.provider != 0 {
                guard selectedProvider(created) == candidate.provider else {
                    destroy(created)
                    return fallback(.registerFailed)
                }
                if session.decision.shouldValidateCandidate {
                    if let failure = validate(created) {
                        destroy(created)
                        return fallback(failure)
                    }
                    session.candidateAccepted()
                }
            } else {
                session.candidateAccepted()
            }
            handle = created
            publish(session.resolution.report, true)
            return true
        }

        func detect<Result>(_ result: inout Result, using operation: (Handle, inout Result) -> Int32) -> Int32? {
            guard ensure(), let active = handle else { return nil }
            var code = operation(active, &result)
            if code < 0, session.decision.activeProvider != 0 {
                destroy(active)
                handle = nil
                session.runtimeFailed()
                publish(session.resolution.report, true)
                guard let cpu = create(session.candidate) else {
                    publish(Value(requestedId: session.preference, activeId: "cpu", activeLabel: "CPU",
                                  state: .failed, reason: .runtimeError), true)
                    return code
                }
                handle = cpu
                code = operation(cpu, &result)
            }
            if code < 0 {
                publish(Value(requestedId: session.preference, activeId: "cpu", activeLabel: "CPU",
                              state: .failed, reason: .runtimeError), true)
            }
            return code
        }

        /// Releases the runtime session after the existing idle timeout, keeping the decision for reopen.
        func release() {
            if let handle { destroy(handle) }
            handle = nil
            session.idleReleased()
        }

        private func fallback(_ reason: FailureReason) -> Bool {
            session.candidateFailed(reason)
            publish(session.resolution.report, true)
            guard let cpu = create(session.candidate) else { return failCPUCreate() }
            handle = cpu
            return true
        }

        private func failCPUCreate() -> Bool {
            publish(Value(requestedId: session.preference, activeId: "cpu", activeLabel: "CPU",
                          state: .failed, reason: .sessionFailed), true)
            return false
        }
    }

    /// Resolves the shell setting without loading ONNX Runtime. A foreign-platform id runs via Automatic
    /// for this process while preserving the requested id in the report and settings store.
    static func resolve(requestedId: String, automaticValidated: Bool,
                        candidateFailure: FailureReason? = nil) -> Resolution {
        let effectiveId: String
        let unavailable: Bool
        switch requestedId {
        case "auto", "cpu", "ane", "gpu":
            effectiveId = requestedId
            unavailable = false
        default:
            effectiveId = "auto"
            unavailable = true
        }

        let candidate: Candidate
        let state: State
        let reason: FailureReason?
        let viaAutomatic = effectiveId == "auto"
        if let candidateFailure, effectiveId == "ane" || effectiveId == "gpu" || (effectiveId == "auto" && automaticValidated) {
            candidate = cpuCandidate
            state = viaAutomatic ? .autoCPU : .failed
            reason = candidateFailure
        } else {
            switch effectiveId {
            case "ane": candidate = Candidate(provider: 1, deviceIndex: 1, activeId: "ane", activeLabel: "Neural Engine")
            case "gpu": candidate = Candidate(provider: 1, deviceIndex: 2, activeId: "gpu", activeLabel: "GPU")
            case "cpu": candidate = cpuCandidate
            default:
                if automaticValidated {
                    candidate = Candidate(provider: 1, deviceIndex: 0, activeId: "auto", activeLabel: "Automatic")
                    let automaticState: State = unavailable ? .unavailable : .ok
                    return Resolution(candidate: candidate,
                                      report: Value(requestedId: requestedId, activeId: candidate.activeId,
                                                    activeLabel: candidate.activeLabel,
                                                    state: automaticState,
                                                    reason: nil), viaAutomatic: true)
                }
                candidate = cpuCandidate
            }
            state = effectiveId == "auto" ? .autoCPU : .ok
            reason = nil
        }
        let finalState: State = unavailable ? .unavailable : state
        return Resolution(candidate: candidate,
                          report: Value(requestedId: requestedId, activeId: candidate.activeId,
                                        activeLabel: candidate.activeLabel, state: finalState, reason: reason),
                          viaAutomatic: viaAutomatic)
    }

    private static let cpuCandidate = Candidate(provider: 0, deviceIndex: 0, activeId: "cpu", activeLabel: "CPU")

    /// Decision retained between detector sessions. Idle release preserves this value; a preference change
    /// creates a new decision, while a runtime error makes CPU sticky until the next preference change.
    struct SessionDecision: Equatable {
        let preference: String
        let activeProvider: Int32
        let validated: Bool
        let shouldRetryFrameOnCPU: Bool

        init(preference: String, automaticValidated: Bool = InferenceDeviceSelection.automaticValidated) {
            self.preference = preference
            self.activeProvider = Self.provider(for: preference, automaticValidated: automaticValidated)
            self.validated = self.activeProvider == 0
            self.shouldRetryFrameOnCPU = false
        }

        private init(preference: String, activeProvider: Int32, validated: Bool, retry: Bool) {
            self.preference = preference
            self.activeProvider = activeProvider
            self.validated = validated
            self.shouldRetryFrameOnCPU = retry
        }

        var shouldValidateCandidate: Bool { activeProvider != 0 && !validated }

        func afterValidation() -> SessionDecision {
            SessionDecision(preference: preference, activeProvider: activeProvider, validated: true, retry: false)
        }

        func afterRuntimeError() -> SessionDecision {
            SessionDecision(preference: preference, activeProvider: 0, validated: true, retry: true)
        }

        func afterCandidateFailure() -> SessionDecision {
            SessionDecision(preference: preference, activeProvider: 0, validated: true, retry: false)
        }

        func afterIdleRelease() -> SessionDecision {
            SessionDecision(preference: preference, activeProvider: activeProvider, validated: validated, retry: false)
        }

        func settingChanged(to value: String) -> SessionDecision {
            SessionDecision(preference: value)
        }

        private static func provider(for preference: String, automaticValidated: Bool) -> Int32 {
            preference == "ane" || preference == "gpu" || ((preference == "auto" || !["cpu", "ane", "gpu"].contains(preference)) && automaticValidated) ? 1 : 0
        }
    }

    final class Report: ObservableObject {
        @Published private(set) var value = Value(requestedId: "auto", activeId: "cpu", activeLabel: "CPU",
                                                  state: .autoCPU, reason: nil)
        @Published private(set) var decided = false

        func update(_ value: Value, decided: Bool) {
            let publish = { [weak self] in
                self?.value = value
                self?.decided = decided
            }
            if Thread.isMainThread { publish() } else { DispatchQueue.main.async(execute: publish) }
        }
    }
}
