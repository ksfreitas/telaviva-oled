import AppKit
import CoreGraphics
import Darwin
import Foundation
import IOKit
import IOKit.graphics
import os

private let wakeLog = Logger(subsystem: Bundle.main.bundleIdentifier ?? "br.com.codebit.telaviva", category: "wake")

/// Apple Silicon DDC/CI transport based on the MIT-licensed m1ddc IORegistry
/// discovery flow. All IOAV access is serialized on `queue`, never on the UI thread.
final class MacDDCController {
    private weak var core: CoreBridge?
    private let queue = DispatchQueue(label: "com.telaviva.ddc", qos: .utility)
    private var active: Set<String> = []
    private var handledActions: Set<UInt64> = []
    private var handledActionOrder: [UInt64] = []
    private var displays: [String: DisplayTransport] = [:]
    private var observer: NSObjectProtocol?
    private var workspaceObservers: [NSObjectProtocol] = []
    private var displayStates: [String: String] = [:]
    private var pendingDisplayStates: [String: String] = [:]
    private var connectionGenerations: [String: UInt64] = [:]
    private var lastReportedDisplayStates: [String: ReportedDisplayState] = [:]
    private var incompleteBrightnessAtDiscovery = Set<String>()
    private var latestPendingActionIDs = Set<UInt64>()
    private var keyboardPendingMonitors = Set<String>()
    private var latestSnapshotRevision: UInt64?
    private var latestMonitors: [String: CoreMonitor] = [:]
    private var asleepActionIDs = Set<UInt64>()
    private var stopped = false
    private var probeReport: [String: Any] = [:]
    private var lastAVSearchStage = "not_attempted"
    var onHardwareKeyDisplays: (([HardwareKeyDisplay]) -> Void)?

    private struct ReportedDisplayState: Equatable {
        let generation: UInt64
        let state: String
    }

    init(core: CoreBridge) { self.core = core }

