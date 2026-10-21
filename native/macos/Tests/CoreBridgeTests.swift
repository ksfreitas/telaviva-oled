import Foundation
import Combine

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("CoreBridgeTests: \(message)") }
}

@main
private struct CoreBridgeTests {
    static func main() {
        refreshBurstsAreConflated()
        oneCommandPublishesOnce()
        identicalRefreshDoesNotPublish()
        stateChangeAfterSubmitIsVisibleWithoutExtraRefresh()
        coreSettingsKeyStepJSON()
        coreSettingsPresenceThresholdJSON()
        presenceThresholdByDisplayDecodes()
        presenceThresholdByDisplayJSON()
        coreSettingsInferenceDeviceJSON()
        cameraHistoryDecodesAndStaysOutOfSaves()
        inferenceDeviceRoundTripsThroughCore()
        cameraSelectionRoundTripsThroughCore()
        coreSettingsAmbientFieldsJSON()
        ambientCurvePointsJSON()
        monitorFollowAndProtectionDecode()
        ambientSnapshotDecodes()
        builtInProtectionSnapshotDecodes()
        builtInAutoBrightnessSettingDecodes()
        builtInAutoBrightnessSettingJSON()
        builtInDisplayReportReachesTheSnapshot()
        builtInDisplayCommandCarriesProtection()
        monitoredDisplaySnapshotDecodes()
        monitoredDisplayCommandCarriesSelection()
        updateCheckerUsesRealCore()
        print("Core bridge asynchronous tests passed")
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

    /// One accepted command publishes exactly one snapshot: the snapshot-ready callback drives the refresh, and
    /// the publish is not repeated by a second read.
    private static func oneCommandPublishesOnce() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the command")
        let baseline = bridge.snapshot.revision ?? 0

        // The first value a subscriber receives is the current snapshot, so it is dropped before counting.
        var emissions = 0
        let subscription = bridge.$snapshot.dropFirst().sink { _ in emissions += 1 }
        expect(bridge.submit(["type": "power", "on_battery": true]), "power command is accepted by the local core")
        expect(waitUntil { (bridge.snapshot.revision ?? 0) > baseline }, "the accepted command publishes a newer revision")
        // The predicate never holds, so this runs the main run loop for the full 0.3 s and a late duplicate would count.
        _ = waitUntil(timeout: 0.3) { false }
        expect(emissions == 1, "one accepted command publishes exactly one snapshot")
        subscription.cancel()

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "shutdown completes before the temporary directory is removed")
    }

    /// After a command has settled, refreshing with an unchanged revision publishes nothing.
    private static func identicalRefreshDoesNotPublish() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the identical refresh")
        let baseline = bridge.snapshot.revision ?? 0
        expect(bridge.submit(["type": "power", "on_battery": true]), "power command is accepted by the local core")
        expect(waitUntil { (bridge.snapshot.revision ?? 0) > baseline }, "the accepted command publishes a newer revision")
        _ = waitUntil(timeout: 0.3) { false }

        var emissions = 0
        let subscription = bridge.$snapshot.dropFirst().sink { _ in emissions += 1 }
        let settledRevision = bridge.snapshot.revision
        bridge.refresh()
        bridge.refresh()
        bridge.refresh()
        _ = waitUntil(timeout: 0.3) { false }
        expect(emissions == 0, "refreshes with an unchanged revision publish nothing")
        expect(bridge.snapshot.revision == settledRevision, "refreshes with an unchanged revision leave the revision unchanged")
        subscription.cancel()

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "shutdown completes before the temporary directory is removed")
    }

    /// A state change reaches the published snapshot through the core's callback alone, with no refresh() call here.
    private static func stateChangeAfterSubmitIsVisibleWithoutExtraRefresh() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-core-bridge-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.settings != nil }, "initial snapshot with settings is published before the change")
        var settings = bridge.snapshot.settings!
        settings.presence_min_brightness_percent = 35
        bridge.updateSettings(settings)
        expect(waitUntil { bridge.snapshot.settings?.presence_min_brightness_percent == 35 },
               "a settings save is visible in the snapshot without an extra refresh")

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
            "screensaver_targets": [String: Double](),
            "builtin_auto_brightness_off_on_protection": false
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

    /// The per-display thresholds decode from a snapshot that carries them, and read as nil when it lacks them.
    private static func presenceThresholdByDisplayDecodes() {
        let withMap = decodeSettings(["presence_min_brightness_by_display": ["monitor-a": 12, "builtin": 40]])
        expect(withMap.presenceMinBrightnessByDisplay == ["monitor-a": 12, "builtin": 40],
               "presence_min_brightness_by_display decodes from a snapshot that carries it")

        let withoutMap = decodeSettings([:])
        expect(withoutMap.presenceMinBrightnessByDisplay == nil,
               "a snapshot without presence_min_brightness_by_display decodes with it absent")
    }

    /// The map is emitted only when set, so an unrelated save leaves the core's stored thresholds untouched.
    private static func presenceThresholdByDisplayJSON() {
        let unset = CoreSettings()
        expect(unset.json["presence_min_brightness_by_display"] == nil,
               "an unset per-display threshold map is omitted from the payload")

        var settings = CoreSettings()
        settings.presenceMinBrightnessByDisplay = ["monitor-a": 12, "builtin": 40]
        expect(JSONSerialization.isValidJSONObject(settings.json), "settings payload with the per-display map is valid JSON")
        expect((settings.json["presence_min_brightness_by_display"] as? [String: Double]) == ["monitor-a": 12, "builtin": 40],
               "the per-display threshold map is emitted as its id-to-percent pairs when set")
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

    /// The built-in auto-brightness setting decodes from a settings payload with either value kept.
    private static func builtInAutoBrightnessSettingDecodes() {
        let enabled = decodeSettings(["builtin_auto_brightness_off_on_protection": true])
        expect(enabled.builtinAutoBrightnessOffOnProtection == true,
               "builtin_auto_brightness_off_on_protection decodes as true when the core stores it")
        let disabled = decodeSettings(["builtin_auto_brightness_off_on_protection": false])
        expect(disabled.builtinAutoBrightnessOffOnProtection == false,
               "builtin_auto_brightness_off_on_protection decodes as false when the core stores it")
    }

    /// The setting is always emitted, so an update carries the draft's value whether it is true or false.
    private static func builtInAutoBrightnessSettingJSON() {
        var settings = CoreSettings()
        settings.builtinAutoBrightnessOffOnProtection = true
        expect((settings.json["builtin_auto_brightness_off_on_protection"] as? Bool) == true,
               "builtin_auto_brightness_off_on_protection is emitted as true when set")
        settings.builtinAutoBrightnessOffOnProtection = false
        expect((settings.json["builtin_auto_brightness_off_on_protection"] as? Bool) == false,
               "builtin_auto_brightness_off_on_protection is emitted as false when cleared")
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

    /// The built-in protection section decodes from a snapshot that carries it, and reads as `none` when it is absent.
    private static func builtInProtectionSnapshotDecodes() {
        let withProtection = decodeSnapshot(["builtin_protection": [
            "protectable": true, "protected": true, "available": true, "reasons": ["presence"],
            "target_percent": 0.0, "hold_auto_brightness": true, "holding": false
        ]])
        expect(withProtection.builtinProtection == CoreBuiltInProtection(
                   protectable: true, protected: true, available: true, reasons: ["presence"], targetPercent: 0,
                   holdAutoBrightness: true, holding: false),
               "builtin_protection decodes from a snapshot that carries it")

        // Snapshots from before the built-in display protection existed must still decode, as not protectable.
        let withoutProtection = decodeSnapshot([:]).builtinProtection
        expect(withoutProtection == CoreBuiltInProtection.none,
               "a snapshot without builtin_protection decodes as the none value")
        expect(withoutProtection.protectable == false && withoutProtection.protected == false &&
               withoutProtection.reasons.isEmpty && withoutProtection.targetPercent == nil,
               "a snapshot without builtin_protection is not protectable, with no reasons or target")
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

    /// The built-in report carries the protection flags, and both are false when a shell does not send them.
    private static func builtInDisplayCommandCarriesProtection() {
        let reported = CoreBridge.builtInDisplayCommand(available: true, brightness: 44, calibrating: false,
                                                        protectable: true, holding: true)
        expect(JSONSerialization.isValidJSONObject(reported), "the built-in display command is valid JSON")
        expect((reported["type"] as? String) == "builtin_display", "the built-in display command keeps its type")
        expect((reported["protectable"] as? Bool) == true && (reported["holding"] as? Bool) == true,
               "a report with protectable and holding sends both as true")

        let legacy = CoreBridge.builtInDisplayCommand(available: true, brightness: 44, calibrating: false)
        expect((legacy["protectable"] as? Bool) == false && (legacy["holding"] as? Bool) == false,
               "a report without the protection arguments sends protectable and holding as false")
        expect((legacy["available"] as? Bool) == true && (legacy["calibrating"] as? Bool) == false,
               "the existing built-in report fields are unchanged")
    }

    /// The monitored display decodes from a snapshot that carries it, and reads as automatic when the snapshot lacks it.
    private static func monitoredDisplaySnapshotDecodes() {
        let selected = decodeSnapshot(["monitored_display": ["id": "builtin", "automatic": false]])
        expect(selected.monitoredDisplay == CoreMonitoredDisplay(id: "builtin", automatic: false),
               "monitored_display decodes the resolved id and the automatic flag from a snapshot that carries it")

        // A remembered choice with no connected display resolves to a null id, and it is not automatic.
        let unresolved = decodeSnapshot(["monitored_display": ["id": NSNull(), "automatic": false]])
        expect(unresolved.monitoredDisplay == CoreMonitoredDisplay(id: nil, automatic: false),
               "a null id decodes as nil and stays distinct from automatic")

        // Snapshots from before the monitored display existed must still decode, as automatic with no id.
        let withoutMonitored = decodeSnapshot([:]).monitoredDisplay
        expect(withoutMonitored == CoreMonitoredDisplay.automatic && withoutMonitored.id == nil,
               "a snapshot without monitored_display decodes as automatic with no id")
    }

    /// The selection command sends only `monitored_display_id`: the display's id as a string, or null for Automatic.
    private static func monitoredDisplayCommandCarriesSelection() {
        let selected = CoreBridge.monitoredDisplayCommand("monitor-a")
        expect(JSONSerialization.isValidJSONObject(selected), "the monitored display command is valid JSON")
        expect((selected["type"] as? String) == "update_settings", "the monitored display command is an update_settings command")
        let selectedSettings = selected["settings"] as? [String: Any]
        expect(selectedSettings?.count == 1 && (selectedSettings?["monitored_display_id"] as? String) == "monitor-a",
               "a selected display sends its id as monitored_display_id and nothing else")

        let automatic = CoreBridge.monitoredDisplayCommand(nil)
        expect(JSONSerialization.isValidJSONObject(automatic), "the Automatic command is valid JSON")
        let automaticSettings = automatic["settings"] as? [String: Any]
        expect(automaticSettings?.count == 1 && automaticSettings?["monitored_display_id"] is NSNull,
               "Automatic sends monitored_display_id as null and nothing else")
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
