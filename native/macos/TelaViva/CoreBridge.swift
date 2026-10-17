import Foundation
import Combine

/// Owns the versioned Rust core when the static library is present. The shell remains
/// launchable without it during early development, but reports that state explicitly.
final class CoreBridge: ObservableObject {
    @Published private(set) var snapshot: CoreSnapshot = .unavailable
    @Published var hardwareKeyStatusKey = "hardwareKeys.status.disabled"
    @Published var hardwareKeyPermissions = HardwareKeyPermissions.current()
    var hardwareKeyValidator: ((HardwareKeyCommand) -> Bool)?
    var onHardwareActionCompleted: ((String, UInt64, UInt64, UInt64, HardwareKeyFeature, Bool, Double?) -> Void)?
    var onHardwareKeyAccepted: ((HardwareKeyCommand, Bool, UInt64?) -> Void)?
    /// Test seam: invoked on the worker queue at the start of every snapshot refresh run, before the core is read.
    var refreshRunObserver: (() -> Void)?
    private var core: OpaquePointer?
    private var shutdownStarted = false
    private let ffiLock = NSRecursiveLock()
    private let workerQueue = DispatchQueue(label: "com.telaviva.core-bridge", qos: .userInitiated)
    private let refreshLock = NSLock()
    private var refreshScheduled = false
    private var refreshAgain = false
    private let publishLock = NSLock()
    private var pendingPublication: CoreSnapshot?
    private var publicationScheduled = false
    private let stateLock = NSLock()

    private var isShuttingDown: Bool {
        stateLock.lock(); defer { stateLock.unlock() }
        return shutdownStarted
    }

    init(configPath: URL? = nil) {
        #if TELAVIVA_CORE
        startCore(configPath: configPath)
        #endif
    }

    func refresh() {
        #if TELAVIVA_CORE
        requestSnapshotRefresh()
        #endif
    }

    #if TELAVIVA_CORE
    private func requestSnapshotRefresh() {
        refreshLock.lock()
        if refreshScheduled {
            refreshAgain = true
            refreshLock.unlock()
            return
        }
        refreshScheduled = true
        refreshLock.unlock()
        workerQueue.async { [weak self] in self?.refreshOnce() }
    }

    private func refreshOnce() {
        refreshRunObserver?()
        if let core, let value = readSnapshot(core) { publish(value) }
        refreshLock.lock()
        if refreshAgain {
            refreshAgain = false
            refreshLock.unlock()
            workerQueue.async { [weak self] in self?.refreshOnce() }
        } else {
            refreshScheduled = false
            refreshLock.unlock()
        }
    }

    private func readSnapshot(_ instance: OpaquePointer) -> CoreSnapshot? {
        ffiLock.lock(); defer { ffiLock.unlock() }
        var buffer = TvBuffer(data: nil, len: 0)
        guard tv_snapshot(instance, &buffer) == 0, let data = buffer.data else { return nil }
        defer { tv_buffer_free(&buffer) }
        return try? JSONDecoder().decode(CoreSnapshot.self, from: Data(bytes: data, count: buffer.len))
    }

    private func publish(_ value: CoreSnapshot) {
        publishLock.lock()
        if let pendingPublication, let current = pendingPublication.revision, let next = value.revision, next < current {
            publishLock.unlock()
            return
        }
        pendingPublication = value
        let schedule = !publicationScheduled
        publicationScheduled = true
        publishLock.unlock()
        if schedule { DispatchQueue.main.async { [weak self] in self?.drainPublications() } }
    }

    private func drainPublications() {
        dispatchPrecondition(condition: .onQueue(.main))
        while true {
            publishLock.lock()
            let value = pendingPublication
            pendingPublication = nil
            if value == nil { publicationScheduled = false }
            publishLock.unlock()
            guard let value else { return }
            if let previous = snapshot.revision, let next = value.revision, next < previous { continue }
            snapshot = value
        }
    }
    #endif

