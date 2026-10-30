import os

/// Diagnostic lines for the macOS unified log. Core lines (category `brightness`), shell lines (category `ddc`) and
/// lock results (category `session`) share the `event key=value` format and carry only hashed monitor ids and numbers.
/// The system keeps `.notice` messages; collect them with `log show --last 1d --predicate 'subsystem == "com.telaviva.oled"'`.
enum DiagnosticLog {
    private static let brightnessLogger = Logger(subsystem: "com.telaviva.oled", category: "brightness")
    private static let ddcLogger = Logger(subsystem: "com.telaviva.oled", category: "ddc")
    private static let sessionLogger = Logger(subsystem: "com.telaviva.oled", category: "session")

    /// Test seams: tests replace these to capture lines instead of writing to the unified log.
    static var coreSink: (String) -> Void = { line in DiagnosticLog.brightnessLogger.notice("\(line, privacy: .public)") }
    static var ddcSink: (String) -> Void = { line in DiagnosticLog.ddcLogger.notice("\(line, privacy: .public)") }
    static var sessionSink: (String) -> Void = { line in DiagnosticLog.sessionLogger.notice("\(line, privacy: .public)") }

    /// Logs a line the core drained through `tv_diagnostics_drain`.
    static func core(_ line: String) {
        coreSink(line)
    }

    /// Logs a line from the DDC shell.
    static func ddc(_ line: String) {
        ddcSink(line)
    }

    /// Logs a lock result line.
    static func session(_ line: String) {
        sessionSink(line)
    }

    /// The monitor id hash shared with the Windows `shortId()`: `id-` and eight lowercase hex digits of the FNV-1a
    /// 32-bit hash of the UTF-8 bytes. Lines correlate across core and shell without writing the raw id.
    static func shortId(_ id: String) -> String {
        var hash: UInt32 = 2_166_136_261
        for byte in id.utf8 {
            hash ^= UInt32(byte)
            hash = hash &* 16_777_619
        }
        let hex = String(hash, radix: 16)
        return "id-" + String(repeating: "0", count: 8 - hex.count) + hex
    }
}
