import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("ManualMonitorControlsTests: \(message)") }
}

private final class FakeSubmitter {
    private let condition = NSCondition()
    private var storedCommands: [ManualControlCommand] = []
    private var completions: [(ManualSubmissionResult) -> Void] = []
    private var blocked = false
    private var enteredBlockedSubmit = false

    var commands: [ManualControlCommand] {
        condition.lock(); defer { condition.unlock() }
        return storedCommands
    }

    func submit(_ command: ManualControlCommand, completion: @escaping (ManualSubmissionResult) -> Void) {
        condition.lock()
        storedCommands.append(command)
        completions.append(completion)
        if blocked {
            enteredBlockedSubmit = true
            condition.broadcast()
            while blocked { condition.wait() }
        }
        condition.broadcast()
        condition.unlock()
    }

    func holdSubmissions() {
        condition.lock(); blocked = true; enteredBlockedSubmit = false; condition.unlock()
    }

    func waitUntilSubmissionEnteredBlock() {
        condition.lock()
        let deadline = Date(timeIntervalSinceNow: 3)
        while !enteredBlockedSubmit && condition.wait(until: deadline) {}
        let entered = enteredBlockedSubmit
        condition.unlock()
        expect(entered, "fake submission reached its deterministic barrier")
    }

    func releaseSubmissions() {
        condition.lock(); blocked = false; condition.broadcast(); condition.unlock()
    }

    func acknowledge(_ index: Int, accepted: Bool) {
        condition.lock(); let completion = completions[index]; condition.unlock()
        completion(ManualSubmissionResult(accepted: accepted, actionGeneration: accepted ? UInt64(index + 100) : nil))
    }
}

@main
private struct ManualMonitorControlsTests {
    static func main() {
        idleAndImmediateDraft()
        rapidInputCoalescesBehindHeldSubmission()
        staleFailureCannotReplaceFreshDraft()
        finalValueReturningToConfirmedIsRetained()
        externalKeyboardIntentCannotOverwriteNewSliderInput()
        keyboardVolumeDoesNotCancelUnacceptedBrightness()
        earlyHardwareCompletionWaitsForSubmissionReceipt()
        protectionBrightnessCapTracksPendingAndFailedWrites()
        independentMonitorsAndFeatures()
        duplicateRawValuesAndBound()
        disconnectSleepAndShutdownRevokeUnsentAuthorization()
        wakeAfterSleepDoesNotReplayPendingDraft()
        generationRolloverAndShutdownIgnoreLateCompletion()
        print("Manual monitor control tests passed")
    }