    @discardableResult
    func submit(_ object: [String: Any]) -> Bool {
        #if TELAVIVA_CORE
        guard let core, let bytes = try? JSONSerialization.data(withJSONObject: object), bytes.count <= 65_536 else { return false }
        ffiLock.lock()
        let accepted = bytes.withUnsafeBytes { raw in tv_submit(core, raw.baseAddress!.assumingMemoryBound(to: UInt8.self), raw.count) == TV_OK }
        ffiLock.unlock()
        if accepted { refresh() }
        return accepted
        #else
        return false
        #endif
    }

    @discardableResult
    func submitHardwareKey(_ command: HardwareKeyCommand) -> Bool {
        guard Thread.isMainThread, !isShuttingDown, hardwareKeyValidator?(command) == true else { return false }
        let type = command.feature == .brightness ? "set_brightness" : "set_volume"
        #if TELAVIVA_CORE
        guard let core else { return false }
        ffiLock.lock()
        let actionGeneration = readSnapshot(core)?.revision.map { $0 &+ 1 }
        let accepted = submit(["type": type, "monitor_id": command.monitorID, "connection_generation": command.generation,
                               "value": command.target])
        ffiLock.unlock()
        onHardwareKeyAccepted?(command, accepted, accepted ? actionGeneration : nil)
        return accepted
        #else
        return false
        #endif
    }

    /// Serializes a manual slider command and its snapshot work off the main thread.
    /// The action generation is captured from the core revision immediately before submit.
    func submitManualControl(_ command: ManualControlCommand, completion: @escaping (ManualSubmissionResult) -> Void) {
        #if TELAVIVA_CORE
        guard !isShuttingDown else { completion(ManualSubmissionResult(accepted: false, actionGeneration: nil)); return }
        workerQueue.async { [weak self] in
            guard let self, let core = self.core, !self.isShuttingDown else {
                DispatchQueue.main.async { completion(ManualSubmissionResult(accepted: false, actionGeneration: nil)) }
                return
            }
            let type = command.feature == .brightness ? "set_brightness" : "set_volume"
            let object: [String: Any] = ["type": type, "monitor_id": command.monitorID,
                                         "connection_generation": command.generation, "value": command.value]
            guard let bytes = try? JSONSerialization.data(withJSONObject: object), bytes.count <= 65_536 else {
                DispatchQueue.main.async { completion(ManualSubmissionResult(accepted: false, actionGeneration: nil)) }
                return
            }
            let submission: ManualSubmissionResult
            self.ffiLock.lock()
            if command.authorization.claim() {
                let before = self.readSnapshot(core)
                let actionGeneration = before?.revision.map { $0 &+ 1 }
                let accepted = bytes.withUnsafeBytes { raw in
                    tv_submit(core, raw.baseAddress!.assumingMemoryBound(to: UInt8.self), raw.count) == TV_OK
                }
                submission = ManualSubmissionResult(accepted: accepted, actionGeneration: accepted ? actionGeneration : nil)
            } else {
                submission = ManualSubmissionResult(accepted: false, actionGeneration: nil)
            }
            self.ffiLock.unlock()
            let after = self.readSnapshot(core)
            if let after { self.publish(after) }
            DispatchQueue.main.async {
                completion(submission)
            }
        }
        #else
        completion(ManualSubmissionResult(accepted: false, actionGeneration: nil))
        #endif
    }

    /// Reports a DDC completion with the originating action identity after Rust accepts it.
    func submitHardwareResult(_ action: PendingAction, connectionGeneration: UInt64, success: Bool,
                              value: Double? = nil, error: String? = nil) -> Bool {
        var result: [String: Any] = ["type": "hardware_result", "action_id": action.id,
                                     "generation": action.generation, "ok": success]
        if let value { result["value"] = value }
        if let error { result["error"] = error }
        let accepted = submit(result)
        guard accepted else { return false }
        let feature: HardwareKeyFeature? = action.kind == "set_brightness" ? .brightness :
            (action.kind == "set_volume" ? .volume : nil)
        if let feature {
            DispatchQueue.main.async { [weak self] in
                self?.onHardwareActionCompleted?(action.monitor_id, action.id, action.generation,
                                                 connectionGeneration, feature, success, value)
            }
        }
        return true
    }

