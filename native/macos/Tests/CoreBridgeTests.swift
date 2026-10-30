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
        presenceThresholdRangeConstants()
        presenceThresholdByDisplayDecodes()
        presenceThresholdByDisplayJSON()
        coreSettingsInferenceDeviceJSON()
        keepAwakeSettingMigrationAndJSON()
        keepAwakeSnapshotDecodes()
        keepAwakeSessionReconcilesOffToOnThroughTheCore()
        cameraHistoryDecodesAndStaysOutOfSaves()
        inferenceDeviceRoundTripsThroughCore()
        protectionBrightnessSettingsUpdatesDoNotReplaceStaleTargets()
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
        cameraRetryDelaySnapshotDecodes()
        lockOnAbsenceSettingsRoundTrip()
        lockOnAbsenceSettingsDefaults()
        lockSnapshotDecodes()
        sessionReportFollowsEitherOption()
        lockResultCommandShape()
        lockResultIsAcceptedByCore()
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

    /// The presence threshold slider spans 1 to 90 percent, and the range constant covers exactly those bounds.
    private static func presenceThresholdRangeConstants() {
        expect(PresenceThreshold.minimumPercent == 1, "the presence threshold minimum is 1 percent")
        expect(PresenceThreshold.maximumPercent == 90, "the presence threshold maximum is 90 percent")
        expect(PresenceThreshold.range == (1.0...90.0),
               "the presence threshold range is 1 through 90 percent as a closed range")
    }

    private static func keepAwakeSettingMigrationAndJSON() {
        let migrated = decodeSettings([:])
        expect(migrated.keep_awake_with_presence == nil, "older settings decode the added Boolean as absent")
        expect(migrated.json["keep_awake_with_presence"] as? Bool == false,
               "saving an older settings snapshot emits the disabled default")
        var enabled = migrated
        enabled.keep_awake_with_presence = true
        expect(enabled.json["keep_awake_with_presence"] as? Bool == true,
               "full settings payload round-trips the enabled value")
        expect(enabled.json["suspend_presence_on_battery"] as? Bool == true,
               "keep-awake settings serialization retains the battery preference")
    }

    private static func keepAwakeSnapshotDecodes() {
        let data = try! JSONSerialization.data(withJSONObject: [
            "keep_awake": ["enabled": true, "requested": true, "observation_id": 42,
                           "generation": 7, "valid_for_ms": 25_000, "reason": "present", "future": "ignored"]
        ])
        let snapshot = try! JSONDecoder().decode(CoreSnapshot.self, from: data)
        expect(snapshot.keepAwake == CoreKeepAwake(enabled: true, requested: true, observationID: 42,
                                                   generation: 7, validForMilliseconds: 25_000, reason: "present"),
               "keep-awake lease decodes its token and remaining TTL")
        let older = try! JSONDecoder().decode(CoreSnapshot.self, from: Data("{}".utf8))
        expect(older.keepAwake == .disabled, "missing additive snapshot data defaults to disabled")
        expect(CoreSnapshot.remainingLeaseMilliseconds(25_000, since: 10, at: 30) == 5_000,
               "time queued before UI delivery is deducted from the first lease snapshot")
        expect(CoreSnapshot.remainingLeaseMilliseconds(25_000, since: 10, at: 35) == 0,
               "a lease queued past its deadline reaches the controller expired")
    }

    private static func keepAwakeSessionReconcilesOffToOnThroughTheCore() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-keep-awake-session-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "core starts before the keep-awake session transition")
        expect(bridge.submit(["type": "power", "on_battery": false]), "AC power state is accepted")
        expect(bridge.submit(["type": "builtin_display", "available": true, "brightness": 80.0,
                              "calibrating": false, "protectable": true]), "awake built-in display is accepted")
        expect(bridge.submit(["type": "keep_awake_session", "active": false]), "inactive session is accepted")
        expect(bridge.submit(["type": "update_settings", "settings": [
            "presence_protection_enabled": true, "keep_awake_with_presence": false
        ]]), "keep-awake off state is accepted")
        expect(waitUntil { bridge.snapshot.camera?.mode == "face" || bridge.snapshot.camera?.mode == "light_only" },
               "camera runs with presence protection while keep-awake is off; got \(String(describing: bridge.snapshot.camera?.mode))/\(String(describing: bridge.snapshot.camera?.display_status))")

        expect(bridge.submit(["type": "update_settings", "settings": ["keep_awake_with_presence": true]]),
               "keep-awake on transition is accepted")
        expect(waitUntil { bridge.snapshot.camera?.mode == "suspended"
            && bridge.snapshot.camera?.display_status == "suspended" },
               "stale inactive core session suspends the camera after keep-awake is enabled")
        guard let resumeValue = KeepAwakeSessionReconciliation.sessionValueToResubmit(
            keepAwakeEnabled: bridge.snapshot.settings?.keep_awake_with_presence ?? false,
            cameraMode: bridge.snapshot.camera?.mode,
            displayStatus: bridge.snapshot.camera?.display_status,
            localMonitoringAllowed: true
        ) else { fatalError("CoreBridgeTests: unlocked stale core gate should request reconciliation") }
        expect(bridge.submit(["type": "keep_awake_session", "active": resumeValue]),
               "unlocked local state is accepted as the reconciliation value")
        expect(waitUntil { bridge.snapshot.camera?.mode == "face" },
               "camera resumes after the current unlocked session is reconciled")

        expect(bridge.submit(["type": "keep_awake_session", "active": false]), "manual lock state is accepted")
        expect(waitUntil { bridge.snapshot.camera?.display_status == "suspended" },
               "manual lock suspends keep-awake camera monitoring")
        let lockedResume = KeepAwakeSessionReconciliation.sessionValueToResubmit(
            keepAwakeEnabled: bridge.snapshot.settings?.keep_awake_with_presence ?? false,
            cameraMode: bridge.snapshot.camera?.mode,
            displayStatus: bridge.snapshot.camera?.display_status,
            localMonitoringAllowed: false
        )
        expect(lockedResume == nil, "locked local state does not submit an active session")
        expect(bridge.snapshot.camera?.mode == "suspended", "camera remains suspended while locked")

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "core shutdown finishes before its temporary config is removed")
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

    private static func protectionBrightnessSettingsUpdatesDoNotReplaceStaleTargets() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-protection-target-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.settings != nil }, "initial settings publish before protection target updates")
        var initial = bridge.snapshot.settings!
        initial.screensaver_targets = ["monitor-c": 18]
        initial.presence_protection_enabled = false
        initial.keep_awake_with_presence = false
        bridge.updateSettings(initial)
        expect(waitUntil { bridge.snapshot.settings?.screensaver_targets["monitor-c"] == 18 },
               "initial protection target is stored")

        // This represents the UI's cached settings value while another preference update lands.
        let stale = bridge.snapshot.settings!
        var latest = stale
        latest.presence_protection_enabled = true
        latest.keep_awake_with_presence = true
        bridge.updateSettings(latest)
        expect(waitUntil {
            bridge.snapshot.settings?.presence_protection_enabled == true &&
                bridge.snapshot.settings?.keep_awake_with_presence == true
        }, "new protection preferences are stored before the slider commands")

        expect(!bridge.updateProtectionBrightness(displayID: "", value: 12),
               "empty display IDs are rejected")
        expect(!bridge.updateProtectionBrightness(displayID: "monitor-a", value: .nan),
               "non-finite protection targets are rejected")
        expect(!bridge.updateProtectionBrightness(displayID: "monitor-a", value: 51),
               "values outside the slider range are rejected")

        // Hold the bridge's serial worker to prove slider setters only enqueue work and return immediately.
        let workerEntered = DispatchSemaphore(value: 0)
        let releaseWorker = DispatchSemaphore(value: 0)
        let observerLock = NSLock()
        var shouldBlockWorker = true
        bridge.refreshRunObserver = {
            observerLock.lock()
            let block = shouldBlockWorker
            shouldBlockWorker = false
            observerLock.unlock()
            if block {
                workerEntered.signal()
                _ = releaseWorker.wait(timeout: .now() + 5)
            }
        }
        bridge.refresh()
        expect(workerEntered.wait(timeout: .now() + 5) == .success,
               "the test blocks the core worker before submitting slider changes")

        let callerReturned = DispatchSemaphore(value: 0)
        let callerFinished = DispatchSemaphore(value: 0)
        let resultLock = NSLock()
        var acceptedCalls = false
        DispatchQueue.global(qos: .userInitiated).async {
            let firstAccepted = bridge.updateProtectionBrightness(displayID: "monitor-a", value: 12)
            let secondAccepted = bridge.updateProtectionBrightness(displayID: "monitor-b", value: 34)
            resultLock.lock()
            acceptedCalls = firstAccepted && secondAccepted
            resultLock.unlock()
            callerReturned.signal()
            callerFinished.signal()
        }

        let returnedBeforeWorkerRelease = callerReturned.wait(timeout: .now() + 0.5) == .success
        releaseWorker.signal()
        expect(returnedBeforeWorkerRelease, "slider setters return while the core worker is occupied")
        expect(callerFinished.wait(timeout: .now() + 5) == .success,
               "both protection target updates are enqueued")
        resultLock.lock()
        let accepted = acceptedCalls
        resultLock.unlock()
        expect(accepted, "both display target updates are accepted for enqueue")

        expect(waitUntil {
            bridge.snapshot.settings?.screensaver_targets["monitor-a"] == 12 &&
                bridge.snapshot.settings?.screensaver_targets["monitor-b"] == 34 &&
                bridge.snapshot.settings?.screensaver_targets["monitor-c"] == 18 &&
                bridge.snapshot.settings?.presence_protection_enabled == true &&
                bridge.snapshot.settings?.keep_awake_with_presence == true
        }, "queued target updates merge latest entries and preserve unrelated settings")

        var shutdownFinished = false
        bridge.beginShutdown { shutdownFinished = true }
        expect(waitUntil { shutdownFinished }, "core shuts down before the temporary directory is removed")
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

    /// The camera retry delay decodes from a snapshot that carries it, and reads as nil when it is absent.
    private static func cameraRetryDelaySnapshotDecodes() {
        let withRetry = decodeSnapshot(["camera": ["mode": "suspended", "status": "busy", "retry_in_ms": 30000]])
        expect(withRetry.camera?.retry_in_ms == 30000, "camera.retry_in_ms decodes from a snapshot that carries it")

        // Snapshots from before the retry delay existed must still decode, with it absent.
        let withoutRetry = decodeSnapshot(["camera": ["mode": "face", "status": "ready"]])
        expect(withoutRetry.camera?.retry_in_ms == nil, "a snapshot without camera.retry_in_ms decodes with it absent")
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

    /// The lock settings decode from a snapshot that carries them and survive a decode of the emitted payload.
    private static func lockOnAbsenceSettingsRoundTrip() {
        let stored = decodeSettings(["lock_on_absence": true, "lock_delay_s": 12])
        expect(stored.lock_on_absence == true && stored.lock_delay_s == 12,
               "lock_on_absence and lock_delay_s decode from a snapshot that carries them")
        expect((stored.json["lock_on_absence"] as? Bool) == true && (stored.json["lock_delay_s"] as? Double) == 12,
               "lock_on_absence and lock_delay_s are emitted as stored")
        let emitted = try! JSONSerialization.data(withJSONObject: stored.json)
        let again = try! JSONDecoder().decode(CoreSettings.self, from: emitted)
        expect(again.lock_on_absence == true && again.lock_delay_s == 12,
               "the emitted lock settings decode back to the same values")
    }

    /// Settings from before the lock feature decode with both absent, and every save emits the defaults of false and 5 seconds.
    private static func lockOnAbsenceSettingsDefaults() {
        let absent = decodeSettings([:])
        expect(absent.lock_on_absence == nil && absent.lock_delay_s == nil,
               "a snapshot without the lock settings decodes with them absent")
        expect((absent.json["lock_on_absence"] as? Bool) == false && (absent.json["lock_delay_s"] as? Double) == 5,
               "absent lock settings are emitted as the defaults of false and 5 seconds")
    }

    /// The lock block decodes its fields when present, reads a null token as nil, and is nil when absent.
    private static func lockSnapshotDecodes() {
        let requested = decodeSnapshot(["lock": ["enabled": true, "stage": "requested", "requested": true, "token": 3]])
        expect(requested.lock == CoreLock(enabled: true, stage: "requested", requested: true, token: 3),
               "the lock block decodes its stage, request and token from a snapshot that carries it")
        let armed = decodeSnapshot(["lock": ["enabled": true, "stage": "armed", "requested": false, "token": NSNull()]])
        expect(armed.lock == CoreLock(enabled: true, stage: "armed", requested: false, token: nil),
               "a null lock token decodes as nil")
        expect(decodeSnapshot([:]).lock == nil, "a snapshot without the lock block decodes with it nil")
    }

    /// The session is reported whenever either option is on, and not when both are off.
    private static func sessionReportFollowsEitherOption() {
        expect(!CoreBridge.shouldReportSession(keepAwake: false, lockOnAbsence: false),
               "the session is not reported when both options are off")
        expect(CoreBridge.shouldReportSession(keepAwake: true, lockOnAbsence: false),
               "keep-awake alone reports the session")
        expect(CoreBridge.shouldReportSession(keepAwake: false, lockOnAbsence: true),
               "the lock option alone reports the session")
        expect(CoreBridge.shouldReportSession(keepAwake: true, lockOnAbsence: true),
               "both options report the session")
    }

    /// The lock result carries its type, token and ok flag, and an error key only when an error is set.
    private static func lockResultCommandShape() {
        let succeeded = CoreBridge.lockResultCommand(token: 3, ok: true, error: nil)
        expect(JSONSerialization.isValidJSONObject(succeeded), "the lock result command is valid JSON")
        expect((succeeded["type"] as? String) == "lock_result" && (succeeded["token"] as? UInt64) == 3 &&
               (succeeded["ok"] as? Bool) == true, "a successful lock result carries its type, token and ok flag")
        expect(succeeded["error"] == nil, "a successful lock result omits the error key")
        let failed = CoreBridge.lockResultCommand(token: 4, ok: false, error: "not_confirmed")
        expect((failed["ok"] as? Bool) == false && (failed["error"] as? String) == "not_confirmed",
               "a failed lock result carries its error code")
    }

    /// The core accepts a lock result whose token has no pending request, so the shell can send one at any time.
    private static func lockResultIsAcceptedByCore() {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("telaviva-lock-result-tests-\(UUID().uuidString)", isDirectory: true)
        try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let bridge = CoreBridge(configPath: directory.appendingPathComponent("settings.json"))
        expect(waitUntil { bridge.snapshot.revision != nil }, "initial snapshot is published before the lock result")
        let command = CoreBridge.lockResultCommand(token: 3, ok: false, error: "not_confirmed")
        let text = String(data: try! JSONSerialization.data(withJSONObject: command), encoding: .utf8)!
        expect(bridge.submitJSON(text), "a lock result is accepted by the core")

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
