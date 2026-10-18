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
        coreSettingsKeyStepJSON()
        coreSettingsPresenceThresholdJSON()
        coreSettingsInferenceDeviceJSON()
        keyStepSettingsReachRouterThroughCore()
        cameraHistoryDecodesAndStaysOutOfSaves()
        inferenceDeviceRoundTripsThroughCore()
        cameraSelectionRoundTripsThroughCore()
        coreSettingsAmbientFieldsJSON()
        ambientCurvePointsJSON()
        monitorFollowAndProtectionDecode()
        ambientSnapshotDecodes()
        builtInDisplayReportReachesTheSnapshot()
        updateCheckerUsesRealCore()
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
        let expectedTarget = observed + HardwareKeyPolicy.defaultStep
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

    /// Synthesized Decodable requires every non-optional field, even one with a default, so all of them are
    /// supplied here; callers vary only the optional keys in `extra`.
    private static func decodeSettings(_ extra: [String: Any]) -> CoreSettings {
        var object: [String: Any] = [
            "locale": "auto", "presence_sampling_interval_s": 10.0, "return_sampling_interval_s": 1.0,
            "suspend_presence_on_battery": true, "presence_protection_enabled": false,
            "screensaver_protection_enabled": false, "launch_at_login": false,
            "screensaver_targets": [String: Double]()
        ]
        object.merge(extra) { _, new in new }
        let data = try! JSONSerialization.data(withJSONObject: object)
        return try! JSONDecoder().decode(CoreSettings.self, from: data)
    }

    private static func coreSettingsKeyStepJSON() {
        let withKeys = decodeSettings(["brightness_key_step": 3, "volume_key_step": 12])
        expect(withKeys.brightness_key_step == 3 && withKeys.volume_key_step == 12,
               "key steps decode from a snapshot that carries them")
        expect((withKeys.json["brightness_key_step"] as? Int) == 3 && (withKeys.json["volume_key_step"] as? Int) == 12,
               "key steps are emitted as integers when they are set")

        let withoutKeys = decodeSettings([:])
        expect(withoutKeys.brightness_key_step == nil && withoutKeys.volume_key_step == nil,
               "absent key steps decode as nil")
        expect((withoutKeys.json["brightness_key_step"] as? Int) == 5 && (withoutKeys.json["volume_key_step"] as? Int) == 5,
               "absent key steps are emitted as the default of 5")
    }

    private static func coreSettingsPresenceThresholdJSON() {
        // The threshold is optional on decode, and always emitted so an unrelated save keeps the core's value.
        let withThreshold = decodeSettings(["presence_min_brightness_percent": 35])
        expect(withThreshold.presence_min_brightness_percent == 35,
               "presence brightness threshold decodes from a snapshot that carries it")
        expect((withThreshold.json["presence_min_brightness_percent"] as? Double) == 35,
               "presence brightness threshold is emitted as the stored value when set")

        let withoutThreshold = decodeSettings([:])
        expect(withoutThreshold.presence_min_brightness_percent == nil,
               "absent presence brightness threshold decodes as nil")
        expect((withoutThreshold.json["presence_min_brightness_percent"] as? Double) == 20,
               "absent presence brightness threshold is emitted as the default of 20")
    }

    private static func coreSettingsInferenceDeviceJSON() {
        let withDevice = decodeSettings(["inference_device": "ane"])
        expect(withDevice.inference_device == "ane", "inference device decodes from a snapshot that carries it")
        expect((withDevice.json["inference_device"] as? String) == "ane",
               "inference device is emitted when present")

        // Older snapshots do not contain the additive setting; unrelated saves still emit its default.
        let withoutDevice = decodeSettings([:])
        expect(withoutDevice.inference_device == nil, "older snapshots without inference_device still decode")
        expect((withoutDevice.json["inference_device"] as? String) == "auto",
               "an absent inference device is emitted as the default")
    }

    private static func keyStepSettingsReachRouterThroughCore() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the step test")

        expect(bridge.submit(["type": "monitor_discovered", "monitor": [
            "id": "screen", "name": "Display", "identity_status": "stable", "connection_path": "usb",
            "brightness": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100],
            "volume": ["supported": true, "value": 50, "raw": 50, "min": 0, "max": 100]
        ]]), "synthetic screen is accepted by the local core")
        // Saved the same way the Settings window saves: a full CoreSettings through updateSettings.
        var settings = CoreSettings()
        settings.brightness_key_step = 3
        settings.volume_key_step = 12
        bridge.updateSettings(settings)
        bridge.refresh()
        expect(waitUntil {
            bridge.snapshot.settings?.brightness_key_step == 3 && bridge.snapshot.settings?.volume_key_step == 12 &&
                bridge.snapshot.monitors?.contains(where: { $0.id == "screen" }) == true
        }, "key steps 3 and 12 and the synthetic screen are published in the snapshot")

        // MenuBarController feeds the snapshot's steps to the router, with 5 as the fallback when a key is absent.
        let snapshotSettings = bridge.snapshot.settings!
        let router = HardwareKeyRouter(submit: { command, completion in
            DispatchQueue.main.async { completion(bridge.submitHardwareKey(command)) }
        }, permissionProvider: { HardwareKeyPermissions(accessibility: true, inputMonitoring: true) })
        router.setSteps(brightness: Double(snapshotSettings.brightness_key_step ?? 5),
                        volume: Double(snapshotSettings.volume_key_step ?? 5))
        // Volume keys follow the default sound output, so the display fixture names the output it owns.
        router.setDefaultOutput(uid: "synthetic-output")
        bridge.hardwareKeyValidator = { router.isCommandCurrent($0) }
        var keyAccepted: Bool?
        var keyActionGeneration: UInt64?
        var keyCommand: HardwareKeyCommand?
        bridge.onHardwareKeyAccepted = { command, accepted, actionGeneration in
            keyCommand = command
            keyAccepted = accepted
            keyActionGeneration = actionGeneration
        }

        func screen() -> CoreMonitor? { bridge.snapshot.monitors?.first(where: { $0.id == "screen" }) }
        let monitor = screen()!
        expect(monitor.brightness?.value != nil && monitor.volume?.value != nil,
               "synthetic screen reports brightness and volume values")
        let observedBrightness = monitor.brightness!.value!
        let observedVolume = monitor.volume!.value!
        let displays = [HardwareKeyDisplay(displayID: 7, bounds: CGRect(x: 0, y: 0, width: 1000, height: 800),
                                           isBuiltin: false, monitorID: "screen", generation: monitor.generation!,
                                           audioDeviceUID: "synthetic-output",
                                           brightness: HardwareKeyFeatureState(value: observedBrightness, supported: true, awake: true),
                                           volume: HardwareKeyFeatureState(value: observedVolume,
                                                                           supported: monitor.volume?.supported ?? false,
                                                                           awake: true))]
        let pointer = CGPoint(x: 500, y: 400)

        // One brightness press moves the target by the configured brightness step (3), not the default.
        let brightnessUp = Int(NX_KEYTYPE_BRIGHTNESS_UP)
        let brightnessDown = HardwareKeyEvent(keyIdentity: brightnessUp, feature: .brightness, direction: .increase,
                                              phase: .down, isRepeat: false, isFineStep: false, modifiersEligible: true)
        let brightnessRelease = HardwareKeyEvent(keyIdentity: brightnessUp, feature: .brightness, direction: .increase,
                                                 phase: .up, isRepeat: false, isFineStep: false, modifiersEligible: true)
        expect(router.processForTesting(brightnessDown, pointer: pointer, displays: displays),
               "brightness key down is handled by the router with the configured step")
        expect(router.processForTesting(brightnessRelease, pointer: pointer, displays: displays),
               "brightness key up is consumed with its down")
        expect(waitUntil { keyAccepted == true && keyActionGeneration != nil },
               "core accepts the brightness key command")
        expect(keyCommand.map { $0.feature == .brightness && abs($0.target - (observedBrightness + 3)) < 0.001 } == true,
               "brightness key target is the observed value plus the brightness step of 3")
        expect(waitUntil {
            bridge.snapshot.pending_actions?.contains(where: { $0.monitor_id == "screen" && $0.kind == "set_brightness" }) == true
        }, "accepted brightness key is queued as a set_brightness action")
        expect(waitUntil { abs((screen()?.brightness?.desired ?? -1) - (observedBrightness + 3)) < 0.001 },
               "brightness target is recorded as the desired brightness")

        // Clear the callback state so only the volume press's own acceptance can satisfy the checks below.
        keyAccepted = nil
        keyActionGeneration = nil
        keyCommand = nil
        // One volume press moves the target by the configured volume step (12) on the default output display.
        let soundUp = Int(NX_KEYTYPE_SOUND_UP)
        let volumeDown = HardwareKeyEvent(keyIdentity: soundUp, feature: .volume, direction: .increase,
                                          phase: .down, isRepeat: false, isFineStep: false, modifiersEligible: true)
        let volumeRelease = HardwareKeyEvent(keyIdentity: soundUp, feature: .volume, direction: .increase,
                                             phase: .up, isRepeat: false, isFineStep: false, modifiersEligible: true)
        expect(router.processForTesting(volumeDown, pointer: pointer, displays: displays),
               "volume key down is handled by the router with the configured step")
        expect(router.processForTesting(volumeRelease, pointer: pointer, displays: displays),
               "volume key up is consumed with its down")
        expect(waitUntil { keyAccepted == true && keyActionGeneration != nil },
               "core accepts the volume key command")
        expect(keyCommand.map { $0.feature == .volume && abs($0.target - (observedVolume + 12)) < 0.001 } == true,
               "volume key target is the observed value plus the volume step of 12")
        // The snapshot lists one scheduled action per monitor, so the queued set_volume is verified through its desired value.
        expect(waitUntil { abs((screen()?.volume?.desired ?? -1) - (observedVolume + 12)) < 0.001 },
               "volume target is recorded as the desired volume")

        router.shutdown()
        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "shutdown completes before the temporary directory is removed")
    }

    private static func cameraHistoryDecodesAndStaysOutOfSaves() {
        // Snapshots from before camera_history existed must still decode, with the history absent.
        let withoutHistory = decodeSettings([:])
        expect(withoutHistory.camera_history == nil && withoutHistory.selected_camera_id == nil,
               "snapshots without camera_history still decode")
        let withHistory = decodeSettings(["selected_camera_id": "camera-b", "camera_history": ["camera-b", "camera-a"]])
        expect(withHistory.camera_history == ["camera-b", "camera-a"], "camera_history decodes in most-recent-first order")
        // The camera is sent only by selectCamera(_:), so a settings payload never carries it or the read-only history.
        expect(withHistory.json["selected_camera_id"] == nil && withHistory.json["camera_history"] == nil,
               "settings payloads never carry the camera selection or the read-only history")
    }

    private static func inferenceDeviceRoundTripsThroughCore() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-inference-device-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.settings?.inference_device == "auto" },
               "core snapshots publish the default inference device")
        var settings = bridge.snapshot.settings!
        settings.inference_device = "ane"
        bridge.updateSettings(settings)
        expect(waitUntil { bridge.snapshot.settings?.inference_device == "ane" },
               "a settings save publishes the selected inference device")

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "core shuts down before the temporary directory is removed")
    }

    private static func cameraSelectionRoundTripsThroughCore() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the camera test")

        expect(bridge.selectCamera("camera-a"), "camera A selection is accepted by the local core")
        expect(waitUntil {
            bridge.snapshot.settings?.selected_camera_id == "camera-a" &&
                bridge.snapshot.settings?.camera_history == ["camera-a"]
        }, "selecting camera A records it as the only history entry")

        expect(bridge.selectCamera("camera-b"), "camera B selection is accepted by the local core")
        expect(waitUntil {
            bridge.snapshot.settings?.selected_camera_id == "camera-b" &&
                bridge.snapshot.settings?.camera_history == ["camera-b", "camera-a"]
        }, "selecting camera B moves it to the front of the history")

        // A fresh CoreSettings carries no camera (selected_camera_id nil). Before the change, this full save sent
        // that null and cleared the selection; an unrelated save must leave the camera alone.
        var unrelated = CoreSettings()
        unrelated.presence_protection_enabled = true
        bridge.updateSettings(unrelated)
        expect(waitUntil { bridge.snapshot.settings?.presence_protection_enabled == true },
               "unrelated settings save is published")
        expect(bridge.snapshot.settings?.selected_camera_id == "camera-b" &&
               bridge.snapshot.settings?.camera_history == ["camera-b", "camera-a"],
               "unrelated settings save leaves the selection and history unchanged")

        expect(bridge.selectCamera(nil), "default camera selection is accepted by the local core")
        expect(waitUntil {
            bridge.snapshot.settings?.selected_camera_id == nil && bridge.snapshot.settings?.camera_history == []
        }, "selecting the default camera clears the selection and the history")

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "shutdown completes before the temporary directory is removed")
    }

    /// Decodes a full snapshot object, the shape `tv_snapshot` returns, so any optional section can be left out.
    private static func decodeSnapshot(_ object: [String: Any]) -> CoreSnapshot {
        let data = try! JSONSerialization.data(withJSONObject: object)
        return try! JSONDecoder().decode(CoreSnapshot.self, from: data)
    }

    /// Builds a curve from (monitor percent, MacBook percent) pairs, in the order given.
    private static func makeCurve(_ pairs: [(Int, Double)]) -> AmbientCurve {
        AmbientCurve(points: pairs.map { AmbientCurvePoint(monitor: Double($0.0), builtin: $0.1) })
    }

    /// Reads the (monitor, builtin) pairs of an emitted curve object, in emitted order.
    private static func emittedPointPairs(_ curve: [String: Any]?) -> [[Double]]? {
        (curve?["points"] as? [[String: Any]])?.map { [($0["monitor"] as? Double) ?? -1, ($0["builtin"] as? Double) ?? -1] }
    }

    private static func coreSettingsAmbientFieldsJSON() {
        // Snapshots from before brightness follow existed must still decode, with the new settings absent.
        let withoutAmbient = decodeSettings([:])
        expect(withoutAmbient.ambientFollowEnabled == nil && withoutAmbient.ambientCurves == nil &&
               withoutAmbient.protectionDisabled == nil, "absent ambient settings decode as nil")

        // A curve with fewer than two points is incomplete, but it must still decode so the settings can be saved back.
        let complete = makeCurve([(25, 18), (50, 40), (75, 66), (100, 100)])
        let partial = makeCurve([(25, 18)])
        let withAmbient = decodeSettings([
            "ambient_follow_enabled": true,
            "ambient_curves": ["monitor-a": ["points": [["monitor": 25, "builtin": 18], ["monitor": 50, "builtin": 40],
                                                        ["monitor": 75, "builtin": 66], ["monitor": 100, "builtin": 100]]],
                               "monitor-c": ["points": [["monitor": 25, "builtin": 18]]]],
            "protection_disabled": ["monitor-b"]
        ])
        expect(withAmbient.ambientFollowEnabled == true, "ambient_follow_enabled decodes from a snapshot that carries it")
        expect(withAmbient.ambientCurves == ["monitor-a": complete, "monitor-c": partial],
               "ambient_curves decode per monitor from the points list")
        expect(withAmbient.protectionDisabled == ["monitor-b"], "protection_disabled decodes as a list of monitor IDs")

        var settings = CoreSettings()
        settings.ambientFollowEnabled = true
        settings.ambientCurves = ["monitor-a": complete, "monitor-c": partial]
        settings.protectionDisabled = ["monitor-b"]
        let emitted = settings.json
        expect(JSONSerialization.isValidJSONObject(emitted), "settings payload with ambient fields is valid JSON")
        expect((emitted["ambient_follow_enabled"] as? Bool) == true, "ambient_follow_enabled is emitted as a bool when set")
        expect((emitted["protection_disabled"] as? [String]) == ["monitor-b"],
               "protection_disabled is emitted as an array of monitor IDs when set")
        let pointsOnly: Set<String> = ["points"]
        let pointKeys: Set<String> = ["monitor", "builtin"]
        let curves = emitted["ambient_curves"] as? [String: Any]
        let emittedComplete = curves?["monitor-a"] as? [String: Any]
        expect(emittedPointPairs(emittedComplete) == [[25, 18], [50, 40], [75, 66], [100, 100]],
               "a complete curve is emitted as its points, with monitor and builtin as numbers")
        expect(emittedComplete.map { Set($0.keys) } == pointsOnly, "a curve object carries only the points list")
        let completePointObjects = emittedComplete?["points"] as? [[String: Any]]
        expect(completePointObjects?.allSatisfy { Set($0.keys) == pointKeys } == true,
               "each emitted point carries exactly monitor and builtin")
        let emittedPartial = curves?["monitor-c"] as? [String: Any]
        expect(emittedPointPairs(emittedPartial) == [[25, 18]],
               "an incomplete curve keeps its single point as numbers")
        expect(emittedPartial.map { Set($0.keys) } == pointsOnly, "an incomplete curve carries only the points list")

        // Omitted keys leave the core's stored values untouched, so an unrelated save must not send these.
        let unset = CoreSettings()
        expect(unset.json["ambient_follow_enabled"] == nil && unset.json["ambient_curves"] == nil &&
               unset.json["protection_disabled"] == nil,
               "unset ambient settings are omitted from the payload so an unrelated save leaves them untouched")
    }

    /// Curves are emitted as a `points` list in stored (ascending) order, and an empty curve emits an empty list.
    private static func ambientCurvePointsJSON() {
        // Points are inserted out of order, so the emitted order depends on the sorted storage in `set`.
        var built = AmbientCurve()
        built.set(monitor: 75, builtin: 66)
        built.set(monitor: 25, builtin: 18)
        built.set(monitor: 50, builtin: 40)
        var settings = CoreSettings()
        settings.ambientCurves = ["monitor-a": built, "monitor-b": AmbientCurve()]
        let emitted = settings.json
        let curves = emitted["ambient_curves"] as? [String: Any]
        let emittedBuilt = curves?["monitor-a"] as? [String: Any]
        expect(emittedPointPairs(emittedBuilt) == [[25, 18], [50, 40], [75, 66]],
               "points are emitted as monitor and builtin numbers in ascending monitor order")
        let pointKeys: Set<String> = ["monitor", "builtin"]
        let pointsOnly: Set<String> = ["points"]
        let builtPointObjects = emittedBuilt?["points"] as? [[String: Any]]
        expect(builtPointObjects?.allSatisfy { Set($0.keys) == pointKeys } == true,
               "each emitted point carries exactly monitor and builtin")
        expect(emittedBuilt.map { Set($0.keys) } == pointsOnly, "a curve object carries only the points list")
        let emittedEmpty = curves?["monitor-b"] as? [String: Any]
        expect((emittedEmpty?["points"] as? [Any])?.isEmpty == true, "a curve with no points emits an empty list")
    }

    private static func monitorFollowAndProtectionDecode() {
        let snapshot = decodeSnapshot(["monitors": [
            ["id": "screen", "name": "Display", "follow": "eligible", "protected": true],
            ["id": "dock", "name": "Dock", "follow": "uncalibrated", "protected": false],
            ["id": "plain", "name": "Plain"]
        ]])
        func monitor(_ id: String) -> CoreMonitor? { snapshot.monitors?.first(where: { $0.id == id }) }
        expect(monitor("screen")?.follow == "eligible" && monitor("screen")?.protected == true,
               "follow and protected decode from a monitor that carries them")
        expect(monitor("dock")?.follow == "uncalibrated" && monitor("dock")?.protected == false,
               "protected false decodes as false, not nil")
        expect(monitor("plain") != nil && monitor("plain")?.follow == nil && monitor("plain")?.protected == nil,
               "absent follow and protected decode as nil")
    }

    private static func ambientSnapshotDecodes() {
        let withAmbient = decodeSnapshot(["ambient": [
            "enabled": true, "available": true, "active": true, "calibrating": false, "builtin_percent": 44
        ]])
        expect(withAmbient.ambient == CoreAmbient(enabled: true, available: true, active: true, calibrating: false,
                                                  builtinPercent: 44),
               "ambient decodes from a snapshot that carries it")

        // builtin_percent is null when the shell does not know the built-in level.
        let unknownLevel = decodeSnapshot(["ambient": [
            "enabled": true, "available": false, "active": false, "calibrating": true, "builtin_percent": NSNull()
        ]])
        expect(unknownLevel.ambient == CoreAmbient(enabled: true, available: false, active: false, calibrating: true,
                                                   builtinPercent: nil),
               "a null builtin_percent decodes as nil")

        expect(decodeSnapshot([:]).ambient == nil, "snapshots without ambient still decode with it absent")
    }

    private static func builtInDisplayReportReachesTheSnapshot() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the built-in display test")

        // The report has no completion, so the published snapshot is the only observable effect.
        bridge.submitBuiltInDisplay(available: true, brightness: 44, calibrating: false)
        expect(waitUntil {
            bridge.snapshot.ambient?.available == true && bridge.snapshot.ambient?.builtinPercent == 44 &&
                bridge.snapshot.ambient?.calibrating == false
        }, "built-in display level 44 is published in the ambient snapshot")

        // A report without a readable level clears the published percent.
        bridge.submitBuiltInDisplay(available: true, brightness: nil, calibrating: false)
        expect(waitUntil {
            bridge.snapshot.ambient?.available == true && bridge.snapshot.ambient?.builtinPercent == nil
        }, "a report without a level publishes a nil built-in percent")

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "shutdown completes before the temporary directory is removed")
    }

    private static func updateCheckerUsesRealCore() {
        #if TELAVIVA_CORE
        expect((CoreUpdate.version() ?? "").isEmpty == false, "the real core reports a non-empty version")
        expect(CoreUpdate.evaluate(installed: "0.1.0", status: 404, body: Data())?.status == "no_release",
               "a 404 evaluated by the real core is no_release")
        let latest = Data(#"{"tag_name":"v999.0.0"}"#.utf8)
        let available = CoreUpdate.evaluate(installed: "0.1.0", status: 200, body: latest)
        expect(available?.status == "update_available" && available?.remoteVersion == "999.0.0" &&
               available?.releaseURL == "https://github.com/ksfreitas/telaviva-oled/releases/tag/v999.0.0",
               "a newer tag evaluated by the real core is update_available with its release URL")
        expect(CoreUpdate.evaluate(installed: "not-a-version", status: 200, body: latest) == nil,
               "a malformed installed version is rejected by the real core")
        #endif
    }

    private static func waitUntil(timeout: TimeInterval = 5, _ predicate: () -> Bool) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while !predicate() && Date() < deadline {
            _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.01))
        }
        return predicate()
    }
}