    private static func idleAndImmediateDraft() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 41, "confirmed value is used while idle")
        expect(fake.commands.isEmpty && controls.pendingCountForTesting == 0 && controls.activeCountForTesting == 0,
               "idle control performs no submission work")
        controls.setValue(62, monitor: monitor, feature: .brightness)
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 62,
               "local value changes synchronously before submission")
        expect(controls.status(monitor: monitor, feature: .brightness) == .pending, "draft is marked pending")
        expect(waitUntil { fake.commands.count == 1 }, "one command is submitted asynchronously")
        fake.acknowledge(0, accepted: false)
        expect(waitUntil { controls.status(monitor: monitor, feature: .brightness) == .failed },
               "rejected submission is exposed as failed")
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 41,
               "rejection reconciles to the last confirmed value")
    }

    private static func rapidInputCoalescesBehindHeldSubmission() {
        let fake = FakeSubmitter()
        fake.holdSubmissions()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        var acceptedRevisions = Set<UInt64>()
        controls.onAccepted = { _, _, _, _, revision, accepted in
            if accepted { acceptedRevisions.insert(revision) }
        }
        controls.setEditing(true, monitor: monitor, feature: .brightness)
        controls.setValue(0, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "first rapid value reaches worker")
        fake.waitUntilSubmissionEnteredBlock()

        controls.setValue(100, monitor: monitor, feature: .brightness)
        controls.setValue(37, monitor: monitor, feature: .brightness)
        expect(!fake.commands[0].authorization.claim(),
               "new slider input invalidates an older command still blocked before acceptance")
        var heartbeat = 0
        heartbeat += 1
        expect(heartbeat == 1, "main-thread heartbeat runs while fake submission is blocked")
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 37,
               "rapid pointer values update the draft immediately")
        expect(controls.activeCountForTesting == 1 && controls.pendingCountForTesting == 1,
               "one update is active and only the latest target is pending")

        fake.releaseSubmissions()
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[0].revision) }, "first submission is accepted")
        expect(fake.commands.count == 1 && controls.activeCountForTesting == 1,
               "accepted submission stays active until hardware completion")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 0)
        expect(waitUntil { fake.commands.count == 2 }, "latest target dispatches after hardware completion")
        expect(fake.commands.map(\.value) == [0, 37], "intermediate value 100 is replaced by final value 37")
        fake.acknowledge(1, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[1].revision) }, "second command is accepted")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[1].revision,
                                 actionGeneration: 101, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 37)
        expect(waitUntil { controls.activeCountForTesting == 0 && controls.status(monitor: monitor, feature: .brightness) == .ready },
               "completion settles the final released value")
        controls.setEditing(false, monitor: monitor, feature: .brightness)
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 37,
               "non-drag release preserves the confirmed final value")
    }

    private static func staleFailureCannotReplaceFreshDraft() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        var acceptedRevisions = Set<UInt64>()
        controls.onAccepted = { _, _, _, _, revision, accepted in
            if accepted { acceptedRevisions.insert(revision) }
        }
        controls.setValue(20, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "older intent reaches worker")
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[0].revision) }, "older intent is accepted")
        expect(controls.activeCountForTesting == 1, "accepted intent remains unfinished")
        controls.setValue(37, monitor: monitor, feature: .brightness)
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: false, confirmedValue: 41)
        expect(waitUntil { fake.commands.count == 2 }, "newer intent remains eligible after older failure")
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 37,
               "older failed completion cannot overwrite the fresh draft")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 999, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 20)
        expect(controls.activeCountForTesting == 1 && controls.displayValue(monitor: monitor, feature: .brightness) == 37,
               "unrelated action generation cannot complete or overwrite the newer draft")
        fake.acknowledge(1, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[1].revision) }, "newer intent is accepted")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[1].revision,
                                 actionGeneration: 101, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 37)
        expect(waitUntil { controls.displayValue(monitor: monitor, feature: .brightness) == 37 && controls.activeCountForTesting == 0 },
               "newer completion reconciles its own draft")
    }

    private static func finalValueReturningToConfirmedIsRetained() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        var acceptedRevisions = Set<UInt64>()
        controls.onAccepted = { _, _, _, _, revision, accepted in
            if accepted { acceptedRevisions.insert(revision) }
        }
        controls.setValue(20, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "initial direction change reaches the worker")
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[0].revision) }, "initial direction change is accepted")
        controls.setValue(41, monitor: monitor, feature: .brightness)
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 41,
               "newest local value can return to the prior confirmed target")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 20)
        expect(waitUntil { fake.commands.count == 2 }, "final target returning to confirmed value is retained")
        expect(fake.commands.map(\.value) == [20, 41], "the newest target is not lost during coalescing")
        fake.acknowledge(1, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[1].revision) }, "final target is accepted")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[1].revision,
                                 actionGeneration: 101, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 41)
        expect(waitUntil { controls.activeCountForTesting == 0 && controls.status(monitor: monitor, feature: .brightness) == .ready },
               "returning to confirmed target settles cleanly")
    }

    private static func independentMonitorsAndFeatures() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        var acceptedRevisions = Set<UInt64>()
        controls.onAccepted = { _, _, _, _, revision, accepted in
            if accepted { acceptedRevisions.insert(revision) }
        }
        let first = display(id: "monitor-a")
        let second = display(id: "monitor-b")
        controls.apply(snapshot([first, second]))
        controls.setValue(70, monitor: first, feature: .brightness)
        controls.setValue(30, monitor: first, feature: .volume)
        controls.setValue(80, monitor: second, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "first independent monitor target is submitted")
        expect(controls.displayValue(monitor: first, feature: .volume) == 30 &&
               controls.displayValue(monitor: second, feature: .brightness) == 80,
               "each monitor and feature keeps its own local draft")
        for index in 0..<3 {
            let command = fake.commands[index]
            fake.acknowledge(index, accepted: true)
            expect(waitUntil { acceptedRevisions.contains(command.revision) }, "independent command is accepted")
            controls.actionCompleted(monitorID: command.monitorID, actionID: command.revision,
                                     actionGeneration: UInt64(index + 100), connectionGeneration: command.generation,
                                     feature: command.feature, success: true, confirmedValue: command.value)
            if index < 2 {
                expect(waitUntil { fake.commands.count == index + 2 }, "next independent target dispatches")
            }
        }
        expect(Set(fake.commands.map { "\($0.monitorID):\($0.feature)" }).count == 3,
               "monitor identity and feature remain distinct")
    }

    private static func externalKeyboardIntentCannotOverwriteNewSliderInput() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        let receipt = controls.adoptExternalIntent(monitorID: monitor.id, generation: 1,
                                                  feature: .brightness, value: 56, sourceToken: 11)
        expect(receipt != nil && controls.displayValue(monitor: monitor, feature: .brightness) == 56,
               "keyboard target rebases the local presentation")
        expect(fake.commands.isEmpty && controls.pendingCountForTesting == 0,
               "external keyboard intent is not submitted twice by slider coordinator")
        controls.bindExternalReceipt(monitorID: monitor.id, generation: 1, feature: .brightness,
                                     revision: receipt!, actionGeneration: 401)
        controls.setValue(73, monitor: monitor, feature: .brightness)
        controls.rejectExternalIntent(monitorID: monitor.id, generation: 1, feature: .brightness,
                                      revision: receipt!)
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 73,
               "late keyboard rejection cannot replace a newer slider draft")
        expect(controls.pendingCountForTesting == 1, "new slider intent remains queued after keyboard race")
    }

    private static func keyboardVolumeDoesNotCancelUnacceptedBrightness() {
        let fake = FakeSubmitter()
        fake.holdSubmissions()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        controls.setValue(70, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "brightness command reaches blocked worker")
        fake.waitUntilSubmissionEnteredBlock()
        let revision = controls.adoptExternalIntent(monitorID: monitor.id, generation: 1,
                                                   feature: .volume, value: 60)
        expect(revision != nil && controls.displayValue(monitor: monitor, feature: .volume) == 60,
               "keyboard volume intent is adopted independently")
        expect(fake.commands[0].authorization.claim(),
               "volume keyboard intent leaves the brightness authorization valid")
        fake.releaseSubmissions()
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { controls.activeCountForTesting == 1 }, "brightness remains active after volume intent")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 70)
        expect(waitUntil { controls.activeCountForTesting == 0 }, "brightness completes independently")
        expect(controls.displayValue(monitor: monitor, feature: .volume) == 60,
               "brightness completion does not overwrite the keyboard volume draft")
    }

    private static func earlyHardwareCompletionWaitsForSubmissionReceipt() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        controls.setValue(63, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "manual command reaches the fake worker")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 63)
        expect(controls.activeCountForTesting == 1 &&
               controls.displayValue(monitor: monitor, feature: .brightness) == 63,
               "early DDC completion is retained while submission receipt is pending")
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { controls.activeCountForTesting == 0 &&
                           controls.status(monitor: monitor, feature: .brightness) == .ready },
               "matching acceptance receipt applies the buffered hardware completion")
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 63,
               "buffered completion settles the confirmed value")
    }

    private static func protectionBrightnessCapTracksPendingAndFailedWrites() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display()
        controls.apply(snapshot([monitor]))
        var acceptedRevisions = Set<UInt64>()
        controls.onAccepted = { _, _, _, _, revision, accepted in
            if accepted { acceptedRevisions.insert(revision) }
        }
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 41,
               "protection brightness begins at the confirmed value")
        controls.setValue(80, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "increase reaches worker")
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 41,
               "pending increase cannot widen the protection cap")
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[0].revision) }, "increase is accepted")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 80)
        expect(waitUntil { controls.activeCountForTesting == 0 }, "increase completion updates confirmed brightness")
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 80,
               "successful increase widens the cap after confirmation")

        controls.setValue(20, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 2 }, "decrease reaches worker")
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 20,
               "pending decrease immediately lowers the protection cap")
        fake.acknowledge(1, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[1].revision) }, "decrease is accepted")
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[1].revision,
                                 actionGeneration: 101, connectionGeneration: 1, feature: .brightness,
                                 success: false)
        expect(waitUntil { controls.activeCountForTesting == 0 && controls.status(monitor: monitor, feature: .brightness) == .failed },
               "failed decrease reconciles to confirmed brightness")
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 80 &&
               controls.displayValue(monitor: monitor, feature: .brightness) == 80,
               "failed decrease restores the last confirmed value and cap")
    }

    private static func duplicateRawValuesAndBound() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        let narrow = display(brightnessMin: 0, brightnessMax: 10)
        controls.apply(snapshot([narrow]))
        controls.setValue(51, monitor: narrow, feature: .brightness)
        controls.setValue(52, monitor: narrow, feature: .brightness)
        expect(controls.pendingCountForTesting == 1, "same rounded raw target is deduplicated")
        expect(controls.displayValue(monitor: narrow, feature: .brightness) == 52,
               "duplicate raw target keeps the newest local presentation without redundant intent")

        let bounded = ManualMonitorControls(submit: fake.submit)
        var rejectedTargets = 0
        bounded.onRejected = { _, _, _ in rejectedTargets += 1 }
        var monitors: [CoreMonitor] = []
        for index in 0..<129 {
            let monitor = display(id: "monitor-\(index)")
            monitors.append(monitor)
        }
        bounded.apply(snapshot(monitors))
        for monitor in monitors {
            bounded.setValue(60, monitor: monitor, feature: .brightness)
        }
        expect(bounded.pendingCountForTesting == 128, "tracked pending targets are capped at 128")
        expect(bounded.displayValue(monitor: monitors[128], feature: .brightness) == 41,
               "excess target does not appear as an optimistic value")
        expect(rejectedTargets == 1, "overflow is reported through the rejection hook")
    }

    private static func generationRolloverAndShutdownIgnoreLateCompletion() {
        let fake = FakeSubmitter()
        let controls = ManualMonitorControls(submit: fake.submit)
        var acceptedRevisions = Set<UInt64>()
        controls.onAccepted = { _, _, _, _, revision, accepted in
            if accepted { acceptedRevisions.insert(revision) }
        }
        let old = display(generation: 1)
        controls.apply(snapshot([old]))
        controls.setValue(20, monitor: old, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "old generation reaches worker")
        fake.acknowledge(0, accepted: true)
        expect(waitUntil { acceptedRevisions.contains(fake.commands[0].revision) }, "old generation command is accepted")
        expect(controls.activeCountForTesting == 1, "old generation command is active")
        let current = display(generation: 2)
        controls.apply(snapshot([current]))
        controls.setValue(70, monitor: current, feature: .brightness)
        controls.actionCompleted(monitorID: old.id, actionID: fake.commands[0].revision,
                                 actionGeneration: 100, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 20)
        expect(waitUntil { fake.commands.count == 2 }, "fresh generation gets its own command")
        expect(fake.commands[1].generation == 2 && controls.displayValue(monitor: current, feature: .brightness) == 70,
               "late old-generation completion does not alter the current draft")

        controls.shutdown()
        fake.acknowledge(1, accepted: true)
        controls.actionCompleted(monitorID: current.id, actionID: fake.commands[1].revision,
                                 actionGeneration: 101, connectionGeneration: 2, feature: .brightness,
                                 success: true, confirmedValue: 70)
        expect(waitUntil { controls.pendingCountForTesting == 0 }, "shutdown clears unsent work")
        expect(fake.commands.count == 2, "late callback after shutdown cannot dispatch new work")
    }

    private static func disconnectSleepAndShutdownRevokeUnsentAuthorization() {
        let disconnectedSubmitter = FakeSubmitter()
        disconnectedSubmitter.holdSubmissions()
        let disconnected = ManualMonitorControls(submit: disconnectedSubmitter.submit)
        let monitor = display()
        disconnected.apply(snapshot([monitor]))
        disconnected.setValue(25, monitor: monitor, feature: .brightness)
        expect(waitUntil { disconnectedSubmitter.commands.count == 1 }, "disconnect case reaches blocked worker")
        disconnectedSubmitter.waitUntilSubmissionEnteredBlock()
        disconnected.apply(snapshot([]))
        expect(!disconnectedSubmitter.commands[0].authorization.claim(),
               "disconnect revokes a not-yet-accepted hardware command")
        disconnectedSubmitter.releaseSubmissions()

        let sleepingSubmitter = FakeSubmitter()
        sleepingSubmitter.holdSubmissions()
        let sleeping = ManualMonitorControls(submit: sleepingSubmitter.submit)
        let awakeMonitor = display(id: "monitor-sleep")
        sleeping.apply(snapshot([awakeMonitor]))
        sleeping.setValue(25, monitor: awakeMonitor, feature: .brightness)
        expect(waitUntil { sleepingSubmitter.commands.count == 1 }, "sleep case reaches blocked worker")
        sleepingSubmitter.waitUntilSubmissionEnteredBlock()
        var asleepMonitor = display(id: awakeMonitor.id)
        asleepMonitor.display_state = "asleep"
        sleeping.apply(snapshot([asleepMonitor]))
        expect(!sleepingSubmitter.commands[0].authorization.claim(), "sleep revokes a not-yet-accepted command")
        sleepingSubmitter.releaseSubmissions()

        let shutdownSubmitter = FakeSubmitter()
        shutdownSubmitter.holdSubmissions()
        let shuttingDown = ManualMonitorControls(submit: shutdownSubmitter.submit)
        let shutdownMonitor = display(id: "monitor-shutdown")
        shuttingDown.apply(snapshot([shutdownMonitor]))
        shuttingDown.setValue(25, monitor: shutdownMonitor, feature: .brightness)
        expect(waitUntil { shutdownSubmitter.commands.count == 1 }, "shutdown case reaches blocked worker")
        shutdownSubmitter.waitUntilSubmissionEnteredBlock()
        shuttingDown.shutdown()
        expect(!shutdownSubmitter.commands[0].authorization.claim(), "shutdown revokes a not-yet-accepted command")
        shutdownSubmitter.releaseSubmissions()
    }

    private static func wakeAfterSleepDoesNotReplayPendingDraft() {
        let fake = FakeSubmitter()
        fake.holdSubmissions()
        let controls = ManualMonitorControls(submit: fake.submit)
        let monitor = display(id: "monitor-wake")
        controls.apply(snapshot([monitor]))
        controls.setValue(25, monitor: monitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 1 }, "pre-sleep command reaches blocked worker")
        fake.waitUntilSubmissionEnteredBlock()
        // The next target queues behind the blocked command, so it is a pending draft that was never submitted.
        controls.setValue(60, monitor: monitor, feature: .brightness)
        expect(controls.pendingCountForTesting == 1 && controls.activeCountForTesting == 1 &&
               controls.displayValue(monitor: monitor, feature: .brightness) == 60,
               "queued draft waits behind the in-flight command")

        var asleepMonitor = display(id: monitor.id)
        asleepMonitor.display_state = "asleep"
        controls.apply(snapshot([asleepMonitor]))
        expect(controls.pendingCountForTesting == 0 && controls.activeCountForTesting == 0,
               "sleep drops the queued draft")
        // Release the blocked worker first so that any replayed drain could actually reach the fake.
        fake.releaseSubmissions()
        expect(!waitUntil(timeout: 0.2) { fake.commands.count > 1 }, "nothing is submitted while asleep")

        let wokenMonitor = display(id: monitor.id)
        controls.apply(snapshot([wokenMonitor]))
        expect(!waitUntil(timeout: 0.2) { fake.commands.count > 1 }, "wake does not replay the queued draft")
        expect(fake.commands.map(\.value) == [25], "pre-sleep draft value 60 is not submitted after wake")

        controls.setValue(33, monitor: wokenMonitor, feature: .brightness)
        expect(waitUntil { fake.commands.count == 2 }, "new draft after wake reaches the worker")
        expect(fake.commands[1].value == 33, "new draft after wake keeps its own value")
        fake.acknowledge(1, accepted: true)
        controls.actionCompleted(monitorID: monitor.id, actionID: fake.commands[1].revision,
                                 actionGeneration: 101, connectionGeneration: 1, feature: .brightness,
                                 success: true, confirmedValue: 33)
        expect(waitUntil { controls.activeCountForTesting == 0 &&
                           controls.status(monitor: wokenMonitor, feature: .brightness) == .ready },
               "new draft after wake settles normally")
    }

    private static func display(id: String = "monitor-a", generation: UInt64 = 1,
                                brightnessMin: Double = 0, brightnessMax: Double = 100) -> CoreMonitor {
        CoreMonitor(id: id, name: id, presence_dimmed: false, identity_status: "stable", connection_path: nil,
                    generation: generation, display_state: "awake",
                    brightness: VcpValue(supported: true, value: 41, raw: 41, min: brightnessMin,
                                         max: brightnessMax, desired: nil, status: "ready"),
                    volume: VcpValue(supported: true, value: 50, raw: 50, min: 0, max: 100,
                                     desired: nil, status: "ready"))
    }

    private static func snapshot(_ monitors: [CoreMonitor]) -> CoreSnapshot {
        let items: [[String: Any]] = monitors.map { monitor in
            let brightness: [String: Any] = [
                "supported": monitor.brightness?.supported ?? false,
                "normalized": (monitor.brightness?.value as Any?) ?? NSNull(),
                "raw": (monitor.brightness?.raw as Any?) ?? NSNull(),
                "min": monitor.brightness?.min ?? 0, "max": monitor.brightness?.max ?? 100,
                "status": monitor.brightness?.status ?? "ready"
            ]
            let volume: [String: Any] = [
                "supported": monitor.volume?.supported ?? false,
                "normalized": (monitor.volume?.value as Any?) ?? NSNull(),
                "raw": (monitor.volume?.raw as Any?) ?? NSNull(),
                "min": monitor.volume?.min ?? 0, "max": monitor.volume?.max ?? 100,
                "status": monitor.volume?.status ?? "ready"
            ]
            return ["id": monitor.id, "name": monitor.name, "generation": monitor.generation!,
                    "display_state": monitor.display_state ?? "awake",
                    "identity_status": monitor.identity_status ?? "stable",
                    "brightness": brightness, "volume": volume]
        }
        let object: [String: Any] = ["monitors": items]
        return try! JSONDecoder().decode(CoreSnapshot.self, from: JSONSerialization.data(withJSONObject: object))
    }

    private static func waitUntil(timeout: TimeInterval = 3, _ predicate: () -> Bool) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while !predicate() && Date() < deadline {
            _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.01))
        }
        return predicate()
    }
}
