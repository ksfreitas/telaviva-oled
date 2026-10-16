import Foundation
import Combine

/// Owns the versioned Rust core when the static library is present. The shell remains
/// launchable without it during early development, but reports that state explicitly.
final class CoreBridge: ObservableObject {
    @Published private(set) var snapshot: CoreSnapshot = .unavailable
    private var core: OpaquePointer?
    private var shutdownStarted = false

    init() {
        #if TELAVIVA_CORE
        startCore()
        #endif
    }

    func refresh() {
        #if TELAVIVA_CORE
        guard let core else { return }
        var buffer = TvBuffer(data: nil, len: 0)
        guard tv_snapshot(core, &buffer) == 0, let data = buffer.data else { return }
        defer { tv_buffer_free(&buffer) }
        let bytes = Data(bytes: data, count: buffer.len)
        if let value = try? JSONDecoder().decode(CoreSnapshot.self, from: bytes) {
            if Thread.isMainThread { snapshot = value }
            else { DispatchQueue.main.async { self.snapshot = value } }
        }
        #endif
    }

    func submit(_ object: [String: Any]) {
        #if TELAVIVA_CORE
        guard let core, let bytes = try? JSONSerialization.data(withJSONObject: object), bytes.count <= 65_536 else { return }
        _ = bytes.withUnsafeBytes { raw in tv_submit(core, raw.baseAddress!.assumingMemoryBound(to: UInt8.self), raw.count) }
        refresh()
        #endif
    }

    func updateSettings(_ settings: CoreSettings) {
        submit(["type": "update_settings", "settings": settings.json])
    }

    func beginShutdown() {
        #if TELAVIVA_CORE
        guard let core, !shutdownStarted else { return }
        shutdownStarted = true
        _ = tv_shutdown(core)
        refresh()
        #endif
    }

    private func startCore() {
        #if TELAVIVA_CORE
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("TelaVivaOLED", isDirectory: true)
        try? FileManager.default.createDirectory(at: appSupport, withIntermediateDirectories: true)
        let options = try? JSONSerialization.data(withJSONObject: [
            "config_path": appSupport.appendingPathComponent("settings.json").path,
            "os_locale": Locale.current.identifier
        ])
        var instance: OpaquePointer?
        let callback: TvSnapshotReady = { _, userdata in
            guard let userdata else { return }
            let owner = Unmanaged<CoreBridge>.fromOpaque(userdata).takeUnretainedValue()
            DispatchQueue.main.async { owner.refresh() }
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
        if let core { if !shutdownStarted { _ = tv_shutdown(core) }; tv_destroy(core) }
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
    var brightness: VcpValue?
    var volume: VcpValue?
}
struct VcpValue: Decodable {
    var supported: Bool
    var value: Double?
    var raw: Double?
    var min: Double?
    var max: Double?
    enum CodingKeys: String, CodingKey { case supported, value = "normalized", raw, min, max }
}
struct PendingAction: Decodable { var id: UInt64; var generation: UInt64; var monitor_id: String; var kind: String; var value: Double?; var target: Double? }
struct CameraSnapshot: Decodable { var mode: String?; var capture_mode: String?; var status: String?; var display_status: String?; var next_sample_in_ms: UInt64?; var generation: UInt64? }

struct CoreSettings: Decodable {
    var locale = "auto"
    var presence_sampling_interval_s = 10.0
    var return_sampling_interval_s = 1.0
    var suspend_presence_on_battery = true
    var presence_protection_enabled = false
    var screensaver_protection_enabled = false
    var launch_at_login = false
    var screensaver_targets: [String: Double] = [:]
    var selected_camera_id: String?

    var json: [String: Any] { [
        "locale": locale, "presence_sampling_interval_s": presence_sampling_interval_s,
        "return_sampling_interval_s": return_sampling_interval_s,
        "suspend_presence_on_battery": suspend_presence_on_battery,
        "presence_protection_enabled": presence_protection_enabled,
        "screensaver_protection_enabled": screensaver_protection_enabled,
        "launch_at_login": launch_at_login, "screensaver_targets": screensaver_targets,
        "selected_camera_id": selected_camera_id ?? NSNull()
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
