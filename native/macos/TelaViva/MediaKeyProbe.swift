import AppKit
import ApplicationServices
import CoreGraphics
import Foundation
import IOKit.hidsystem

/// Pure formatting rules for the `--probe-media-keys` diagnostic.
/// Only media and function-key events produce a line; every other key returns nil.
enum MediaKeyProbePolicy {
    /// F1...F20 followed by media-adjacent key codes. Nothing outside this list is ever listed.
    static let allowedKeyCodes: Set<Int64> = [
        122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111, 105, 107, 113, 106, 64, 79, 80, 90,
        144, 145, 160, 177, 178, 179,
    ]

    static func describe(typeRaw: UInt32, subtype: Int, data1: Int64, keyCode: Int64,
                         isRepeat: Bool, flags: CGEventFlags) -> String? {
        if typeRaw == UInt32(NX_SYSDEFINED) {
            guard subtype == Int(NX_SUBTYPE_AUX_CONTROL_BUTTONS) else { return "sysdefined subtype=\(subtype)" }
            let payload = UInt64(bitPattern: data1)
            let keyType = (payload >> 16) & 0xFFFF
            let state = (payload >> 8) & 0xFF
            let repeating = payload & 1
            return "sysdefined subtype=\(subtype) key=\(keyType) state=\(String(format: "0x%02X", state)) repeat=\(repeating) flags=\(flagNames(flags))"
        }
        let isDown = typeRaw == CGEventType.keyDown.rawValue
        guard isDown || typeRaw == CGEventType.keyUp.rawValue, allowedKeyCodes.contains(keyCode) else { return nil }
        return "key \(isDown ? "down" : "up") code=\(keyCode) repeat=\(isRepeat ? 1 : 0) flags=\(flagNames(flags))"
    }

    /// Modifier names only; no other flag bit is ever reported.
    static func flagNames(_ flags: CGEventFlags) -> String {
        let table: [(CGEventFlags, String)] = [
            (.maskShift, "shift"), (.maskControl, "control"), (.maskAlternate, "option"),
            (.maskCommand, "command"), (.maskSecondaryFn, "fn"),
        ]
        let names = table.filter { flags.contains($0.0) }.map { $0.1 }
        return names.isEmpty ? "none" : names.joined(separator: "+")
    }
}

private final class ProbeCounters {
    var listed = 0
    var ignored = 0
}

enum MediaKeyProbe {
    /// Listen-only: events are never modified, consumed, or injected. Output is stdout only.
    static func run(seconds: Int) -> Int32 {
        print("TelaViva media-key probe: only media and function-key events are listed; letters, digits, and text are never recorded.")
        let accessibility = AXIsProcessTrusted()
        let inputMonitoring = CGPreflightListenEventAccess()
        print("accessibility=\(accessibility) inputMonitoring=\(inputMonitoring)")
        guard accessibility && inputMonitoring else {
            if !accessibility { print("missing: Accessibility") }
            if !inputMonitoring { print("missing: Input Monitoring") }
            fflush(stdout)
            return 2
        }

        let counters = ProbeCounters()
        let types: [UInt32] = [UInt32(NX_SYSDEFINED), CGEventType.keyDown.rawValue, CGEventType.keyUp.rawValue]
        let mask = types.reduce(CGEventMask(0)) { $0 | (CGEventMask(1) << CGEventMask($1)) }
        let callback: CGEventTapCallBack = { _, type, event, userInfo in
            guard let userInfo else { return Unmanaged.passUnretained(event) }
            let counters = Unmanaged<ProbeCounters>.fromOpaque(userInfo).takeUnretainedValue()
            var subtype = 0
            var data1: Int64 = 0
            if type.rawValue == UInt32(NX_SYSDEFINED), let native = NSEvent(cgEvent: event) {
                subtype = Int(native.subtype.rawValue)
                data1 = Int64(native.data1)
            }
            let line = MediaKeyProbePolicy.describe(
                typeRaw: type.rawValue, subtype: subtype, data1: data1,
                keyCode: event.getIntegerValueField(.keyboardEventKeycode),
                isRepeat: event.getIntegerValueField(.keyboardEventAutorepeat) != 0,
                flags: event.flags)
            if let line {
                counters.listed += 1
                print(line)
                fflush(stdout)
            } else {
                counters.ignored += 1
            }
            return Unmanaged.passUnretained(event)
        }
        guard let tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap,
                                          options: .listenOnly, eventsOfInterest: mask, callback: callback,
                                          userInfo: Unmanaged.passUnretained(counters).toOpaque()) else {
            print("event tap could not be created")
            fflush(stdout)
            return 2
        }
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        print("listening for \(min(60, max(5, seconds))) seconds")
        fflush(stdout)
        CFRunLoopRunInMode(.defaultMode, CFTimeInterval(min(60, max(5, seconds))), false)
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, .commonModes)
        CFMachPortInvalidate(tap)
        print("done events_listed=\(counters.listed) events_ignored=\(counters.ignored)")
        fflush(stdout)
        return 0
    }
}
