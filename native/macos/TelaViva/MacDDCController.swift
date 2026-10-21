import AppKit
import CoreGraphics
import Darwin
import Foundation
import IOKit
import IOKit.graphics
import os

private let wakeLog = Logger(subsystem: Bundle.main.bundleIdentifier ?? "com.telaviva.oled", category: "wake")

/// Apple Silicon DDC/CI transport based on the MIT-licensed m1ddc IORegistry
/// discovery flow. All IOAV access is serialized on `queue`, never on the UI thread.
final class MacDDCController {
    private weak var core: CoreBridge?
    /// The core's controls driver; receives monitor profiles, removals and display power.
    var controls: ControlsDriver?
    private let queue = DispatchQueue(label: "com.telaviva.ddc", qos: .utility)
    private var active: Set<String> = []
    private var displays: [String: DisplayTransport] = [:]
    private var observer: NSObjectProtocol?
    private var workspaceObservers: [NSObjectProtocol] = []
    private var displayStates: [String: String] = [:]
    private var pendingDisplayStates: [String: String] = [:]
    private var connectionGenerations: [String: UInt64] = [:]
    private var lastReportedDisplayStates: [String: ReportedDisplayState] = [:]
    private var incompleteBrightnessAtDiscovery = Set<String>()
    private var latestSnapshotRevision: UInt64?
    private var latestMonitors: [String: CoreMonitor] = [:]
    private var stopped = false
    private var probeReport: [String: Any] = [:]
    private var lastAVSearchStage = "not_attempted"
    // Monitors whose cached IOAV service failed an attempt; the next discovery pass replaces it.
    private var staleServiceKeys = Set<String>()
    // True while one rediscovery pass is queued by markServiceStale.
    private var rediscoveryScheduled = false
    // Consecutive failed DDC primitives per monitor, for the attempt_recovered line; reset on success and removal.
    private var attemptFailures: [String: Int] = [:]
    var onHardwareKeyDisplays: (([HardwareKeyDisplay]) -> Void)?

    private struct ReportedDisplayState: Equatable {
        let generation: UInt64
        let state: String
    }

    init(core: CoreBridge) { self.core = core }

