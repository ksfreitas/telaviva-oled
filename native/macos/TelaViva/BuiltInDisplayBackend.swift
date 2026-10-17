import CoreGraphics
import Darwin
import Foundation
import IOKit
import ObjectiveC

enum BuiltInUnavailableReason: String {
    case noBuiltInDisplay
    case brightnessNotAdjustable
    case apiUnavailable
    /// Built-in display is off, asleep, mirrored or the lid is closed.
    case displayOffline
}

struct BuiltInDisplayStatus: Equatable {
    var available: Bool
    var reason: BuiltInUnavailableReason?
    var displayID: UInt32?
}

/// Built-in display access. Implementations must be thread-safe: every method may be
/// called from any queue. Failures return false, nil or an unavailable status; they never throw or trap.
protocol BuiltInDisplayBackend: AnyObject {
    func status() -> BuiltInDisplayStatus
    /// True when the built-in display is asleep or not present.
    func isAsleep() -> Bool
    /// Brightness as a percent in 0...100, or nil when it cannot be read.
    func readLevel() -> Double?
    func writeLevel(_ percent: Double) -> Bool
    /// Automatic brightness state, or nil when unknown.
    func autoBrightnessEnabled() -> Bool?
    func setAutoBrightness(_ enabled: Bool) -> Bool
}

/// Built-in display backend over undocumented Apple frameworks, loaded at runtime and never linked.
/// These APIs are undocumented and were verified only by reflection on one development Mac.
/// The brightness setter and the automatic-brightness setter are unverified on hardware.
/// Every failure fails closed: a missing symbol, class or selector, a nonzero status,
/// an NSError or an unexpected result yields unavailable, nil or false.
final class DisplayServicesBackend: BuiltInDisplayBackend {
    private typealias CanChangeFunction = @convention(c) (UInt32) -> Bool
    private typealias GetBrightnessFunction = @convention(c) (UInt32, UnsafeMutablePointer<Float>) -> Int32
    private typealias SetBrightnessFunction = @convention(c) (UInt32, Float) -> Int32

    private struct Symbols {
        let canChange: CanChangeFunction
        let getBrightness: GetBrightnessFunction
        let setBrightness: SetBrightnessFunction
    }

    private typealias ObjectMessage = @convention(c) (AnyObject, Selector) -> UnsafeMutableRawPointer?
    private typealias BoolMessage = @convention(c) (AnyObject, Selector, UnsafeMutablePointer<NSError?>?) -> Bool
    private typealias DisplayClientMessage = @convention(c) (AnyObject, Selector, UInt32, UnsafeMutablePointer<NSError?>?) -> UnsafeMutableRawPointer?
    private typealias SetBoolMessage = @convention(c) (AnyObject, Selector, Bool, UnsafeMutablePointer<NSError?>?) -> Bool

