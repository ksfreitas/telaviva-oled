import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("DiagnosticLogTests: \(message)") }
}

@main
private struct DiagnosticLogTests {
    static func main() {
        shortIdMatchesTheSharedFNVVectors()
        shortIdIsEightLowercaseHexDigitsForEveryOneByteID()
        sinksReceiveTheLinesTheyAreGiven()
        drainedArrayIsForwardedLineByLine()
        malformedDrainsDecodeToNoLines()
        print("Diagnostic log tests passed")
    }

    private static func shortIdMatchesTheSharedFNVVectors() {
        expect(DiagnosticLog.shortId("abc") == "id-1a47e90b", "abc hashes to the shared FNV-1a vector")
        expect(DiagnosticLog.shortId("") == "id-811c9dc5", "the empty id is the FNV-1a offset basis")
    }

    private static func shortIdIsEightLowercaseHexDigitsForEveryOneByteID() {
        let allowed: Set<Character> = Set("0123456789abcdef")
        for byte in UInt8(0x20)...UInt8(0x7e) {
            let hash = DiagnosticLog.shortId(String(Character(UnicodeScalar(byte))))
            let digits = hash.dropFirst(3)
            expect(hash.hasPrefix("id-") && digits.count == 8 && digits.allSatisfy { allowed.contains($0) },
                   "every printable one-byte id formats as id- and eight lowercase hex digits")
        }
    }

    private static func sinksReceiveTheLinesTheyAreGiven() {
        var coreLines: [String] = []
        var ddcLines: [String] = []
        let defaultCore = DiagnosticLog.coreSink
        let defaultDDC = DiagnosticLog.ddcSink
        DiagnosticLog.coreSink = { coreLines.append($0) }
        DiagnosticLog.ddcSink = { ddcLines.append($0) }
        DiagnosticLog.core("restore_enqueued monitor=id-1a47e90b target=70 baseline=70")
        DiagnosticLog.ddc("display_sleep")
        DiagnosticLog.coreSink = defaultCore
        DiagnosticLog.ddcSink = defaultDDC
        expect(coreLines == ["restore_enqueued monitor=id-1a47e90b target=70 baseline=70"],
               "core lines reach the injected brightness sink unchanged")
        expect(ddcLines == ["display_sleep"], "ddc lines reach the injected ddc sink unchanged")
    }

    private static func drainedArrayIsForwardedLineByLine() {
        let lines = [
            "restore_enqueued monitor=id-1a47e90b target=70 baseline=70",
            "diagnostics_dropped count=2"
        ]
        let json = try! JSONSerialization.data(withJSONObject: lines)
        expect(CoreBridge.decodeDiagnostics(json) == lines, "a JSON array decodes to its lines in order")
        var coreLines: [String] = []
        let defaultCore = DiagnosticLog.coreSink
        DiagnosticLog.coreSink = { coreLines.append($0) }
        CoreBridge.forwardDiagnostics(json)
        DiagnosticLog.coreSink = defaultCore
        expect(coreLines == lines, "each drained line reaches the brightness sink, in order")
    }

    private static func malformedDrainsDecodeToNoLines() {
        expect(CoreBridge.decodeDiagnostics(Data()).isEmpty, "empty data yields no lines")
        expect(CoreBridge.decodeDiagnostics(Data("not json".utf8)).isEmpty, "non-JSON data yields no lines")
        expect(CoreBridge.decodeDiagnostics(Data(#"{"lines":[]}"#.utf8)).isEmpty, "a JSON object is not a line array")
        expect(CoreBridge.decodeDiagnostics(Data("[1,2]".utf8)).isEmpty, "an array of non-strings yields no lines")
        expect(CoreBridge.decodeDiagnostics(Data("[]".utf8)).isEmpty, "an empty array yields no lines")
    }
}
