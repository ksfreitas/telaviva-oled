import CoreGraphics
import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("MediaKeyProbeTests: \(message)") }
}

private let keyDown = CGEventType.keyDown.rawValue
private let keyUp = CGEventType.keyUp.rawValue
private let sysDefined: UInt32 = 14 // NX_SYSDEFINED

private func describe(_ type: UInt32, subtype: Int = 0, data1: Int64 = 0, code: Int64 = 0,
                      isRepeat: Bool = false, flags: CGEventFlags = []) -> String? {
    MediaKeyProbePolicy.describe(typeRaw: type, subtype: subtype, data1: data1, keyCode: code,
                                 isRepeat: isRepeat, flags: flags)
}

private func auxData(key: Int64, state: Int64, repeating: Int64) -> Int64 {
    (key << 16) | (state << 8) | repeating
}

private let lineShape = try! NSRegularExpression(
    pattern: "^(sysdefined subtype=[0-9]+( key=[0-9]+ state=0x[0-9A-F]{2} repeat=[01] flags=(none|[a-z]+(\\+[a-z]+)*))?|key (down|up) code=[0-9]+ repeat=[01] flags=(none|[a-z]+(\\+[a-z]+)*))$")

private func matchesShape(_ line: String) -> Bool {
    lineShape.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)) != nil
}

@main
private struct MediaKeyProbeTests {
    static func main() {
        ordinaryKeysAreIgnored()
        allowlistedKeysAreListed()
        auxControlButtonsAreDecoded()
        otherSubtypesNeverIncludePayload()
        modifierNames()
        otherEventTypesAreIgnored()
        linesMatchConservativeShape()
        print("Media key probe tests passed")
    }

    static func ordinaryKeysAreIgnored() {
        let ordinary: [Int64] = [0, 1, 2, 13, 36, 48, 49, 51, 53, 123, 124, 125, 126, 12, 14, 18, 29, 46, 55, 56, 117, 119, 121]
        for code in ordinary {
            expect(describe(keyDown, code: code) == nil, "keyDown \(code) must be ignored")
            expect(describe(keyUp, code: code) == nil, "keyUp \(code) must be ignored")
        }
    }

    static func allowlistedKeysAreListed() {
        for code: Int64 in [122, 120, 144, 145, 160, 177] {
            expect(describe(keyDown, code: code) == "key down code=\(code) repeat=0 flags=none", "keyDown \(code)")
            expect(describe(keyUp, code: code, isRepeat: true) == "key up code=\(code) repeat=1 flags=none", "keyUp \(code)")
        }
        for code in MediaKeyProbePolicy.allowedKeyCodes {
            expect(describe(keyDown, code: code) != nil, "allowlist \(code)")
        }
        expect(MediaKeyProbePolicy.allowedKeyCodes.count == 26, "allowlist size")
    }

    static func auxControlButtonsAreDecoded() {
        for key: Int64 in [0, 1, 2, 3, 7] {
            let down = describe(sysDefined, subtype: 8, data1: auxData(key: key, state: 0x0A, repeating: 0))
            expect(down == "sysdefined subtype=8 key=\(key) state=0x0A repeat=0 flags=none", "down key \(key): \(down ?? "nil")")
            let up = describe(sysDefined, subtype: 8, data1: auxData(key: key, state: 0x0B, repeating: 1), flags: .maskShift)
            expect(up == "sysdefined subtype=8 key=\(key) state=0x0B repeat=1 flags=shift", "up key \(key): \(up ?? "nil")")
        }
    }

    static func otherSubtypesNeverIncludePayload() {
        for subtype in [0, 1, 7, 9, 16] {
            let line = describe(sysDefined, subtype: subtype, data1: 0x1234_5678)
            expect(line == "sysdefined subtype=\(subtype)", "subtype \(subtype): \(line ?? "nil")")
        }
    }

    static func modifierNames() {
        expect(MediaKeyProbePolicy.flagNames([]) == "none", "none")
        expect(MediaKeyProbePolicy.flagNames([.maskShift, .maskControl, .maskAlternate, .maskCommand, .maskSecondaryFn])
               == "shift+control+option+command+fn", "all modifiers")
        expect(MediaKeyProbePolicy.flagNames(.maskSecondaryFn) == "fn", "fn")
        let noise: CGEventFlags = [.maskAlphaShift, .maskNumericPad, .maskHelp, .maskNonCoalesced, CGEventFlags(rawValue: 0x8000_0000)]
        expect(MediaKeyProbePolicy.flagNames(noise) == "none", "unknown bits omitted")
        expect(MediaKeyProbePolicy.flagNames(noise.union(.maskCommand)) == "command", "unknown bits omitted next to modifiers")
    }

    static func otherEventTypesAreIgnored() {
        for raw: UInt32 in [CGEventType.leftMouseDown.rawValue, CGEventType.flagsChanged.rawValue, CGEventType.mouseMoved.rawValue, 0, 99] {
            expect(describe(raw, code: 122) == nil, "type \(raw) must be ignored")
        }
    }

    static func linesMatchConservativeShape() {
        var lines: [String] = []
        for code in MediaKeyProbePolicy.allowedKeyCodes {
            lines.append(describe(keyDown, code: code, flags: [.maskShift, .maskSecondaryFn])!)
            lines.append(describe(keyUp, code: code, isRepeat: true)!)
        }
        for key: Int64 in [0, 1, 2, 3, 7, 20, 65535] {
            lines.append(describe(sysDefined, subtype: 8, data1: auxData(key: key, state: 0xFF, repeating: 1), flags: .maskCommand)!)
        }
        lines.append(describe(sysDefined, subtype: 3, data1: -1)!)
        for line in lines { expect(matchesShape(line), "unexpected line shape: \(line)") }
    }
}