    func start() {
        queue.async { [weak self] in self?.discoverDisplays() }
        observer = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in self?.enumerate() }
        let workspaceCenter = NSWorkspace.shared.notificationCenter
        workspaceObservers.append(workspaceCenter.addObserver(
            forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: nil
        ) { [weak self] _ in self?.setAllDisplaysAsleep() })
        workspaceObservers.append(workspaceCenter.addObserver(
            forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: nil
        ) { [weak self] _ in
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

    func process(_ snapshot: CoreSnapshot) {
        guard let revision = snapshot.revision else { return }
        let actions = snapshot.pending_actions ?? []
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            if let latest = self.latestSnapshotRevision, revision < latest { return }
            self.latestSnapshotRevision = revision
            self.latestMonitors = Dictionary(uniqueKeysWithValues: (snapshot.monitors ?? []).map { ($0.id, $0) })
            self.connectionGenerations = Dictionary(uniqueKeysWithValues: (snapshot.monitors ?? []).compactMap { monitor in
                monitor.generation.map { (monitor.id, $0) }
            })
            self.latestPendingActionIDs = Set(actions.map(\.id))
            self.keyboardPendingMonitors = Set(actions.map(\.monitor_id))
            self.publishHardwareKeyDisplays()
            self.asleepActionIDs.formIntersection(self.latestPendingActionIDs)
            for (monitorID, state) in self.displayStates {
                if let generation = self.connectionGenerations[monitorID],
                   self.lastReportedDisplayStates[monitorID] != ReportedDisplayState(generation: generation, state: state) {
                    self.pendingDisplayStates[monitorID] = state
                }
            }
            self.flushPendingDisplayStates()
            for action in actions {
                guard self.latestPendingActionIDs.contains(action.id) else { continue }
                guard !self.asleepActionIDs.contains(action.id) else { continue }
                guard self.displayStates[action.monitor_id] != "asleep" else {
                    self.asleepActionIDs.insert(action.id)
                    continue
                }
                guard let transport = self.displays[action.monitor_id] else { continue }
                if CGDisplayIsAsleep(transport.displayID) != 0 {
                    self.blockForSleep(action)
                    self.reportDisplayState(action.monitor_id, state: "asleep")
                    continue
                }
                guard self.remember(action.id) else { continue }
                self.perform(action)
            }
        }
    }

    /// Called by the router's bounded worker, never from the system event callback.
    /// Validation and Rust submission share the DDC controller's serial queue with
    /// discovery and display-state updates.
    func submitHardwareKey(_ command: HardwareKeyCommand, completion: @escaping (Bool) -> Void) {
        queue.async { [weak self] in
            guard let self, !self.stopped,
                  let transport = self.displays[command.monitorID],
                  transport.identityIsStable,
                  self.connectionGenerations[command.monitorID] == command.generation,
                  let monitor = self.latestMonitors[command.monitorID],
                  monitor.generation == command.generation,
                  monitor.identity_status == "stable",
                  self.displayStates[command.monitorID] != "asleep",
                  CGDisplayIsAsleep(transport.displayID) == 0 else {
                completion(false)
                return
            }
            let feature = command.feature == .brightness ? monitor.brightness : monitor.volume
            guard feature?.supported == true else { completion(false); return }
            // The final validity check and submission share the main executor with UI controls.
            // A slider accepted after this command was queued invalidates its authorization epoch.
            DispatchQueue.main.async { [weak core = self.core] in
                completion(core?.submitHardwareKey(command) ?? false)
            }
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
                let width = (value?.max ?? 100) - (value?.min ?? 0)
                let pending = monitorID.map { keyboardPendingMonitors.contains($0) } ?? false
                let tolerance = width > 0 ? 50.0 / width + 0.0001 : 0.51
                let status = value?.status ?? "ready"
                let failed = !["ready", "pending", "verifying_ownership", "read_required"].contains(status)
                return HardwareKeyFeatureState(value: value?.value, supported: value?.supported == true,
                                               awake: awake, pending: pending, tolerance: tolerance, failed: failed)
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
                if prior.service != nil, let service = transport.service, service != prior.service {
                    Unmanaged<AnyObject>.fromOpaque(service).release()
                }
                let service = prior.service ?? transport.service
                let chipAddress = prior.service == nil ? transport.chipAddress : prior.chipAddress
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
                core?.submit(["type": "monitor_discovered", "monitor": [
                    "id": key,
                    "name": transport.name,
                    "identity_status": transport.identityIsStable ? "stable" : "ambiguous",
                    "connection_path": transport.location,
                    "brightness": featureJSON(brightness),
                    "volume": featureJSON(volume)
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
                    "volume": featureJSON(volume)
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

    private func blockForSleep(_ action: PendingAction) {
        asleepActionIDs.insert(action.id)
        if handledActions.remove(action.id) != nil {
            handledActionOrder.removeAll { $0 == action.id }
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
            core?.submit(["type": "monitor_disconnected", "monitor_id": removed])
        }
        active = present
    }

    private func remember(_ actionID: UInt64) -> Bool {
        guard handledActions.insert(actionID).inserted else { return false }
        handledActionOrder.append(actionID)
        if handledActionOrder.count > 512 {
            handledActions.remove(handledActionOrder.removeFirst())
        }
        return true
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

    private func perform(_ action: PendingAction) {
        guard displayStates[action.monitor_id] != "asleep" else { return }
        guard let transport = displays[action.monitor_id], transport.identityIsStable else {
            submitResult(action, ok: false, error: displays[action.monitor_id] == nil ? "disconnected" : "ambiguous")
            return
        }
        guard transport.service != nil else { submitResult(action, ok: false, error: "unsupported"); return }
        let feature: UInt8
        switch action.kind {
        case "read_brightness", "set_brightness": feature = 0x10
        case "set_volume": feature = 0x62
        default: submitResult(action, ok: false, error: "unsupported"); return
        }
        if action.kind.hasPrefix("set_") {
            guard let target = action.value,
                  let current = readVCP(transport: transport, code: feature), current.maximum > 0 else {
                submitResult(action, ok: false, error: "unsupported"); return
            }
            // Core has already mapped the user-facing 0...100 value into this
            // feature's raw DDC range before creating the hardware action.
            let raw = UInt16(max(0, min(Double(current.maximum), target.rounded())))
            guard let currentDisplayID = displays[action.monitor_id]?.displayID,
                  CGDisplayIsAsleep(currentDisplayID) == 0,
                  displayStates[action.monitor_id] != "asleep" else {
                blockForSleep(action)
                reportDisplayState(action.monitor_id, state: "asleep")
                return
            }
            guard writeVCP(transport: transport, code: feature, value: raw) else {
                submitResult(action, ok: false, error: "io"); return
            }
            Thread.sleep(forTimeInterval: 0.06)
            guard let observed = readVCP(transport: transport, code: feature) else {
                submitResult(action, ok: false, error: "io"); return
            }
            // IOAV submission success does not acknowledge a Set VCP command.
            // An unchanged or clamped value must not be reported as applied.
            guard observed.current == raw else {
                submitResult(action, ok: false, error: "io"); return
            }
            submitResult(action, ok: true, value: Double(observed.current))
            return
        }
        guard let observed = readVCP(transport: transport, code: feature) else {
            submitResult(action, ok: false, error: "unsupported"); return
        }
        submitResult(action, ok: true, value: Double(observed.current))
    }

    private func submitResult(_ action: PendingAction, ok: Bool, value: Double? = nil, error: String? = nil) {
        guard let core else { return }
        _ = core.submitHardwareResult(action, connectionGeneration: connectionGenerations[action.monitor_id] ?? 0,
                                      success: ok, value: value, error: error)
    }

    private typealias Reading = DDCTransaction.Reading

    private func readVCP(transport: DisplayTransport, code: UInt8) -> Reading? {
        transaction(transport).readVCP(code: code)
    }

    private func writeVCP(transport: DisplayTransport, code: UInt8, value: UInt16) -> Bool {
        transaction(transport).writeVCP(code: code, value: value)
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
