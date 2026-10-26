import Foundation
import Combine

/// Owns the versioned Rust core when the static library is present. The shell remains
/// launchable without it during early development, but reports that state explicitly.
final class CoreBridge: ObservableObject {
    @Published private(set) var snapshot: CoreSnapshot = .unavailable
    @Published var hardwareKeyStatusKey = "hardwareKeys.status.disabled"
    @Published var keepAwakeStatusKey = "keepAwake.status.disabled"
    @Published var hardwareKeyPermissions = HardwareKeyPermissions.current()
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
        drainDiagnostics()
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
        // The revision is the core's change counter, so an equal revision carries the same state.
        if let pendingPublication, let current = pendingPublication.revision, let next = value.revision, next <= current {
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
            if let previous = snapshot.revision, let next = value.revision, next <= previous { continue }
            snapshot = value
        }
    }

    /// Logs the core's pending diagnostic lines to the `brightness` category. Runs on the worker queue only.
    /// Never throws: a failed drain logs nothing.
    private func drainDiagnostics() {
        guard let core, let data = takeDiagnostics(core) else { return }
        CoreBridge.forwardDiagnostics(data)
    }

    /// Returns a copy of the drained JSON array and frees the core's buffer, under the same lock as `readSnapshot`.
    private func takeDiagnostics(_ instance: OpaquePointer) -> Data? {
        ffiLock.lock(); defer { ffiLock.unlock() }
        var buffer = TvBuffer(data: nil, len: 0)
        guard tv_diagnostics_drain(instance, &buffer) == 0, let data = buffer.data else { return nil }
        defer { tv_buffer_free(&buffer) }
        return Data(bytes: data, count: buffer.len)
    }
    #endif

    /// Logs each line of a drained diagnostics array to the `brightness` category, in order.
    static func forwardDiagnostics(_ data: Data) {
        for line in decodeDiagnostics(data) { DiagnosticLog.core(line) }
    }

    /// Decodes the JSON array of strings that `tv_diagnostics_drain` returns. Data that is not such an array yields no lines.
    static func decodeDiagnostics(_ data: Data) -> [String] {
        (try? JSONDecoder().decode([String].self, from: data)) ?? []
    }

    @discardableResult
    func submit(_ object: [String: Any]) -> Bool {
        #if TELAVIVA_CORE
        guard let core, let bytes = try? JSONSerialization.data(withJSONObject: object), bytes.count <= 65_536 else { return false }
        ffiLock.lock()
        let accepted = bytes.withUnsafeBytes { raw in tv_submit(core, raw.baseAddress!.assumingMemoryBound(to: UInt8.self), raw.count) == TV_OK }
        ffiLock.unlock()
        // The snapshot-ready callback already schedules a refresh for every accepted command. The core skips the
        // callback once it is stopped, and shutdown restoration still needs this refresh.
        if accepted && isShuttingDown { refresh() }
        return accepted
        #else
        return false
        #endif
    }

    /// Passes a JSON text built by the Rust controls machine (a command_json or result_json) to tv_submit verbatim.
    /// Callable from any thread; the caller reports the outcome back to the machine.
    @discardableResult
    func submitJSON(_ text: String) -> Bool {
        #if TELAVIVA_CORE
        guard let core else { return false }
        let bytes = Array(text.utf8)
        guard bytes.count <= 65_536 else { return false }
        ffiLock.lock()
        let accepted = bytes.withUnsafeBytes { raw in
            tv_submit(core, raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count) == TV_OK
        }
        ffiLock.unlock()
        // Same rule as submit(_:): the callback refreshes, except after shutdown.
        if accepted && isShuttingDown { refresh() }
        return accepted
        #else
        return false
        #endif
    }

    func updateSettings(_ settings: CoreSettings) {
        submit(["type": "update_settings", "settings": settings.json])
    }

    /// Enqueues a merge of one protection target into the core's latest settings. `true` means accepted for enqueue;
    /// the read and partial submit stay serialized with core snapshots and other commands without blocking the caller.
    @discardableResult
    func updateProtectionBrightness(displayID: String, value: Double) -> Bool {
        guard !displayID.isEmpty, value.isFinite, (1...50).contains(value) else { return false }
        #if TELAVIVA_CORE
        guard core != nil, !isShuttingDown else { return false }
        workerQueue.async { [weak self] in
            guard let self, let core = self.core, !self.isShuttingDown else { return }
            self.ffiLock.lock()
            defer { self.ffiLock.unlock() }
            guard var targets = self.readSnapshot(core)?.settings?.screensaver_targets else { return }
            targets[displayID] = value
            let command: [String: Any] = ["type": "update_settings", "settings": ["screensaver_targets": targets]]
            guard let bytes = try? JSONSerialization.data(withJSONObject: command), bytes.count <= 65_536 else { return }
            _ = bytes.withUnsafeBytes { raw in
                tv_submit(core, raw.baseAddress!.assumingMemoryBound(to: UInt8.self), raw.count) == TV_OK
            }
        }
        return true
        #else
        return false
        #endif
    }

    /// The only path that sends `selected_camera_id`. Call it when the user picks a device or "Use default camera".
    /// `nil` clears the core's history, so the app follows the system default from then on.
    @discardableResult
    func selectCamera(_ cameraID: String?) -> Bool {
        let value: Any = cameraID ?? NSNull()
        return submit(["type": "update_settings", "settings": ["selected_camera_id": value]])
    }

    /// The only path that sends `monitored_display_id`. Call it when the user picks a display or Automatic.
    /// `nil` clears the core's history, so the core resolves the monitored display again from what is connected.
    @discardableResult
    func selectMonitoredDisplay(_ id: String?) -> Bool {
        submit(CoreBridge.monitoredDisplayCommand(id))
    }

    /// The `update_settings` body that selects the monitored display. Pure, so the payload can be checked without a running core.
    static func monitoredDisplayCommand(_ id: String?) -> [String: Any] {
        let value: Any = id ?? NSNull()
        return ["type": "update_settings", "settings": ["monitored_display_id": value]]
    }

    /// Reports the built-in display's level for brightness follow. Callable from any thread and fire-and-forget:
    /// a rejected report leaves the core's previous state in place. `protectable` and `holding` default to false,
    /// so a report without them is the legacy report.
    func submitBuiltInDisplay(available: Bool, brightness: Int?, calibrating: Bool, protectable: Bool = false,
                              holding: Bool = false) {
        submit(CoreBridge.builtInDisplayCommand(available: available, brightness: brightness, calibrating: calibrating,
                                                protectable: protectable, holding: holding))
    }

    /// The `builtin_display` command body. Pure, so the protection flags can be checked without a running core.
    static func builtInDisplayCommand(available: Bool, brightness: Int?, calibrating: Bool, protectable: Bool = false,
                                      holding: Bool = false) -> [String: Any] {
        let level: Any = brightness ?? NSNull()
        return ["type": "builtin_display", "available": available, "brightness": level, "calibrating": calibrating,
                "protectable": protectable, "holding": holding]
    }

    func beginShutdown(completion: @escaping () -> Void = {}) {
        #if TELAVIVA_CORE
        guard let core, !isShuttingDown else { DispatchQueue.main.async(execute: completion); return }
        stateLock.lock(); shutdownStarted = true; stateLock.unlock()
        ffiLock.lock(); _ = tv_shutdown(core); ffiLock.unlock()
        workerQueue.async { [weak self] in
            guard let self else { DispatchQueue.main.async(execute: completion); return }
            if let value = self.readSnapshot(core) { self.publish(value) }
            // The core is still alive here: deinit cannot run while this block holds self.
            self.drainDiagnostics()
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
    /// Monotonic receipt time used to account for time spent queued before the UI consumes a lease.
    private(set) var decodedAtUptime: TimeInterval = ProcessInfo.processInfo.systemUptime
    var revision: UInt64?
    var settings: CoreSettings?
    var monitors: [CoreMonitor]?
    var pending_actions: [PendingAction]?
    var camera: CameraSnapshot?
    var ambient: CoreAmbient?
    /// Built-in display protection. Snapshots from before it existed decode as `.none`.
    var builtinProtection: CoreBuiltInProtection = .none
    /// The core's resolved monitored display. Snapshots from before it existed decode as automatic.
    var monitoredDisplay: CoreMonitoredDisplay = .automatic
    /// Keep-awake request and finite presence lease. Missing on snapshots produced by older cores.
    var keepAwake: CoreKeepAwake = .disabled
    enum CodingKeys: String, CodingKey {
        case revision, settings, monitors, pending_actions, camera, ambient, builtinProtection = "builtin_protection",
             monitoredDisplay = "monitored_display", keepAwake = "keep_awake"
    }
    static let unavailable = CoreSnapshot(revision: nil, settings: nil, monitors: [], pending_actions: [], camera: nil)
    private init(revision: UInt64?, settings: CoreSettings?, monitors: [CoreMonitor]?, pending_actions: [PendingAction]?, camera: CameraSnapshot?) {
        decodedAtUptime = ProcessInfo.processInfo.systemUptime
        self.revision = revision; self.settings = settings; self.monitors = monitors
        self.pending_actions = pending_actions; self.camera = camera
    }

    /// Written by hand so `builtin_protection` and `monitored_display` can fall back; the other fields decode as the synthesized code would.
    init(from decoder: Decoder) throws {
        decodedAtUptime = ProcessInfo.processInfo.systemUptime
        let container = try decoder.container(keyedBy: CodingKeys.self)
        revision = try container.decodeIfPresent(UInt64.self, forKey: .revision)
        settings = try container.decodeIfPresent(CoreSettings.self, forKey: .settings)
        monitors = try container.decodeIfPresent([CoreMonitor].self, forKey: .monitors)
        pending_actions = try container.decodeIfPresent([PendingAction].self, forKey: .pending_actions)
        camera = try container.decodeIfPresent(CameraSnapshot.self, forKey: .camera)
        ambient = try container.decodeIfPresent(CoreAmbient.self, forKey: .ambient)
        builtinProtection = try container.decodeIfPresent(CoreBuiltInProtection.self, forKey: .builtinProtection) ?? .none
        monitoredDisplay = try container.decodeIfPresent(CoreMonitoredDisplay.self, forKey: .monitoredDisplay) ?? .automatic
        keepAwake = try container.decodeIfPresent(CoreKeepAwake.self, forKey: .keepAwake) ?? .disabled
    }

    func keepAwakeRemainingMilliseconds(at uptime: TimeInterval = ProcessInfo.processInfo.systemUptime) -> UInt64 {
        Self.remainingLeaseMilliseconds(keepAwake.validForMilliseconds, since: decodedAtUptime, at: uptime)
    }

    static func remainingLeaseMilliseconds(_ milliseconds: UInt64, since receivedAt: TimeInterval,
                                           at uptime: TimeInterval) -> UInt64 {
        guard milliseconds > 0 else { return 0 }
        let elapsed = max(0, uptime - receivedAt)
        let elapsedMilliseconds = UInt64(min(elapsed * 1_000, Double(UInt64.max)))
        return milliseconds > elapsedMilliseconds ? milliseconds - elapsedMilliseconds : 0
    }
}

struct CoreKeepAwake: Decodable, Equatable {
    var enabled: Bool
    var requested: Bool
    var observationID: UInt64
    var generation: UInt64
    var validForMilliseconds: UInt64
    var reason: String

    static let disabled = CoreKeepAwake(enabled: false, requested: false, observationID: 0,
                                        generation: 0, validForMilliseconds: 0, reason: "disabled")

    enum CodingKeys: String, CodingKey {
        case enabled, requested, observationID = "observation_id", generation
        case validForMilliseconds = "valid_for_ms", reason
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
    /// "eligible", "uncalibrated", "unsupported" or "unstable". Optional so older snapshots still decode.
    var follow: String?
    /// False when the monitor ID is in `protection_disabled`.
    var protected: Bool?
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
struct CameraSnapshot: Decodable { var mode: String?; var capture_mode: String?; var status: String?; var display_status: String?; var next_sample_in_ms: UInt64?; var retry_in_ms: UInt64?; var generation: UInt64?; var presence: String? }

/// One calibration point of a monitor's brightness-follow curve. `monitor` is the monitor brightness percent and
/// `builtin` is the MacBook level (percent) at which the monitor equals `monitor` percent.
struct AmbientCurvePoint: Codable, Equatable { var monitor: Double; var builtin: Double }

/// One monitor's brightness-follow calibration: a list of points kept in ascending order of monitor percent.
/// Each point's builtin is the MacBook level (percent) at which the monitor equals `monitor` percent. The curve is
/// complete with at least `minPoints` points and holds at most `maxPoints`.
struct AmbientCurve: Codable, Equatable {
    var points: [AmbientCurvePoint] = []
    static let minPoints = 2
    static let maxPoints = 32

    enum CodingKeys: String, CodingKey { case points }

    var isComplete: Bool { points.count >= Self.minPoints }

    func point(at monitor: Int) -> AmbientCurvePoint? {
        points.first { $0.monitor == Double(monitor) }
    }

    /// Replaces the point with that monitor percent, or inserts a new one at its sorted position.
    /// Inserting does nothing when the curve already has `maxPoints` points.
    mutating func set(monitor: Int, builtin: Double) {
        let value = Double(monitor)
        if let index = points.firstIndex(where: { $0.monitor == value }) {
            points[index].builtin = builtin
            return
        }
        guard points.count < Self.maxPoints else { return }
        let index = points.firstIndex { $0.monitor > value } ?? points.count
        points.insert(AmbientCurvePoint(monitor: value, builtin: builtin), at: index)
    }

    mutating func remove(monitor: Int) {
        points.removeAll { $0.monitor == Double(monitor) }
    }
}

struct CoreAmbient: Codable, Equatable {
    var enabled: Bool
    var available: Bool
    var active: Bool
    var calibrating: Bool
    var builtinPercent: Double?
    enum CodingKeys: String, CodingKey { case enabled, available, active, calibrating, builtinPercent = "builtin_percent" }
    init(enabled: Bool, available: Bool, active: Bool, calibrating: Bool, builtinPercent: Double?) {
        self.enabled = enabled; self.available = available; self.active = active
        self.calibrating = calibrating; self.builtinPercent = builtinPercent
    }
}

/// The core's built-in display protection state, read from the snapshot. `none` stands in for a snapshot without it.
struct CoreBuiltInProtection: Decodable, Equatable {
    var protectable: Bool
    var protected: Bool
    var available: Bool
    /// Subset of "presence" and "screensaver"; empty when nothing dims the built-in display.
    var reasons: [String]
    /// Nil when `reasons` is empty; otherwise the percent the built-in display is dimmed to.
    var targetPercent: Double?
    var holdAutoBrightness: Bool
    /// The shell's last reported `holding`, echoed back by the core.
    var holding: Bool
    static let none = CoreBuiltInProtection(protectable: false, protected: false, available: false, reasons: [],
                                            targetPercent: nil, holdAutoBrightness: false, holding: false)
    enum CodingKeys: String, CodingKey {
        case protectable, protected, available, reasons
        case targetPercent = "target_percent", holdAutoBrightness = "hold_auto_brightness", holding
    }
}

/// The monitored display the core resolved. `id` is a monitor id, "builtin", or nil when none is connected; `automatic`
/// is true while the user has made no selection.
struct CoreMonitoredDisplay: Decodable, Equatable {
    var id: String?
    var automatic: Bool
    static let automatic = CoreMonitoredDisplay(id: nil, automatic: true)
}

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
    /// Optional so snapshots written before device selection still decode; saves emit the core default when absent.
    var inference_device: String?
    /// Brightness above which presence checks may run, 0 to 90. Optional so older snapshots still decode.
    var presence_min_brightness_percent: Double?
    /// Brightness follow and protection opt-outs. Optional so older snapshots still decode; sent only when set.
    var ambientFollowEnabled: Bool?
    var ambientCurves: [String: AmbientCurve]?
    var protectionDisabled: [String]?
    /// Presence thresholds per display (monitor id or "builtin"), 0 to 90. Optional so older snapshots still decode;
    /// sent only when set, because `update_settings` replaces the whole map.
    var presenceMinBrightnessByDisplay: [String: Double]?
    /// Holds automatic brightness off while the built-in display is dimmed for protection. Not optional: the core
    /// always sends it, and saves always emit it.
    var builtinAutoBrightnessOffOnProtection = false
    /// Optional to decode snapshots written before the setting existed; saves always emit it.
    var keep_awake_with_presence: Bool?

    /// Keys are listed explicitly because the three ambient fields use snake_case raw names.
    enum CodingKeys: String, CodingKey {
        case locale, presence_sampling_interval_s, return_sampling_interval_s, suspend_presence_on_battery,
             presence_protection_enabled, always_on_camera, screensaver_protection_enabled, launch_at_login,
             screensaver_targets, selected_camera_id, camera_history, hardware_keys_enabled,
             brightness_key_step, volume_key_step, presence_min_brightness_percent, inference_device
        case ambientFollowEnabled = "ambient_follow_enabled", ambientCurves = "ambient_curves"
        case protectionDisabled = "protection_disabled"
        case presenceMinBrightnessByDisplay = "presence_min_brightness_by_display"
        case builtinAutoBrightnessOffOnProtection = "builtin_auto_brightness_off_on_protection"
        case keep_awake_with_presence
    }

    /// Omits `selected_camera_id` and `camera_history` on purpose: only `selectCamera(_:)` sends the camera, so an
    /// unrelated save never re-sends a fallback camera and promotes it to the front of the history.
    var json: [String: Any] {
        var object: [String: Any] = [
            "locale": locale, "presence_sampling_interval_s": presence_sampling_interval_s,
            "return_sampling_interval_s": return_sampling_interval_s,
            "suspend_presence_on_battery": suspend_presence_on_battery,
            "presence_protection_enabled": presence_protection_enabled,
            "always_on_camera": always_on_camera ?? true,
            "screensaver_protection_enabled": screensaver_protection_enabled,
            "launch_at_login": launch_at_login, "screensaver_targets": screensaver_targets,
            "hardware_keys_enabled": hardware_keys_enabled ?? false,
            "brightness_key_step": brightness_key_step ?? 5, "volume_key_step": volume_key_step ?? 5,
            "presence_min_brightness_percent": presence_min_brightness_percent ?? 20,
            "inference_device": inference_device ?? "auto",
            "builtin_auto_brightness_off_on_protection": builtinAutoBrightnessOffOnProtection,
            "keep_awake_with_presence": keep_awake_with_presence ?? false
        ]
        // Ambient keys are added only when set, so an unrelated save leaves the core's stored values untouched.
        if let ambientFollowEnabled { object["ambient_follow_enabled"] = ambientFollowEnabled }
        if let ambientCurves {
            object["ambient_curves"] = ambientCurves.mapValues { (curve: AmbientCurve) -> [String: Any] in
                // Points are emitted in the stored order, which the core requires to be ascending.
                ["points": curve.points.map { (point: AmbientCurvePoint) -> [String: Any] in
                    ["monitor": point.monitor, "builtin": point.builtin]
                }]
            }
        }
        if let protectionDisabled { object["protection_disabled"] = protectionDisabled.sorted() }
        if let presenceMinBrightnessByDisplay { object["presence_min_brightness_by_display"] = presenceMinBrightnessByDisplay }
        return object
    }
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
