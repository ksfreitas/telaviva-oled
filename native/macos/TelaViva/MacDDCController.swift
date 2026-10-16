import AppKit
import CoreGraphics
import Darwin
import Foundation
import IOKit
import IOKit.graphics

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
    private var stopped = false
    private var probeReport: [String: Any] = [:]
    private var lastAVSearchStage = "not_attempted"

    init(core: CoreBridge) { self.core = core }

    func start() {
        queue.async { [weak self] in self?.discoverDisplays() }
        observer = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in self?.enumerate() }
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
        guard let actions = snapshot.pending_actions else { return }
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            for action in actions where self.remember(action.id) {
                self.perform(action)
            }
        }
    }

    private func enumerate() {
        queue.async { [weak self] in
            guard let self, !self.stopped else { return }
            self.discoverDisplays()
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

    private func discoverDisplays() {
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
                core?.submit(["type": "monitor_discovered", "monitor": [
                    "id": key,
                    "name": transport.name,
                    "identity_status": transport.identityIsStable ? "stable" : "ambiguous",
                    "connection_path": transport.location,
                    "brightness": featureJSON(readVCP(transport: transport, code: 0x10)),
                    "volume": featureJSON(readVCP(transport: transport, code: 0x62))
                ]])
            }
        }
        publishDisconnectedDisplays(Set(displays.keys))
    }

    private func publishDisconnectedDisplays(_ present: Set<String>) {
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
            guard writeVCP(transport: transport, code: feature, value: raw) else {
                submitResult(action, ok: false, error: "io"); return
            }
            Thread.sleep(forTimeInterval: 0.06)
        }
        guard let observed = readVCP(transport: transport, code: feature) else {
            submitResult(action, ok: false, error: "unsupported"); return
        }
        submitResult(action, ok: true, value: Double(observed.current))
    }

    private func submitResult(_ action: PendingAction, ok: Bool, value: Double? = nil, error: String? = nil) {
        var result: [String: Any] = ["type": "hardware_result", "action_id": action.id,
                                     "generation": action.generation, "ok": ok]
        if let value { result["value"] = value }
        if let error { result["error"] = error }
        core?.submit(result)
    }

    private struct Reading { let current: UInt16; let maximum: UInt16 }

    private func readVCP(transport: DisplayTransport, code: UInt8) -> Reading? {
        retryIO { readVCPOnce(transport: transport, code: code) }
    }

    private func readVCPOnce(transport: DisplayTransport, code: UInt8) -> Reading? {
        // The DDC/CI spec includes the virtual host address in the packet XOR.
        // m1ddc 1.2.0's helper omits 0x51 on reads, so try that legacy checksum
        // only if the standards-form request receives no valid reply.
        for seed in [UInt8(0x6e ^ 0x51), UInt8(0x6e)] {
            var request: [UInt8] = [0x82, 0x01, code]
            request.append(checksum(request, seed: seed))
            guard writeI2C(transport, bytes: request) else { continue }
            Thread.sleep(forTimeInterval: transport.chipAddress == 0xB7 ? 0.06 : 0.04)
            var reply = [UInt8](repeating: 0, count: 12)
            guard readI2C(transport, bytes: &reply), reply[0] == 0x6e, reply[1] == 0x88,
                  reply[2] == 0x02, reply[3] == 0x00, reply[4] == code,
                  checksum(Array(reply.prefix(11)), seed: 0x50) == 0 else { continue }
            return Reading(current: (UInt16(reply[8]) << 8) | UInt16(reply[9]),
                           maximum: (UInt16(reply[6]) << 8) | UInt16(reply[7]))
        }
        return nil
    }

    private func writeVCP(transport: DisplayTransport, code: UInt8, value: UInt16) -> Bool {
        retryIO {
            var bytes: [UInt8] = [0x84, 0x03, code, UInt8(value >> 8), UInt8(value & 0xff)]
            bytes.append(checksum(bytes, seed: 0x6e ^ 0x51))
            return writeI2C(transport, bytes: bytes) ? true : nil
        } ?? false
    }

    private func retryIO<T>(_ operation: () -> T?) -> T? {
        for attempt in 0..<3 {
            if let result = operation() { return result }
            if attempt == 0 { Thread.sleep(forTimeInterval: 0.2) }
            else if attempt == 1 { Thread.sleep(forTimeInterval: 1.0) }
        }
        return nil
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

    private func checksum(_ bytes: [UInt8], seed: UInt8) -> UInt8 { bytes.reduce(seed) { $0 ^ $1 } }

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
            unsafeBitCast(address, to: Read.self)(service, transport.chipAddress, 0x51,
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