    func updateSettings(_ settings: CoreSettings) {
        submit(["type": "update_settings", "settings": settings.json])
    }

    /// The only path that sends `selected_camera_id`. Call it when the user picks a device or "Use default camera".
    /// `nil` clears the core's history, so the app follows the system default from then on.
    @discardableResult
    func selectCamera(_ cameraID: String?) -> Bool {
        let value: Any = cameraID ?? NSNull()
        return submit(["type": "update_settings", "settings": ["selected_camera_id": value]])
    }

    func beginShutdown(completion: @escaping () -> Void = {}) {
        #if TELAVIVA_CORE
        guard let core, !isShuttingDown else { DispatchQueue.main.async(execute: completion); return }
        stateLock.lock(); shutdownStarted = true; stateLock.unlock()
        ffiLock.lock(); _ = tv_shutdown(core); ffiLock.unlock()
        workerQueue.async { [weak self] in
            guard let self else { DispatchQueue.main.async(execute: completion); return }
            if let value = self.readSnapshot(core) { self.publish(value) }
            DispatchQueue.main.async(execute: completion)
        }
        #else
        DispatchQueue.main.async(execute: completion)
        #endif
    }

    private func startCore(configPath: URL?) {
        #if TELAVIVA_CORE
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("TelaVivaOLED", isDirectory: true)
        let settingsURL = configPath ?? appSupport.appendingPathComponent("settings.json")
        try? FileManager.default.createDirectory(at: settingsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        let options = try? JSONSerialization.data(withJSONObject: [
            "config_path": settingsURL.path,
            "os_locale": Locale.current.identifier
        ])
        var instance: OpaquePointer?
        let callback: TvSnapshotReady = { _, userdata in
            guard let userdata else { return }
            let owner = Unmanaged<CoreBridge>.fromOpaque(userdata).takeUnretainedValue()
            owner.requestSnapshotRefresh()
        }
        let created = options?.withUnsafeBytes { bytes in
            tv_create(TV_CORE_ABI_VERSION, bytes.baseAddress?.assumingMemoryBound(to: UInt8.self), bytes.count,
                     callback, Unmanaged.passUnretained(self).toOpaque(), &instance)
        } ?? tv_create(TV_CORE_ABI_VERSION, nil, 0, callback, Unmanaged.passUnretained(self).toOpaque(), &instance)
        guard tv_abi_version() == TV_CORE_ABI_VERSION, created == TV_OK, let instance else {
            snapshot = .unavailable
            return
        }
        core = instance
        refresh()
        #else
        snapshot = .unavailable
        #endif
    }

    deinit {
        #if TELAVIVA_CORE
        if let core { ffiLock.lock(); if !isShuttingDown { _ = tv_shutdown(core) }; tv_destroy(core); ffiLock.unlock() }
        #endif
    }
}

struct CoreSnapshot: Decodable {
    var revision: UInt64?
    var settings: CoreSettings?
    var messages: [String: String]?
    var monitors: [CoreMonitor]?
    var pending_actions: [PendingAction]?
    var camera: CameraSnapshot?
    static let unavailable = CoreSnapshot(revision: nil, settings: nil, messages: nil, monitors: [], pending_actions: [], camera: nil)
    private init(revision: UInt64?, settings: CoreSettings?, messages: [String: String]?, monitors: [CoreMonitor]?, pending_actions: [PendingAction]?, camera: CameraSnapshot?) {
        self.revision = revision; self.settings = settings; self.messages = messages; self.monitors = monitors
        self.pending_actions = pending_actions; self.camera = camera
    }
}

struct CoreMonitor: Decodable, Identifiable {
    var id: String
    var name: String
    var presence_dimmed: Bool?
    var identity_status: String?
    var connection_path: String?
    var generation: UInt64?
    var display_state: String?
    var brightness: VcpValue?
    var volume: VcpValue?
}
struct VcpValue: Decodable {
    var supported: Bool
    var value: Double?
    var raw: Double?
    var min: Double?
    var max: Double?
    var desired: Double?
    var status: String?
    enum CodingKeys: String, CodingKey { case supported, value = "normalized", raw, min, max, desired, status }
}
struct PendingAction: Decodable { var id: UInt64; var generation: UInt64; var monitor_id: String; var kind: String; var value: Double?; var target: Double? }
struct CameraSnapshot: Decodable { var mode: String?; var capture_mode: String?; var status: String?; var display_status: String?; var next_sample_in_ms: UInt64?; var generation: UInt64? }

struct CoreSettings: Decodable {
    var locale = "auto"
    var presence_sampling_interval_s = 10.0
    var return_sampling_interval_s = 1.0
    var suspend_presence_on_battery = true
    var presence_protection_enabled = false
    var always_on_camera: Bool?
    var screensaver_protection_enabled = false
    var launch_at_login = false
    var screensaver_targets: [String: Double] = [:]
    var selected_camera_id: String?
    /// Read-only, most recent first. Optional so a snapshot without the key still decodes.
    var camera_history: [String]?
    var hardware_keys_enabled: Bool?
    var brightness_key_step: Int?
    var volume_key_step: Int?

