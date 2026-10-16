import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("DefaultAudioOutputMonitorTests: \(message)") }
}

/// Mutable UID source injected into the monitor in place of CoreAudio.
private final class UIDBox {
    var value: String?

    init(_ value: String?) {
        self.value = value
    }
}

private final class ReportLog {
    var values: [String?] = []
}

@main
private struct DefaultAudioOutputMonitorTests {
    static func main() {
        startReportsCurrentValueOnce()
        unchangedValueDoesNotReportAgain()
        changedValueReportsOnce()
        valueBecomingNilReportsNil()
        stopSilencesLaterRefreshes()
        doubleStartAndStopAreSafe()
        startAfterStopReportsCurrentValueAgain()
        nilAtStartReportsOnce()
        print("Default audio output monitor tests passed")
    }

    private static func makeMonitor(_ box: UIDBox, _ log: ReportLog) -> DefaultAudioOutputMonitor {
        let monitor = DefaultAudioOutputMonitor(readDefaultOutputUID: { box.value })
        monitor.onChange = { log.values.append($0) }
        return monitor
    }

    private static func startReportsCurrentValueOnce() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        expect(log.values.count == 1 && log.values[0] == "uid-a", "start reports the current UID once")
        monitor.refreshForTesting()
        expect(log.values.count == 1, "start does not report the same value again on refresh")
    }

    private static func unchangedValueDoesNotReportAgain() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        monitor.refreshForTesting()
        monitor.refreshForTesting()
        expect(log.values.count == 1, "unchanged value does not report again")
    }

    private static func changedValueReportsOnce() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        box.value = "uid-b"
        monitor.refreshForTesting()
        monitor.refreshForTesting()
        expect(log.values.count == 2 && log.values[0] == "uid-a" && log.values[1] == "uid-b",
               "changed value reports once")
    }

    private static func valueBecomingNilReportsNil() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        box.value = nil
        monitor.refreshForTesting()
        monitor.refreshForTesting()
        expect(log.values.count == 2 && log.values[1] == nil, "value becoming nil reports nil once")
    }

    private static func stopSilencesLaterRefreshes() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        monitor.stop()
        box.value = "uid-b"
        monitor.refreshForTesting()
        expect(log.values.count == 1, "stop silences later refreshes")
    }

    private static func doubleStartAndStopAreSafe() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        monitor.start()
        expect(log.values.count == 1, "second start does not report again")
        box.value = "uid-b"
        monitor.refreshForTesting()
        expect(log.values.count == 2 && log.values[1] == "uid-b", "double start keeps a single observer")

        monitor.stop()
        monitor.stop()
        box.value = "uid-c"
        monitor.refreshForTesting()
        expect(log.values.count == 2, "double stop leaves the monitor silent")
    }

    private static func startAfterStopReportsCurrentValueAgain() {
        let box = UIDBox("uid-a")
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        monitor.stop()
        monitor.start()
        expect(log.values.count == 2 && log.values[1] == "uid-a",
               "start after stop reports the current value again")
    }

    private static func nilAtStartReportsOnce() {
        let box = UIDBox(nil)
        let log = ReportLog()
        let monitor = makeMonitor(box, log)
        defer { monitor.stop() }

        monitor.start()
        expect(log.values.count == 1 && log.values[0] == nil, "nil at start reports nil once")
        monitor.refreshForTesting()
        expect(log.values.count == 1, "repeated nil does not report again")
    }
}
