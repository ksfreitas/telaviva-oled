import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("InferenceDeviceSelectionTests: \(message)") }
}

@main
private struct InferenceDeviceSelectionTests {
    static func main() {
        let options = InferenceDeviceSelection.options
        expect(options.map(\.id) == ["auto", "ane", "gpu", "cpu"], "stable option order and ids")
        expect(options.map(\.kind) == ["auto", "ane", "gpu", "cpu"], "locale-independent option kinds")

        let auto = InferenceDeviceSelection.resolve(requestedId: "auto", automaticValidated: false)
        expect(auto.candidate.provider == 0 && auto.report.state == .autoCPU,
               "unvalidated Automatic stays on CPU")
        expect(auto.candidate.activeId == "cpu", "unvalidated Automatic reports CPU")

        let validatedAuto = InferenceDeviceSelection.resolve(requestedId: "auto", automaticValidated: true)
        expect(validatedAuto.candidate.provider == 1 && validatedAuto.candidate.deviceIndex == 0,
               "validated Automatic maps to CoreML ALL")

        let ane = InferenceDeviceSelection.resolve(requestedId: "ane", automaticValidated: false)
        expect(ane.candidate.provider == 1 && ane.candidate.deviceIndex == 1,
               "Neural Engine preference maps to CPUAndNeuralEngine")
        let gpu = InferenceDeviceSelection.resolve(requestedId: "gpu", automaticValidated: false)
        expect(gpu.candidate.provider == 1 && gpu.candidate.deviceIndex == 2,
               "GPU preference maps to CPUAndGPU")
        let cpu = InferenceDeviceSelection.resolve(requestedId: "cpu", automaticValidated: true)
        expect(cpu.candidate.provider == 0 && cpu.report.state == .ok, "CPU is explicit and ready")

        let foreignId = InferenceDeviceSelection.resolve(requestedId: "gpu:1002:164e:0", automaticValidated: false)
        expect(foreignId.report.state == .unavailable && foreignId.candidate.provider == 0,
               "an id that does not apply stays stored and runs via Automatic")

        let failed = InferenceDeviceSelection.resolve(requestedId: "ane", automaticValidated: false,
                                                       candidateFailure: .selfTestMismatch)
        expect(failed.report.state == .failed && failed.report.activeId == "cpu",
               "a failed explicit candidate falls back to CPU and reports failure")
        let failedAuto = InferenceDeviceSelection.resolve(requestedId: "auto", automaticValidated: true,
                                                           candidateFailure: .sessionFailed)
        expect(failedAuto.report.state == .autoCPU && failedAuto.report.activeId == "cpu",
               "Automatic candidate failure falls back to CPU")

        var session = InferenceDeviceSelection.Session(preference: "ane")
        expect(session.candidate.provider == 1 && session.decision.shouldValidateCandidate,
               "accelerated candidate needs first-session validation")
        session.candidateAccepted()
        session.idleReleased()
        expect(session.candidate.provider == 1 && !session.decision.shouldValidateCandidate,
               "accepted decision is retained for idle recreation without another self-test")
        session.runtimeFailed()
        expect(session.candidate.provider == 0 && session.decision.shouldRetryFrameOnCPU,
               "runtime failure changes the session to CPU and retries the current frame")
        session.idleReleased()
        expect(session.candidate.provider == 0 && !session.decision.shouldValidateCandidate,
               "idle recreation keeps CPU fallback for this preference session")
        session.preferenceChanged(to: "gpu")
        expect(session.decision.shouldValidateCandidate && session.candidate.provider == 1 && session.preference == "gpu",
               "a changed preference clears the old decision and starts a new validation")

        let autoSession = InferenceDeviceSelection.Session(preference: "auto", automaticValidated: true)
        expect(autoSession.candidate.provider == 1 && autoSession.decision.shouldValidateCandidate,
               "validated Automatic still validates CoreML once per preference session")
        let unavailableAutomatic = InferenceDeviceSelection.Session(preference: "gpu:1002:164e:0",
                                                                     automaticValidated: true)
        expect(unavailableAutomatic.candidate.provider == 1 && unavailableAutomatic.decision.shouldValidateCandidate
                && unavailableAutomatic.resolution.report.state == .unavailable,
               "an unavailable stored id follows validated Automatic while retaining its notice")

        var nextHandle = 0
        var handleProviders: [Int: Int32] = [:]
        var createdProviders: [Int32] = []
        var validatedCount = 0
        var published: [(InferenceDeviceSelection.Value, Bool)] = []
        let host = InferenceDeviceSelection.DetectorHost<Int>(
            session: InferenceDeviceSelection.Session(preference: "ane"),
            create: { candidate in
                nextHandle += 1
                handleProviders[nextHandle] = candidate.provider
                createdProviders.append(candidate.provider)
                return nextHandle
            },
            destroy: { _ in },
            selectedProvider: { handleProviders[$0] ?? -1 },
            validate: { _ in validatedCount += 1; return nil },
            publish: { published.append(($0, $1)) })
        var frameResult = 0
        let runtimeStatus = host.detect(&frameResult) { handle, result in
            if handleProviders[handle] == 1 { return -1 }
            result = 1
            return 0
        }
        expect(runtimeStatus == 0 && frameResult == 1 && createdProviders == [1, 0],
               "production detector host retries a failed accelerated frame on CPU")
        expect(host.session.candidate.provider == 0 && host.session.decision.shouldRetryFrameOnCPU,
               "runtime fallback becomes the retained CPU decision")
        expect(published.last?.0.state == .failed && published.last?.0.reason == .runtimeError,
               "runtime failure is visible in the report")
        host.release()
        expect(host.ensure() && createdProviders == [1, 0, 0] && validatedCount == 1,
               "idle recreation stays on CPU and does not rerun the accelerator self-test")
        host.setPreference("gpu")
        expect(host.ensure() && createdProviders.last == 1 && validatedCount == 2,
               "a new preference resets fallback and validates its selected candidate")

        let failedHost = InferenceDeviceSelection.DetectorHost<Int>(
            session: InferenceDeviceSelection.Session(preference: "gpu"),
            create: { $0.provider == 1 ? nil : 7 }, destroy: { _ in },
            selectedProvider: { _ in 0 }, validate: { _ in nil }, publish: { published.append(($0, $1)) })
        expect(failedHost.ensure() && failedHost.session.resolution.report.state == .failed
                && failedHost.session.resolution.report.reason == .sessionFailed,
               "candidate creation failure is reported and retains a working CPU session")

        let registerFailureHost = InferenceDeviceSelection.DetectorHost<Int>(
            session: InferenceDeviceSelection.Session(preference: "ane"),
            create: { $0.provider == 0 ? 8 : 9 }, destroy: { _ in },
            selectedProvider: { _ in 0 }, validate: { _ in nil }, publish: { published.append(($0, $1)) })
        expect(registerFailureHost.ensure() && registerFailureHost.session.resolution.report.state == .failed
                && registerFailureHost.session.resolution.report.reason == .registerFailed,
               "provider registration mismatch is rejected and reported")

        for reason in [InferenceDeviceSelection.FailureReason.selfTestMismatch, .selfTestSlow] {
            var providers: [Int32] = []
            let validationFailureHost = InferenceDeviceSelection.DetectorHost<Int>(
                session: InferenceDeviceSelection.Session(preference: "ane"),
                create: { providers.append($0.provider); return providers.count }, destroy: { _ in },
                selectedProvider: { _ in 1 }, validate: { _ in reason },
                publish: { published.append(($0, $1)) })
            expect(validationFailureHost.ensure() && providers == [1, 0]
                    && validationFailureHost.session.resolution.report.state == .failed
                    && validationFailureHost.session.resolution.report.reason == reason,
                   "\(reason.rawValue) rejects the candidate and keeps a CPU detector")
        }

        let report = InferenceDeviceSelection.Report()
        expect(!report.decided, "report starts undecided while session creation is lazy")
        report.update(auto.report, decided: false)
        expect(!report.decided && report.value.requestedId == "auto", "undecided report can show its predicted route")

        print("Inference device selection tests passed")
    }
}