    /// Loaded once on first use; nil when the framework or any symbol is missing.
    private static let symbols: Symbols? = loadSymbols()
    private static let coreBrightnessLoaded: Bool =
        dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_LAZY) != nil

    private let autoLock = NSLock()
    // Guarded by autoLock.
    private var cbClient: AnyObject?
    private var displayClientCache: (displayID: UInt32, client: AnyObject)?

    func status() -> BuiltInDisplayStatus {
        let found = Self.onlineBuiltInDisplay()
        if let reason = Self.offlineReason(
            builtInFound: found != nil,
            lidClosed: Self.lidClosed(),
            online: found.map { CGDisplayIsOnline($0) != 0 } ?? false,
            active: found.map { CGDisplayIsActive($0) != 0 } ?? false,
            asleep: found.map { CGDisplayIsAsleep($0) != 0 } ?? false
        ) {
            return BuiltInDisplayStatus(available: false, reason: reason, displayID: found)
        }
        guard let id = found else {
            return BuiltInDisplayStatus(available: false, reason: .noBuiltInDisplay, displayID: nil)
        }
        guard let symbols = Self.symbols else {
            return BuiltInDisplayStatus(available: false, reason: .apiUnavailable, displayID: id)
        }
        guard symbols.canChange(id) else {
            return BuiltInDisplayStatus(available: false, reason: .brightnessNotAdjustable, displayID: id)
        }
        return BuiltInDisplayStatus(available: true, reason: nil, displayID: id)
    }

    /// True when the MacBook lid is closed; false when unknown or on a Mac without a lid.
    static func lidClosed() -> Bool {
        let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"))
        guard service != 0 else { return false }
        defer { IOObjectRelease(service) }
        let value = IORegistryEntryCreateCFProperty(service, "AppleClamshellState" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
        return (value as? Bool) ?? false
    }

    /// Why the built-in display cannot be followed, or nil when it is on. A closed lid is always offline, even when the display left the online list.
    static func offlineReason(builtInFound: Bool, lidClosed: Bool, online: Bool, active: Bool, asleep: Bool) -> BuiltInUnavailableReason? {
        if lidClosed { return .displayOffline }
        guard builtInFound else { return .noBuiltInDisplay }
        return online && active && !asleep ? nil : .displayOffline
    }

    func isAsleep() -> Bool {
        guard let id = Self.onlineBuiltInDisplay() else { return true }
        return CGDisplayIsAsleep(id) != 0
    }

    func readLevel() -> Double? {
        guard let symbols = Self.symbols, let id = adjustableDisplayID() else { return nil }
        var level: Float = 0
        guard symbols.getBrightness(id, &level) == 0, level.isFinite else { return nil }
        return Double(min(max(level, 0), 1)) * 100
    }

    func writeLevel(_ percent: Double) -> Bool {
        guard percent.isFinite, let symbols = Self.symbols, let id = adjustableDisplayID() else { return false }
        let level = Float(min(max(percent, 0), 100) / 100)
        return symbols.setBrightness(id, level) == 0
    }

    func autoBrightnessEnabled() -> Bool? {
        autoLock.lock()
        defer { autoLock.unlock() }
        guard let displayID = Self.onlineBuiltInDisplay(),
              let client = brightnessClientLocked(displayID: displayID),
              let method = Self.lookup(client, "isAutobrightnessEnabledWithError:") else { return nil }
        let message = unsafeBitCast(method.imp, to: BoolMessage.self)
        var error: NSError?
        let enabled = message(client, method.sel, &error)
        return error == nil ? enabled : nil
    }

    func setAutoBrightness(_ enabled: Bool) -> Bool {
        autoLock.lock()
        defer { autoLock.unlock() }
        guard let displayID = Self.onlineBuiltInDisplay(),
              let client = brightnessClientLocked(displayID: displayID),
              let method = Self.lookup(client, "setAutoBrightnessEnabled:error:") else { return false }
        let message = unsafeBitCast(method.imp, to: SetBoolMessage.self)
        var error: NSError?
        let applied = message(client, method.sel, enabled, &error)
        return applied && error == nil
    }

    /// The built-in display ID only when it is online, active and reports adjustable brightness.
    private func adjustableDisplayID() -> UInt32? {
        let current = status()
        return current.available ? current.displayID : nil
    }

    private static func onlineBuiltInDisplay() -> UInt32? {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(UInt32(ids.count), &ids, &count) == .success else { return nil }
        return ids.prefix(Int(count)).first(where: { CGDisplayIsBuiltin($0) != 0 })
    }

    private static func loadSymbols() -> Symbols? {
        guard let handle = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_LAZY),
              let canChange = dlsym(handle, "DisplayServicesCanChangeBrightness"),
              let getBrightness = dlsym(handle, "DisplayServicesGetBrightness"),
              let setBrightness = dlsym(handle, "DisplayServicesSetBrightness") else { return nil }
        return Symbols(
            canChange: unsafeBitCast(canChange, to: CanChangeFunction.self),
            getBrightness: unsafeBitCast(getBrightness, to: GetBrightnessFunction.self),
            setBrightness: unsafeBitCast(setBrightness, to: SetBrightnessFunction.self)
        )
    }

    /// Caller must hold `autoLock`. Creates the CoreBrightness client chain once per display and caches it.
    private func brightnessClientLocked(displayID: UInt32) -> AnyObject? {
        guard Self.coreBrightnessLoaded else { return nil }
        if cbClient == nil { cbClient = Self.makeCBClient() }
        guard let root = cbClient else { return nil }
        if displayClientCache?.displayID != displayID {
            displayClientCache = nil
            guard let created = Self.newDisplayClient(root, displayID: displayID) else { return nil }
            displayClientCache = (displayID: displayID, client: created)
        }
        guard let displayClient = displayClientCache?.client else { return nil }
        return Self.brightnessClient(of: displayClient)
    }

    private static func makeCBClient() -> AnyObject? {
        guard let cbClass = NSClassFromString("CBClient") as? NSObject.Type else { return nil }
        let client: AnyObject = cbClass.init()
        guard let method = lookup(client, "activateWithError:") else { return nil }
        let activate = unsafeBitCast(method.imp, to: BoolMessage.self)
        var error: NSError?
        let activated = activate(client, method.sel, &error)
        return activated && error == nil ? client : nil
    }

    private static func newDisplayClient(_ client: AnyObject, displayID: UInt32) -> AnyObject? {
        guard let method = lookup(client, "newDisplayClientForID:withError:") else { return nil }
        let message = unsafeBitCast(method.imp, to: DisplayClientMessage.self)
        var error: NSError?
        let created = object(message(client, method.sel, displayID, &error), retained: true)
        return error == nil ? created : nil
    }

    private static func brightnessClient(of displayClient: AnyObject) -> AnyObject? {
        guard let method = lookup(displayClient, "brightnessClient") else { return nil }
        let message = unsafeBitCast(method.imp, to: ObjectMessage.self)
        return object(message(displayClient, method.sel), retained: false)
    }

    /// Resolves a selector only when the receiver's class responds to it, so a missing method never reaches a forwarder.
    private static func lookup(_ target: AnyObject, _ name: String) -> (imp: IMP, sel: Selector)? {
        let sel = NSSelectorFromString(name)
        guard let cls = object_getClass(target), class_respondsToSelector(cls, sel) else { return nil }
        let imp: IMP? = class_getMethodImplementation(cls, sel)
        guard let imp else { return nil }
        return (imp, sel)
    }

    /// `retained` is true for alloc/init/new-family results, which carry a +1 reference.
    private static func object(_ raw: UnsafeMutableRawPointer?, retained: Bool) -> AnyObject? {
        guard let raw else { return nil }
        let unmanaged = Unmanaged<AnyObject>.fromOpaque(raw)
        return retained ? unmanaged.takeRetainedValue() : unmanaged.takeUnretainedValue()
    }
}