    /// Omits `selected_camera_id` and `camera_history` on purpose: only `selectCamera(_:)` sends the camera, so an
    /// unrelated save never re-sends a fallback camera and promotes it to the front of the history.
    var json: [String: Any] { [
        "locale": locale, "presence_sampling_interval_s": presence_sampling_interval_s,
        "return_sampling_interval_s": return_sampling_interval_s,
        "suspend_presence_on_battery": suspend_presence_on_battery,
        "presence_protection_enabled": presence_protection_enabled,
        "always_on_camera": always_on_camera ?? false,
        "screensaver_protection_enabled": screensaver_protection_enabled,
        "launch_at_login": launch_at_login, "screensaver_targets": screensaver_targets,
        "hardware_keys_enabled": hardware_keys_enabled ?? false,
        "brightness_key_step": brightness_key_step ?? 5, "volume_key_step": volume_key_step ?? 5
    ] }
}

#if TELAVIVA_CORE
extension CoreBridge {
    static func smokeTest() -> Int32 {
        var instance: OpaquePointer?
        let smokeDirectory = FileManager.default.temporaryDirectory.appendingPathComponent("telaviva-smoke", isDirectory: true)
        try? FileManager.default.createDirectory(at: smokeDirectory, withIntermediateDirectories: true)
        let config = smokeDirectory.appendingPathComponent("settings.json").path
        let options = try? JSONSerialization.data(withJSONObject: ["config_path": config, "os_locale": Locale.current.identifier])
        let createStatus = options?.withUnsafeBytes { bytes in
            tv_create(TV_CORE_ABI_VERSION, bytes.baseAddress?.assumingMemoryBound(to: UInt8.self), bytes.count, nil, nil, &instance)
        } ?? tv_create(TV_CORE_ABI_VERSION, nil, 0, nil, nil, &instance)
        guard createStatus == TV_OK, let instance else { return 1 }
        defer { _ = tv_shutdown(instance); tv_destroy(instance) }
        var buffer = TvBuffer(data: nil, len: 0)
        guard tv_snapshot(instance, &buffer) == TV_OK, let data = buffer.data else { return 2 }
        let snapshotData = Data(bytes: data, count: buffer.len)
        tv_buffer_free(&buffer)
        guard let json = try? JSONSerialization.jsonObject(with: snapshotData) as? [String: Any],
              json["settings"] != nil, json["monitors"] != nil else { return 3 }
        let invalid: [UInt8] = [0x7b, 0x7d]
        guard tv_submit(instance, invalid, invalid.count) != TV_OK else { return 4 }
        let valid = Array(#"{"type":"power","on_battery":false}"#.utf8)
        guard tv_submit(instance, valid, valid.count) == TV_OK else { return 5 }
        return 0
    }
}
#endif
