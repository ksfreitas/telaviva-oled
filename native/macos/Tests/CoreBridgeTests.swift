import Foundation
import CoreGraphics
import IOKit.hidsystem

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("CoreBridgeTests: \(message)") }
}

@main
private struct CoreBridgeTests {
    static func main() {
        asynchronousManualControlUsesCoreReceiptAndCompletion()
        refreshBurstsAreConflated()
        hardwareBrightnessKeyDuringProtections()
        print("Core bridge asynchronous tests passed")
    }

    private static func asynchronousManualControlUsesCoreReceiptAndCompletion() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(bridge.snapshot.revision == nil, "core snapshot starts unpublished")
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published asynchronously")
        let initialRevision = bridge.snapshot.revision!

        expect(bridge.submit(["type": "monitor_discovered", "monitor": [
            "id": "synthetic-monitor", "name": "Synthetic", "identity_status": "stable",
            "connection_path": "test", "brightness": ["supported": true, "value": 41, "raw": 41, "min": 0, "max": 100],
            "volume": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100]
        ]]), "synthetic monitor is accepted by the local core")
        bridge.refresh()
        expect(waitUntil { bridge.snapshot.monitors?.contains(where: { $0.id == "synthetic-monitor" }) == true },
               "synthetic monitor appears in the decoded snapshot")
        let monitor = bridge.snapshot.monitors!.first(where: { $0.id == "synthetic-monitor" })!
        var submission: ManualSubmissionResult?
        let controls = ManualMonitorControls { command, completion in
            bridge.submitManualControl(command) { result in
                submission = result
                completion(result)
            }
        }
        controls.apply(bridge.snapshot)
        var completionCount = 0
        bridge.onHardwareActionCompleted = { id, actionID, actionGeneration, connectionGeneration, feature, success, value in
            completionCount += 1
            controls.actionCompleted(monitorID: id, actionID: actionID, actionGeneration: actionGeneration,
                                     connectionGeneration: connectionGeneration, feature: feature,
                                     success: success, confirmedValue: value)
        }
        controls.setValue(73, monitor: monitor, feature: .brightness)
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 73,
               "optimistic slider value is visible immediately")
        expect(waitUntil {
            bridge.snapshot.pending_actions?.contains(where: { $0.monitor_id == monitor.id && $0.kind == "set_brightness" }) == true
        }, "manual command is submitted through the Rust bridge")
        expect(waitUntil { controls.activeCountForTesting == 1 }, "submission receipt does not finish physical action")
        let action = bridge.snapshot.pending_actions!.first(where: { $0.monitor_id == monitor.id && $0.kind == "set_brightness" })!
        expect(waitUntil { submission != nil }, "asynchronous submission returns a core receipt")
        expect(submission?.accepted == true && submission?.actionGeneration == action.generation,
               "submission receipt matches the pending action generation")
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 73,
               "accepted command keeps the local draft until hardware completion")

        controls.actionCompleted(monitorID: monitor.id, actionID: action.id,
                                 actionGeneration: action.generation &+ 1,
                                 connectionGeneration: monitor.generation!, feature: .brightness,
                                 success: true, confirmedValue: 73)
        expect(controls.activeCountForTesting == 1, "unrelated action generation is ignored")
        expect(bridge.submitHardwareResult(action, connectionGeneration: monitor.generation!, success: true, value: 73),
               "synthetic hardware result is accepted without a physical device")
        expect(waitUntil { completionCount == 1 && controls.activeCountForTesting == 0 },
               "real hardware-result callback settles the matching manual action")
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 73,
               "confirmed completion adopts the verified value")

        let stale = ManualControlCommand(monitorID: monitor.id, generation: monitor.generation!,
                                         feature: .volume, value: 60, revision: 999,
                                         authorization: ManualCommandAuthorization())
        stale.authorization.invalidate()
        var staleAccepted: Bool?
        bridge.submitManualControl(stale) { result in staleAccepted = result.accepted }
        expect(waitUntil { staleAccepted != nil }, "invalidated queued command receives a result")
        expect(staleAccepted == false, "invalidated command never reaches Rust")

        let beforeShutdown = bridge.snapshot.revision ?? 0
        var shutdownRevision: UInt64?
        bridge.beginShutdown {
            shutdownRevision = bridge.snapshot.revision
        }
        expect(waitUntil { shutdownRevision != nil }, "shutdown completion runs after a fresh snapshot is published")
        expect(shutdownRevision! > beforeShutdown, "shutdown callback observes the final core revision")
        expect(shutdownRevision == bridge.snapshot.revision,
               "shutdown completion sees the same revision that the bridge published")
        expect(bridge.snapshot.revision! > initialRevision, "core revisions advance through manual and shutdown work")
    }

    private static func refreshBurstsAreConflated() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the burst")

        // Only the first refresh run blocks; every later run passes straight through.
        let entered = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let runLock = NSLock()
        var runCount = 0
        func currentRunCount() -> Int {
            runLock.lock(); defer { runLock.unlock() }
            return runCount
        }
        bridge.refreshRunObserver = {
            runLock.lock()
            runCount += 1
            let isFirstRun = runCount == 1
            runLock.unlock()
            guard isFirstRun else { return }
            entered.signal()
            // Bounded wait so a regression fails the assertions instead of hanging the test.
            _ = release.wait(timeout: .now() + 5)
        }

        bridge.refresh()
        expect(entered.wait(timeout: .now() + 5) == .success, "first refresh run reaches the observer")
        for _ in 0..<50 { bridge.refresh() }
        release.signal()

        expect(waitUntil { currentRunCount() >= 2 }, "conflated refresh schedules one re-run")
        Thread.sleep(forTimeInterval: 0.3)
        expect(currentRunCount() == 2, "refresh burst runs exactly one re-run")

        bridge.refresh()
        expect(waitUntil { currentRunCount() >= 3 }, "bridge refreshes again after a burst")
        bridge.refreshRunObserver = nil
    }

    private static func hardwareBrightnessKeyDuringProtections() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the key test")

        // Mirrors generation_checked_manual_command_keeps_existing_behavior_during_active_protections in the Rust contract tests.
        expect(bridge.submit(["type": "monitor_discovered", "monitor": [
            "id": "screen", "name": "Display", "identity_status": "stable", "connection_path": "usb",
            "brightness": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100],
            "volume": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100]
        ]]), "synthetic screen is accepted by the local core")
        expect(bridge.submit(["type": "update_settings", "settings": [
            "presence_protection_enabled": true, "screensaver_protection_enabled": true,
            "screensaver_targets": ["screen": 5.0]
        ]]), "protection settings are accepted by the local core")
        expect(bridge.submit(["type": "power", "on_battery": false]), "power state is accepted by the local core")
        expect(bridge.submit(["type": "screensaver", "state": "active"]), "screensaver activation is accepted by the local core")
        bridge.refresh()
        // The initial snapshot already has a camera generation, so wait for a snapshot that reflects the commands above.
        expect(waitUntil {
            bridge.snapshot.settings?.presence_protection_enabled == true &&
                bridge.snapshot.settings?.screensaver_protection_enabled == true &&
                bridge.snapshot.monitors?.contains(where: { $0.id == "screen" }) == true
        }, "protection settings and screen are published")
        // Observations are only accepted in face mode, which requires power to be applied (on_battery false).
        expect(waitUntil { bridge.snapshot.camera?.mode == "face" }, "camera reaches face mode")
        let cameraGeneration = bridge.snapshot.camera!.generation!
        // Same clock basis as the Rust capture_time helper: Unix milliseconds minus 60 s plus the offset.
        // Integer milliseconds are required because the core reads captured_at_ms as an unsigned integer.
        let nowMs = UInt64(Date().timeIntervalSince1970 * 1000)
        for offset in [10_000, 20_000] as [UInt64] {
            expect(bridge.submit(["type": "presence_observation", "status": "absent",
                                  "captured_at_ms": nowMs - 60_000 + offset, "camera_generation": cameraGeneration]),
                   "absent presence observation is accepted by the local core")
        }
        expect(waitUntil {
            bridge.snapshot.monitors?.contains(where: { $0.id == "screen" && $0.presence_dimmed == true }) == true
        }, "presence protection dims the screen before the key")

        // The real router enables itself in processForTesting, so isCommandCurrent can validate the reserved intent headlessly.
        let router = HardwareKeyRouter(submit: { command, completion in
            DispatchQueue.main.async { completion(bridge.submitHardwareKey(command)) }
        }, permissionProvider: { HardwareKeyPermissions(accessibility: true, inputMonitoring: true) })
        bridge.hardwareKeyValidator = { router.isCommandCurrent($0) }
        var keyAccepted: Bool?
        var keyActionGeneration: UInt64?
        var keyTarget: Double?
        bridge.onHardwareKeyAccepted = { command, accepted, actionGeneration in
            keyTarget = command.target
            keyAccepted = accepted
            keyActionGeneration = actionGeneration
        }

        func screen() -> CoreMonitor? { bridge.snapshot.monitors?.first(where: { $0.id == "screen" }) }
        let monitor = screen()!
        expect(monitor.brightness?.value != nil, "screen reports a brightness value")
        let observed = monitor.brightness!.value!
        let expectedTarget = observed + HardwareKeyPolicy.normalStep
        let displays = [HardwareKeyDisplay(displayID: 7, bounds: CGRect(x: 0, y: 0, width: 1000, height: 800),
                                           isBuiltin: false, monitorID: "screen", generation: monitor.generation!,
                                           audioDeviceUID: nil,
                                           brightness: HardwareKeyFeatureState(value: observed, supported: true, awake: true),
                                           volume: HardwareKeyFeatureState(value: monitor.volume?.value,
                                                                           supported: monitor.volume?.supported ?? false, awake: true))]
        let pointer = CGPoint(x: 500, y: 400)
        let brightnessUp = Int(NX_KEYTYPE_BRIGHTNESS_UP)
        let down = HardwareKeyEvent(keyIdentity: brightnessUp, feature: .brightness, direction: .increase, phase: .down,
                                    isRepeat: false, isFineStep: false, modifiersEligible: true)
        let up = HardwareKeyEvent(keyIdentity: brightnessUp, feature: .brightness, direction: .increase, phase: .up,
                                  isRepeat: false, isFineStep: false, modifiersEligible: true)
        expect(router.processForTesting(down, pointer: pointer, displays: displays), "brightness key down is handled by the router")
        expect(router.processForTesting(up, pointer: pointer, displays: displays), "brightness key up is consumed with its down")

        expect(waitUntil { keyAccepted == true && keyActionGeneration != nil },
               "core accepts the hardware brightness command")
        expect(keyTarget.map { abs($0 - expectedTarget) < 0.001 } == true,
               "router target is the observed value plus one normal step")
        expect(waitUntil {
            bridge.snapshot.pending_actions?.contains(where: { $0.monitor_id == "screen" && $0.kind == "set_brightness" }) == true
        }, "accepted hardware key is queued as a set_brightness action")
        expect(waitUntil { abs((screen()?.brightness?.desired ?? -1) - expectedTarget) < 0.001 },
               "manual target is recorded as the desired brightness")
        expect(screen()?.presence_dimmed == true, "manual key keeps presence protection active")

        router.shutdown()
        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "shutdown completes before the temporary directory is removed")
    }

    private static func waitUntil(timeout: TimeInterval = 5, _ predicate: () -> Bool) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while !predicate() && Date() < deadline {
            _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.01))
        }
        return predicate()
    }
}