    func start() {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
        DiagnosticLog.ddc("app_start version=\(version)")
        queue.async { [weak self] in self?.discoverDisplays() }
        observer = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            DiagnosticLog.ddc("screen_parameters_changed")
            self?.enumerate()
        }
        let workspaceCenter = NSWorkspace.shared.notificationCenter
        workspaceObservers.append(workspaceCenter.addObserver(
            forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: nil
        ) { [weak self] _ in
            DiagnosticLog.ddc("display_sleep")
            self?.controls?.displayPower(on: false)
            DiagnosticLog.ddc("display_power on=false")
            self?.setAllDisplaysAsleep()
        })
        workspaceObservers.append(workspaceCenter.addObserver(
            forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: nil
        ) { [weak self] _ in
            DiagnosticLog.ddc("display_wake")
            self?.controls?.displayPower(on: true)
            DiagnosticLog.ddc("display_power on=true")
            wakeLog.debug("screens_did_wake at_ms=\(Int64(Date().timeIntervalSince1970 * 1000), privacy: .public)")
            self?.refreshDisplayStates()
        })
    }

    /// Performs the same real DDC reads used by display discovery and returns
    /// their results without issuing any VCP writes.
    func probeDisplays(completion: @escaping ([[String: Any]], [String: Any]) -> Void) {
        queue.async { [weak self] in
            guard let self, !self.stopped else { completion([], [:]); return }
            self.discoverDisplays()
            let result = self.displays.values.map { transport -> [String: Any] in
                let brightness = self.readVCP(transport: transport, code: 0x10)
                let volume = self.readVCP(transport: transport, code: 0x62)
                return [
                    "id": "\(transport.vendor)-\(transport.product)-\(transport.serial)-\(transport.edidUUID ?? "unknown")-\(transport.uuid)-\(transport.location)",
                    "name": self.displayName(adapterPath: transport.location) ?? "External display",
                    "vendor": transport.vendor, "product": transport.product, "serial": transport.serial,
                    "identity_stable": transport.identityIsStable,
                    "brightness": self.readingJSON(brightness), "volume": self.readingJSON(volume)
                ]
            }.sorted { ($0["name"] as? String ?? "") < ($1["name"] as? String ?? "") }
            completion(result, self.probeReport)
        }
    }

    func shutdown() {
        if let observer { NotificationCenter.default.removeObserver(observer) }
        observer = nil
        for token in workspaceObservers { NSWorkspace.shared.notificationCenter.removeObserver(token) }
        workspaceObservers.removeAll()
        queue.async { [weak self] in
            guard let self else { return }
            self.stopped = true
            for transport in self.displays.values {
                if let service = transport.service { Unmanaged<AnyObject>.fromOpaque(service).release() }
            }
            self.displays.removeAll()
            self.active.removeAll()
        }
    }

    /// Applies a published snapshot's monitors and display-state reports. The core's controls driver
    /// receives the pending actions separately, so this never reads them.
    func process(_ snapshot: CoreSnapshot) {
        guard let revision = snapshot.revision else { return }
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            if let latest = self.latestSnapshotRevision, revision < latest { return }
            self.latestSnapshotRevision = revision
            self.latestMonitors = Dictionary(uniqueKeysWithValues: (snapshot.monitors ?? []).map { ($0.id, $0) })
            self.connectionGenerations = Dictionary(uniqueKeysWithValues: (snapshot.monitors ?? []).compactMap { monitor in
                monitor.generation.map { (monitor.id, $0) }
            })
            self.publishHardwareKeyDisplays()
            for (monitorID, state) in self.displayStates {
                if let generation = self.connectionGenerations[monitorID],
                   self.lastReportedDisplayStates[monitorID] != ReportedDisplayState(generation: generation, state: state) {
                    self.pendingDisplayStates[monitorID] = state
                }
            }
            self.flushPendingDisplayStates()
        }
    }

    private func publishHardwareKeyDisplays() {
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(0, nil, &count) == .success, count > 0 else {
            DispatchQueue.main.async { [weak self] in self?.onHardwareKeyDisplays?([]) }
            return
        }
        var displayIDs = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetOnlineDisplayList(count, &displayIDs, &count) == .success else { return }
        let descriptors: [HardwareKeyDisplay] = displayIDs.prefix(Int(count)).map { displayID in
            let transportEntry = displays.first { $0.value.displayID == displayID }
            let transport = transportEntry?.value
            let monitor = transportEntry.flatMap { latestMonitors[$0.key] }
            let isBuiltin = CGDisplayIsBuiltin(displayID) != 0
            let monitorID = transport?.identityIsStable == true ? transportEntry?.key : nil
            let generation = monitorID.flatMap { latestMonitors[$0]?.generation }
            let awake = !isBuiltin && transport != nil &&
                (transportEntry.flatMap { displayStates[$0.key] } != "asleep") && CGDisplayIsAsleep(displayID) == 0
            func feature(_ value: VcpValue?) -> HardwareKeyFeatureState {
                HardwareKeyFeatureState(value: value?.value, supported: value?.supported == true, awake: awake)
            }
            return HardwareKeyDisplay(displayID: displayID, bounds: CGDisplayBounds(displayID), isBuiltin: isBuiltin,
                                      monitorID: monitorID, generation: generation,
                                      audioDeviceUID: monitorID != nil && !isBuiltin ? transport?.edidUUID : nil,
                                      brightness: feature(monitor?.brightness), volume: feature(monitor?.volume))
        }
        DispatchQueue.main.async { [weak self] in self?.onHardwareKeyDisplays?(descriptors) }
    }

    private func enumerate() {
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            self.discoverDisplays(recoverInitialBrightness: true)
        }
    }

    private struct DisplayTransport {
        let displayID: CGDirectDisplayID
        let vendor: UInt32
        let product: UInt32
        let serial: UInt32
        let name: String
        let uuid: String
        let location: String
        let edidUUID: String?
        let service: UnsafeMutableRawPointer?
        let chipAddress: UInt32
        let identityIsStable: Bool
    }

    private func discoverDisplays(recoverInitialBrightness: Bool = false) {
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(0, nil, &count) == .success, count > 0 else {
            probeReport = ["online_display_count": count, "external_display_count": 0, "discovered_monitor_count": 0, "ioav_transport_count": 0, "displays": []]
            publishDisconnectedDisplays([])
            return
        }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetOnlineDisplayList(count, &ids, &count) == .success else { return }

        var next: [String: DisplayTransport] = [:]
        var collisions = Set<String>()
        var diagnosticRows: [[String: Any]] = []
        for displayID in ids.prefix(Int(count)) where CGDisplayIsBuiltin(displayID) == 0 {
            let info = displayInfo(displayID)
            guard let uuid = info?.uuid, !uuid.isEmpty else {
                diagnosticRows.append(["display_id": displayID, "stage": info == nil ? "core_display_info_unavailable" : "display_uuid_missing"])
                continue
            }
            let location = info?.location ?? "CoreGraphics display \(displayID)"
            let adapter = IORegistryEntryCopyFromPath(kIOMainPortDefault, location as CFString)
            let adapterFound = adapter != MACH_PORT_NULL
            let edidUUID = adapterFound ? stringProperty(adapter, key: "EDID UUID") : nil
            let connector = location.split(separator: "/").first(where: { $0.hasPrefix("dispext") })
                .map { String($0).split(separator: "@").first.map(String.init) ?? String($0) } ?? ""
            let endpoint = adapterFound ? externalAVService(adapter: adapter, connector: connector) : nil
            if adapterFound { IOObjectRelease(adapter) }

            let vendor = CGDisplayVendorNumber(displayID)
            let product = CGDisplayModelNumber(displayID)
            let serial = CGDisplaySerialNumber(displayID)
            let key = "\(vendor)-\(product)-\(serial)-\(edidUUID ?? "unknown")-\(uuid)-\(location)"
            let stable = info?.location != nil && !(info?.location?.isEmpty ?? true)
            diagnosticRows.append(["display_id": displayID, "connector": connector, "name": info?.name ?? "External display",
                                   "uuid": uuid, "location": location, "adapter_found": adapterFound,
                                   "ioav_transport_found": endpoint != nil,
                                   "stage": endpoint == nil ? (adapterFound ? lastAVSearchStage : "iodisplay_location_not_found") : "transport_ready"])
            if next[key] != nil {
                collisions.insert(key)
                if let endpoint { Unmanaged<AnyObject>.fromOpaque(endpoint.0).release() }
                continue
            }
            next[key] = DisplayTransport(
                displayID: displayID, vendor: vendor, product: product, serial: serial,
                name: info?.name ?? "External display", uuid: uuid, location: location, edidUUID: edidUUID,
                service: endpoint?.0, chipAddress: endpoint?.1 ?? 0, identityIsStable: stable
            )
        }
        for key in collisions {
            if let transport = next.removeValue(forKey: key), let service = transport.service { Unmanaged<AnyObject>.fromOpaque(service).release() }
        }

        probeReport = ["online_display_count": count,
                       "external_display_count": ids.prefix(Int(count)).filter { CGDisplayIsBuiltin($0) == 0 }.count,
                       "discovered_monitor_count": next.count,
                       "ioav_transport_count": next.values.filter { $0.service != nil }.count,
                       "displays": diagnosticRows]
        let previous = displays
        displays = next
        for (key, old) in previous where next[key] == nil {
            if let service = old.service { Unmanaged<AnyObject>.fromOpaque(service).release() }
        }
        for (key, transport) in next {
            let prior = previous[key]
            // Keep the existing IOAV object when this is the same monitor and
            // connection; routine display refreshes must not reset ownership.
            if let prior {
                // Routine refreshes keep the existing IOAV object unless an attempt marked it stale.
                let replaceStale = staleServiceKeys.contains(key) && transport.service != nil
                staleServiceKeys.remove(key)
                if replaceStale {
                    if let oldService = prior.service, oldService != transport.service {
                        Unmanaged<AnyObject>.fromOpaque(oldService).release()
                    }
                } else if prior.service != nil, let service = transport.service, service != prior.service {
                    Unmanaged<AnyObject>.fromOpaque(service).release()
                }
                let service = replaceStale ? transport.service : (prior.service ?? transport.service)
                let chipAddress = replaceStale || prior.service == nil ? transport.chipAddress : prior.chipAddress
                displays[key] = DisplayTransport(
                    displayID: transport.displayID, vendor: transport.vendor,
                    product: transport.product, serial: transport.serial, name: transport.name,
                    uuid: transport.uuid, location: transport.location,
                    edidUUID: transport.edidUUID, service: service,
                    chipAddress: chipAddress,
                    identityIsStable: transport.identityIsStable
                )
            }
        }

        for (key, transport) in displays {
            let previousTransport = previous[key]
            if previousTransport == nil {
                let brightness = readVCP(transport: transport, code: 0x10)
                let volume = readVCP(transport: transport, code: 0x62)
                if brightness == nil, transport.service != nil {
                    incompleteBrightnessAtDiscovery.insert(key)
                } else {
                    incompleteBrightnessAtDiscovery.remove(key)
                }
                controls?.monitorProfile(monitorID: key, manufacturer: EDIDManufacturer.code(vendor: transport.vendor))
                DiagnosticLog.ddc("discovery_added monitor=\(DiagnosticLog.shortId(key)) brightness=\(brightnessSummary(brightness))")
                core?.submit(["type": "monitor_discovered", "monitor": [
                    "id": key,
                    "name": transport.name,
                    "identity_status": transport.identityIsStable ? "stable" : "ambiguous",
                    "connection_path": transport.location,
                    "brightness": featureJSON(brightness),
                    "volume": featureJSON(volume),
                    "primary": CGDisplayIsMain(transport.displayID) != 0
                ]])
            }
        }
        refreshDisplayStatesOnQueue(recoverInitialBrightness: recoverInitialBrightness)
        publishDisconnectedDisplays(Set(displays.keys))
        publishHardwareKeyDisplays()
    }

    private func setAllDisplaysAsleep() {
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            // The workspace sleep notification is authoritative for this edge;
            // CGDisplayIsAsleep can lag behind it.
            for monitorID in self.displays.keys {
                self.reportDisplayState(monitorID, state: "asleep")
            }
        }
    }

    private func refreshDisplayStates() {
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            self.discoverDisplays(recoverInitialBrightness: true)
        }
    }

    private func refreshDisplayStatesOnQueue(recoverInitialBrightness: Bool = false) {
        for (monitorID, transport) in displays {
            let state = CGDisplayIsAsleep(transport.displayID) != 0 ? "asleep" : "awake"
            if recoverInitialBrightness, state == "awake", incompleteBrightnessAtDiscovery.contains(monitorID),
               let brightness = readVCP(transport: transport, code: 0x10) {
                let volume = readVCP(transport: transport, code: 0x62)
                incompleteBrightnessAtDiscovery.remove(monitorID)
                // Initial discovery happened while the display was asleep, so
                // there was no trustworthy baseline. Rebuild this one monitor
                // from a fresh physical read before reporting it awake.
                connectionGenerations.removeValue(forKey: monitorID)
                core?.submit(["type": "monitor_discovered", "monitor": [
                    "id": monitorID,
                    "name": transport.name,
                    "identity_status": transport.identityIsStable ? "stable" : "ambiguous",
                    "connection_path": transport.location,
                    "brightness": featureJSON(brightness),
                    "volume": featureJSON(volume),
                    "primary": CGDisplayIsMain(transport.displayID) != 0
                ]])
            }
            reportDisplayState(monitorID, state: state)
        }
    }

    private func reportDisplayState(_ monitorID: String, state: String) {
        displayStates[monitorID] = state
        pendingDisplayStates[monitorID] = state
        flushPendingDisplayStates()
        publishHardwareKeyDisplays()
    }

    private func flushPendingDisplayStates() {
        for (monitorID, state) in Array(pendingDisplayStates) {
            guard let generation = connectionGenerations[monitorID] else { continue }
            let reported = ReportedDisplayState(generation: generation, state: state)
            guard lastReportedDisplayStates[monitorID] != reported else {
                pendingDisplayStates.removeValue(forKey: monitorID)
                continue
            }
            core?.submit(["type": "monitor_display_state", "monitor_id": monitorID,
                          "generation": generation, "state": state])
            lastReportedDisplayStates[monitorID] = reported
            pendingDisplayStates.removeValue(forKey: monitorID)
        }
    }

    private func publishDisconnectedDisplays(_ present: Set<String>) {
        for removed in Set(displayStates.keys).subtracting(present) {
            displayStates.removeValue(forKey: removed)
            pendingDisplayStates.removeValue(forKey: removed)
            connectionGenerations.removeValue(forKey: removed)
            lastReportedDisplayStates.removeValue(forKey: removed)
            incompleteBrightnessAtDiscovery.remove(removed)
        }
        for removed in active.subtracting(present) {
            controls?.monitorRemoved(monitorID: removed)
            attemptFailures.removeValue(forKey: removed)
            DiagnosticLog.ddc("discovery_removed monitor=\(DiagnosticLog.shortId(removed))")
            core?.submit(["type": "monitor_disconnected", "monitor_id": removed])
        }
        active = present
        // Drop stale-service marks for displays that are gone so keys do not accumulate.
        staleServiceKeys.formIntersection(present)
    }

    private struct DisplayInfo { let uuid: String?; let location: String?; let name: String? }

    private func displayInfo(_ display: CGDirectDisplayID) -> DisplayInfo? {
        guard let address = coreDisplayInfoSymbol() else { return nil }
        typealias CreateInfo = @convention(c) (CGDirectDisplayID) -> Unmanaged<CFDictionary>?
        guard let unmanaged = unsafeBitCast(address, to: CreateInfo.self)(display) else { return nil }
        let dictionary = unmanaged.takeRetainedValue() as NSDictionary
        let product = dictionary["DisplayProductName"] as? NSDictionary
        return DisplayInfo(uuid: dictionary["kCGDisplayUUID"] as? String,
                           location: dictionary["IODisplayLocation"] as? String,
                           name: product?["en_US"] as? String ?? product?[Locale.current.identifier] as? String)
    }

    private func coreDisplayInfoSymbol() -> UnsafeMutableRawPointer? {
        struct Cache { static let symbol: UnsafeMutableRawPointer? = {
            guard let handle = dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_LAZY) else { return nil }
            return dlsym(handle, "CoreDisplay_DisplayCreateInfoDictionary")
        }() }
        return Cache.symbol
    }

    private func externalAVService(adapter: io_service_t, connector: String) -> (UnsafeMutableRawPointer, UInt32)? {
        guard !connector.isEmpty else { lastAVSearchStage = "display_connector_unavailable"; return nil }
        return externalProxyBelow(connector: connector)
    }

    private func externalProxyBelow(connector: String) -> (UnsafeMutableRawPointer, UInt32)? {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("DCPAVServiceProxy"), &iterator) == KERN_SUCCESS else {
            lastAVSearchStage = "proxy_match_failed"
            return nil
        }
        defer { IOObjectRelease(iterator) }
        var proxyCount = 0
        var connectorMatchCount = 0
        while true {
            let entry = IOIteratorNext(iterator)
            if entry == MACH_PORT_NULL { break }
            defer { IOObjectRelease(entry) }
            proxyCount += 1
            guard stringProperty(entry, key: "Location") == "External" else { continue }
            var provider: io_registry_entry_t = 0
            let interface = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &provider) == KERN_SUCCESS
                ? stringProperty(provider, key: "interface-name") : nil
            if provider != MACH_PORT_NULL { IOObjectRelease(provider) }
            lastAVSearchStage = "connector_\(connector)_proxy_interface_\(interface ?? "missing")"
            guard interface?.hasPrefix("\(connector):dcpav-service-epic:") == true else { continue }
            connectorMatchCount += 1
            guard let service = createAVService(entry: entry) else {
                lastAVSearchStage = "ioav_service_create_failed"
                continue
            }
            let chip: UInt32 = isMCDP29xxProxy(entry) ? 0xB7 : 0x37
            return (service, chip)
        }
        lastAVSearchStage = "proxy_count_\(proxyCount)_connector_match_\(connectorMatchCount)_\(lastAVSearchStage)"
        return nil
    }

    private func isMCDP29xxProxy(_ proxy: io_service_t) -> Bool {
        var parent: io_registry_entry_t = 0
        guard IORegistryEntryGetParentEntry(proxy, kIOServicePlane, &parent) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(parent) }
        return stringProperty(parent, key: "EPICProviderClass") == "AppleDCPMCDP29XX"
    }

    private func stringProperty(_ entry: io_registry_entry_t, key: String) -> String? {
        guard let value = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key as CFString,
                                                           kCFAllocatorDefault, IOOptionBits(kIORegistryIterateRecursively)) else { return nil }
        guard CFGetTypeID(value) == CFStringGetTypeID() else { return nil }
        return (value as! CFString) as String
    }

    private func displayName(adapterPath: String) -> String? {
        let adapter = IORegistryEntryCopyFromPath(kIOMainPortDefault, adapterPath as CFString)
        guard adapter != MACH_PORT_NULL else { return nil }
        defer { IOObjectRelease(adapter) }
        guard let value = IORegistryEntrySearchCFProperty(adapter, kIOServicePlane, "DisplayAttributes" as CFString,
                                                           kCFAllocatorDefault, IOOptionBits(kIORegistryIterateRecursively)) else { return nil }
        let root = value as? NSDictionary
        let attrs = root?["ProductAttributes"] as? NSDictionary
        return attrs?["ProductName"] as? String
    }

    /// A failed attempt may mean the cached IOAV service went stale after a display change. The next
    /// discovery pass replaces it; one pass is scheduled at a time, and the core's backoff paces the retries.
    private func markServiceStale(_ key: String) {
        staleServiceKeys.insert(key)
        DiagnosticLog.ddc("service_marked_stale monitor=\(DiagnosticLog.shortId(key))")
        guard !rediscoveryScheduled else { return }
        rediscoveryScheduled = true
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            self.rediscoveryScheduled = false
            self.discoverDisplays()
        }
    }

    /// Logs the outcome of one DDC primitive. A failure extends the monitor's streak and logs its op, VCP code and
    /// elapsed time; the first success after a streak logs how many attempts failed.
    private func logAttempt(monitorID: String, write: Bool, code: UInt8, succeeded: Bool, startedAt: UInt64) {
        let key = DiagnosticLog.shortId(monitorID)
        guard !succeeded else {
            if let failures = attemptFailures.removeValue(forKey: monitorID) {
                DiagnosticLog.ddc("attempt_recovered monitor=\(key) after_failures=\(failures)")
            }
            return
        }
        attemptFailures[monitorID, default: 0] += 1
        let elapsedMs = (DispatchTime.now().uptimeNanoseconds - startedAt) / 1_000_000
        let op = write ? "write" : "read"
        let vcp = String(format: "0x%02x", UInt32(code))
        DiagnosticLog.ddc("attempt_failed monitor=\(key) op=\(op) code=\(vcp) error=io elapsed_ms=\(elapsedMs)")
    }

    /// The brightness of a discovery line: "current/maximum" for a reading, otherwise "read_failed".
    private func brightnessSummary(_ reading: Reading?) -> String {
        guard let reading else { return "read_failed" }
        return "\(reading.current)/\(reading.maximum)"
    }

    /// Runs exactly one DDC primitive for the core's `run_attempt` effect on the DDC queue and reports the outcome.
    /// The core owns retries, settle and backoff; this never sleeps between attempts.
    func runAttempt(_ request: ControlsAttemptRequest, completion: @escaping (ControlsAttemptOutcome) -> Void) {
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            guard let transport = self.displays[request.monitorID] else {
                completion(.failure("disconnected"))
                return
            }
            guard transport.service != nil else { completion(.failure("unsupported")); return }
            guard CGDisplayIsAsleep(transport.displayID) == 0, self.displayStates[request.monitorID] != "asleep" else {
                // The core holds the action until the display reports awake.
                self.reportDisplayState(request.monitorID, state: "asleep")
                completion(.failure("io"))
                return
            }
            let startedAt = DispatchTime.now().uptimeNanoseconds
            if request.write {
                let written = self.writeVCPOnce(transport: transport, code: request.code, value: request.value)
                self.logAttempt(monitorID: request.monitorID, write: true, code: request.code, succeeded: written, startedAt: startedAt)
                if written {
                    completion(.success(value: nil))
                } else {
                    self.markServiceStale(request.monitorID)
                    completion(.failure("io"))
                }
            } else {
                let readback = self.readVCPOnce(transport: transport, code: request.code)
                self.logAttempt(monitorID: request.monitorID, write: false, code: request.code, succeeded: readback != nil, startedAt: startedAt)
                guard let reading = readback else {
                    self.markServiceStale(request.monitorID)
                    completion(.failure("io"))
                    return
                }
                completion(.success(value: Double(reading.current)))
            }
        }
    }

    private typealias Reading = DDCTransaction.Reading

    private func readVCP(transport: DisplayTransport, code: UInt8) -> Reading? {
        transaction(transport).readVCP(code: code)
    }

    private func writeVCP(transport: DisplayTransport, code: UInt8, value: UInt16) -> Bool {
        transaction(transport).writeVCP(code: code, value: value)
    }

    private func readVCPOnce(transport: DisplayTransport, code: UInt8) -> Reading? {
        transaction(transport).readVCPOnce(code: code)
    }

    private func writeVCPOnce(transport: DisplayTransport, code: UInt8, value: UInt16) -> Bool {
        transaction(transport).writeVCPOnce(code: code, value: value)
    }

    private func transaction(_ transport: DisplayTransport) -> DDCTransaction {
        DDCTransaction(write: { self.writeI2C(transport, bytes: $0) }, read: { count in
            var reply = [UInt8](repeating: 0, count: count)
            return self.readI2C(transport, bytes: &reply) ? reply : nil
        })
    }

    private func featureJSON(_ reading: Reading?) -> [String: Any] {
        guard let reading, reading.maximum > 0 else { return ["supported": false, "min": 0, "max": 100] }
        return ["supported": true, "value": Double(reading.current) / Double(reading.maximum) * 100,
                "raw": Double(reading.current), "min": 0, "max": Double(reading.maximum)]
    }

    private func readingJSON(_ reading: Reading?) -> [String: Any] {
        guard let reading, reading.maximum > 0 else { return ["supported": false] }
        return ["supported": true, "current": reading.current, "maximum": reading.maximum,
                "normalized": Double(reading.current) / Double(reading.maximum) * 100]
    }

    private func createAVService(entry: io_service_t) -> UnsafeMutableRawPointer? {
        guard let address = symbols.ioAVCreate else { return nil }
        typealias Create = @convention(c) (CFAllocator?, io_service_t) -> UnsafeMutableRawPointer?
        return unsafeBitCast(address, to: Create.self)(kCFAllocatorDefault, entry)
    }

    private func writeI2C(_ transport: DisplayTransport, bytes: [UInt8]) -> Bool {
        guard let address = symbols.ioAVWrite, let service = transport.service else { return false }
        typealias Write = @convention(c) (UnsafeMutableRawPointer, UInt32, UInt32, UnsafeRawPointer, UInt32) -> Int32
        return bytes.withUnsafeBufferPointer { buffer in
            unsafeBitCast(address, to: Write.self)(service, transport.chipAddress, 0x51,
                                                   buffer.baseAddress!, UInt32(buffer.count)) == 0
        }
    }

    private func readI2C(_ transport: DisplayTransport, bytes: inout [UInt8]) -> Bool {
        guard let address = symbols.ioAVRead, let service = transport.service else { return false }
        typealias Read = @convention(c) (UnsafeMutableRawPointer, UInt32, UInt32, UnsafeMutableRawPointer, UInt32) -> Int32
        return bytes.withUnsafeMutableBufferPointer { buffer in
            unsafeBitCast(address, to: Read.self)(service, transport.chipAddress, 0,
                                                   buffer.baseAddress!, UInt32(buffer.count)) == 0
        }
    }

    private var symbols: Symbols { Symbols.shared }
    private struct Symbols {
        let ioAVCreate: UnsafeMutableRawPointer?
        let ioAVWrite: UnsafeMutableRawPointer?
        let ioAVRead: UnsafeMutableRawPointer?
        static let shared: Symbols = {
            guard let handle = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY) else {
                return Symbols(ioAVCreate: nil, ioAVWrite: nil, ioAVRead: nil)
            }
            return Symbols(ioAVCreate: dlsym(handle, "IOAVServiceCreateWithService"),
                           ioAVWrite: dlsym(handle, "IOAVServiceWriteI2C"),
                           ioAVRead: dlsym(handle, "IOAVServiceReadI2C"))
        }()
    }

    func refresh() { enumerate() }
}
