import Darwin
import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String, line: UInt = #line) {
    guard condition() else {
        FileHandle.standardError.write(Data("BuiltInBrightnessMonitorTests line \(line): \(message)\n".utf8))
        exit(1)
    }
}

private func reportAvailable(_ brightness: Int?, calibrating: Bool = false) -> BuiltInDisplayReport {
    BuiltInDisplayReport(available: true, brightness: brightness, calibrating: calibrating, reason: nil)
}

private func reportUnavailable(_ reason: BuiltInUnavailableReason) -> BuiltInDisplayReport {
    BuiltInDisplayReport(available: false, brightness: nil, calibrating: false, reason: reason)
}

private final class FakeBackend: BuiltInDisplayBackend {
    var statusValue = BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)
    var asleep = false
    var level: Double? = 40
    private(set) var reads = 0

    func status() -> BuiltInDisplayStatus { statusValue }
    func isAsleep() -> Bool { asleep }
    func readLevel() -> Double? {
        reads += 1
        return level
    }
    func writeLevel(_ percent: Double) -> Bool { false }
    func autoBrightnessEnabled() -> Bool? { nil }
    func setAutoBrightness(_ enabled: Bool) -> Bool { false }
}

/// Records registrations. Only live timers fire: a canceled or already-fired one-shot never runs.
private final class FakeScheduler: BuiltInMonitorScheduling {
    final class Registration: BuiltInMonitorCancelable {
        /// Repeating interval, or the delay for a one-shot.
        let interval: TimeInterval
        /// Repeating leeway; 0 for a one-shot.
        let leeway: TimeInterval
        let block: () -> Void
        private(set) var canceled = false
        var fired = false

        init(interval: TimeInterval, leeway: TimeInterval, block: @escaping () -> Void) {
            self.interval = interval
            self.leeway = leeway
            self.block = block
        }

        var isLive: Bool { !canceled && !fired }
        func cancel() { canceled = true }
    }

    private(set) var repeatingRegistrations: [Registration] = []
    private(set) var onceRegistrations: [Registration] = []

    var liveRepeating: Int { repeatingRegistrations.filter { $0.isLive }.count }
    var liveOnce: Int { onceRegistrations.filter { $0.isLive }.count }

    func repeating(interval: TimeInterval, leeway: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable {
        let registration = Registration(interval: interval, leeway: leeway, block: block)
        repeatingRegistrations.append(registration)
        return registration
    }

    func once(after: TimeInterval, _ block: @escaping () -> Void) -> BuiltInMonitorCancelable {
        let registration = Registration(interval: after, leeway: 0, block: block)
        onceRegistrations.append(registration)
        return registration
    }

    /// Runs every live repeating timer once and returns how many ran.
    @discardableResult
    func fireRepeating() -> Int {
        let live = repeatingRegistrations.filter { $0.isLive }
        for registration in live { registration.block() }
        return live.count
    }

    /// Runs every live one-shot once, retires it, and returns how many ran.
    @discardableResult
    func fireOnce() -> Int {
        let live = onceRegistrations.filter { $0.isLive }
        for registration in live {
            registration.fired = true
            registration.block()
        }
        return live.count
    }
}

private final class ReportLog {
    private(set) var reports: [BuiltInDisplayReport] = []

    func record(_ report: BuiltInDisplayReport) {
        reports.append(report)
    }
}

private final class Harness {
    let backend: FakeBackend
    let scheduler: FakeScheduler
    let log: ReportLog
    let monitor: BuiltInBrightnessMonitor

    init() {
        let backend = FakeBackend()
        let scheduler = FakeScheduler()
        let log = ReportLog()
        self.backend = backend
        self.scheduler = scheduler
        self.log = log
        monitor = BuiltInBrightnessMonitor(backend: backend, scheduler: scheduler, submit: log.record)
    }
}

@main
private struct BuiltInBrightnessMonitorTests {
    static func main() {
        startWhileDisabledReportsAvailabilityOnly()
        enablingReadsRoundedLevelAndArmsOneRepeatingTimer()
        ticksDebounceOnlyChangesOfAtLeastOnePercent()
        changesInsideDebounceCoalesceToLatestValue()
        sameRoundedValueIsNotReported()
        failedReadKeepsLastReportedValue()
        asleepTickSkipsRead()
        calibratingCancelsTimerAndRestoresOnExit()
        availabilityLossAndRestoreTransitions()
        disablingCancelsTimerAndDebounce()
        enablingWhileUnavailableWaitsForAvailability()
        stopCancelsEverything()
        tickDetectsAvailabilityLoss()
        deliveryDropsSupersededReports()
        offlineReasonClassifiesLidAndSleep()
        pausesWhileDisplayIsOffAndResumesWithFreshLevel()
        print("Built-in brightness monitor tests passed")
    }

    private static func startWhileDisabledReportsAvailabilityOnly() {
        let h = Harness()
        h.monitor.start()
        expect(h.log.reports == [reportAvailable(nil)], "start reports availability with no level")
        expect(h.scheduler.repeatingRegistrations.isEmpty && h.scheduler.onceRegistrations.isEmpty,
               "disabled start schedules no timers")
        expect(h.backend.reads == 0, "disabled start reads no level")

        h.monitor.setEnabled(true)
        expect(h.scheduler.liveRepeating == 1 && h.backend.reads == 1,
               "positive control: enabling arms a timer and reads once")
    }

    private static func enablingReadsRoundedLevelAndArmsOneRepeatingTimer() {
        let h = Harness()
        h.backend.level = 40.6
        h.monitor.start()
        h.monitor.setEnabled(true)
        expect(h.backend.reads == 1, "enabling reads the level right away")
        expect(h.log.reports == [reportAvailable(nil), reportAvailable(41)], "enabling reports the rounded level")
        expect(h.scheduler.repeatingRegistrations.count == 1 && h.scheduler.liveRepeating == 1,
               "enabling schedules exactly one repeating timer")
        let timer = h.scheduler.repeatingRegistrations[0]
        expect(timer.interval == 1.0 && timer.leeway == 0.5, "repeating timer uses a 1.0 s interval and 0.5 s leeway")
        expect(h.scheduler.onceRegistrations.isEmpty, "enabling schedules no debounce")

        h.scheduler.fireRepeating()
        expect(h.backend.reads == 2, "positive control: the armed timer ticks and reads")
    }

    private static func ticksDebounceOnlyChangesOfAtLeastOnePercent() {
        let h = Harness()
        h.backend.level = 44.0
        h.monitor.start()
        h.monitor.setEnabled(true)

        h.backend.level = 44.4
        h.scheduler.fireRepeating()
        expect(h.backend.reads == 2 && h.scheduler.onceRegistrations.isEmpty && h.log.reports.count == 2,
               "a tick that rounds to the reported 44 schedules no debounce")

        h.backend.level = 44.6
        h.scheduler.fireRepeating()
        expect(h.scheduler.onceRegistrations.count == 1 && h.scheduler.liveOnce == 1,
               "a tick that rounds to 45 schedules one debounce")
        expect(h.scheduler.onceRegistrations[0].interval == 0.25, "debounce is 250 ms")
        expect(h.log.reports.count == 2, "nothing is reported before the debounce fires")

        h.scheduler.fireOnce()
        expect(h.log.reports == [reportAvailable(nil), reportAvailable(44), reportAvailable(45)],
               "the debounce reports 45 once")
    }

    private static func changesInsideDebounceCoalesceToLatestValue() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)

        h.backend.level = 45
        h.scheduler.fireRepeating()
        expect(h.scheduler.liveOnce == 1, "first change starts the debounce")

        h.backend.level = 47
        h.scheduler.fireRepeating()
        expect(h.log.reports.count == 2, "changes inside the debounce window are not reported")
        expect(h.scheduler.onceRegistrations.count == 2 && h.scheduler.onceRegistrations[0].canceled
               && h.scheduler.liveOnce == 1, "a second change restarts the debounce")

        h.scheduler.fireOnce()
        expect(h.log.reports.last == reportAvailable(47) && h.log.reports.count == 3,
               "exactly one report carries the latest value")
        let refired = h.scheduler.fireOnce()
        expect(refired == 0 && h.log.reports.count == 3, "the fired debounce does not report again")
    }

    private static func sameRoundedValueIsNotReported() {
        let h = Harness()
        h.backend.level = 40.2
        h.monitor.start()
        h.monitor.setEnabled(true)

        h.backend.level = 40.4
        h.scheduler.fireRepeating()
        expect(h.backend.reads == 2, "the tick still reads the level")
        expect(h.scheduler.onceRegistrations.isEmpty && h.log.reports.count == 2,
               "a change that rounds to the reported value schedules nothing and reports nothing")

        h.backend.level = 41.0
        h.scheduler.fireRepeating()
        expect(h.scheduler.liveOnce == 1 && h.log.reports.count == 2,
               "positive control: a one percent change schedules a debounce")
    }

    private static func failedReadKeepsLastReportedValue() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)

        h.backend.level = nil
        h.scheduler.fireRepeating()
        expect(h.backend.reads == 2 && h.log.reports.count == 2 && h.scheduler.onceRegistrations.isEmpty,
               "a nil read reports nothing and schedules nothing")

        h.backend.level = 40.3
        h.scheduler.fireRepeating()
        expect(h.backend.reads == 3 && h.log.reports.count == 2 && h.scheduler.onceRegistrations.isEmpty,
               "the value kept before the failure still matches a later read of the same level")

        h.backend.level = 42
        h.scheduler.fireRepeating()
        expect(h.scheduler.liveOnce == 1, "positive control: a real change after the failure schedules a debounce")
        h.scheduler.fireOnce()
        expect(h.log.reports.last == reportAvailable(42), "positive control: the debounce reports the new level")
    }

    private static func asleepTickSkipsRead() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)

        h.backend.asleep = true
        h.backend.level = 50
        h.scheduler.fireRepeating()
        expect(h.backend.reads == 1 && h.scheduler.onceRegistrations.isEmpty && h.log.reports.count == 2,
               "an asleep tick does not read the level")

        h.backend.asleep = false
        h.scheduler.fireRepeating()
        expect(h.backend.reads == 2 && h.scheduler.liveOnce == 1,
               "positive control: the awake tick reads and debounces the same change")
    }

    private static func calibratingCancelsTimerAndRestoresOnExit() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)
        h.backend.level = 45
        h.scheduler.fireRepeating()
        expect(h.scheduler.liveRepeating == 1 && h.scheduler.liveOnce == 1 && h.backend.reads == 2,
               "positive control: timer and debounce are live before calibration")

        h.monitor.setCalibrating(true)
        expect(h.scheduler.liveRepeating == 0 && h.scheduler.liveOnce == 0,
               "calibrating cancels the timer and the pending debounce")
        expect(h.log.reports.last == reportAvailable(40, calibrating: true) && h.log.reports.count == 3,
               "calibrating reports the last reported value at once")

        h.backend.level = 60
        let fired = h.scheduler.fireRepeating() + h.scheduler.fireOnce()
        expect(fired == 0 && h.backend.reads == 2, "ticks do nothing while calibrating")

        h.monitor.setCalibrating(false)
        expect(h.scheduler.liveRepeating == 1, "leaving calibration restarts the timer")
        expect(h.backend.reads == 3 && h.log.reports.last == reportAvailable(60) && h.log.reports.count == 4,
               "leaving calibration re-reads and reports at once")

        h.scheduler.fireRepeating()
        expect(h.backend.reads == 4, "positive control: the restarted timer ticks")
    }

    private static func availabilityLossAndRestoreTransitions() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)
        h.monitor.refreshAvailability()
        expect(h.log.reports.count == 2, "unchanged availability reports nothing")

        h.backend.statusValue = BuiltInDisplayStatus(available: false, reason: .brightnessNotAdjustable, displayID: 1)
        h.monitor.refreshAvailability()
        expect(h.log.reports.last == reportUnavailable(.brightnessNotAdjustable) && h.log.reports.count == 3,
               "losing availability reports the reason at once")
        expect(h.scheduler.liveRepeating == 0, "losing availability cancels the timer")
        let readsWhileUnavailable = h.backend.reads
        let fired = h.scheduler.fireRepeating() + h.scheduler.fireOnce()
        expect(fired == 0 && h.backend.reads == readsWhileUnavailable, "no tick runs while unavailable")

        h.backend.statusValue = BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)
        h.backend.level = 52
        h.monitor.refreshAvailability()
        expect(h.log.reports.last == reportAvailable(52) && h.log.reports.count == 4,
               "restoring availability reports the current level at once")
        expect(h.scheduler.liveRepeating == 1, "restoring availability restarts the timer")

        let readsBeforeTick = h.backend.reads
        h.scheduler.fireRepeating()
        expect(h.backend.reads == readsBeforeTick + 1, "positive control: the restarted timer ticks")
    }

    private static func disablingCancelsTimerAndDebounce() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)
        h.backend.level = 45
        h.scheduler.fireRepeating()
        expect(h.scheduler.liveRepeating == 1 && h.scheduler.liveOnce == 1,
               "positive control: timer and debounce are live before disabling")

        let readsBefore = h.backend.reads
        h.monitor.setEnabled(false)
        expect(h.scheduler.liveRepeating == 0 && h.scheduler.liveOnce == 0,
               "disabling cancels the timer and the pending debounce")

        h.backend.level = 70
        let fired = h.scheduler.fireRepeating() + h.scheduler.fireOnce()
        expect(fired == 0 && h.backend.reads == readsBefore, "no tick, debounce or read happens after disabling")

        h.monitor.setEnabled(true)
        expect(h.backend.reads == readsBefore + 1 && h.scheduler.liveRepeating == 1,
               "positive control: re-enabling reads the level and arms the timer")
    }

    private static func enablingWhileUnavailableWaitsForAvailability() {
        let h = Harness()
        h.backend.statusValue = BuiltInDisplayStatus(available: false, reason: .noBuiltInDisplay, displayID: nil)
        h.monitor.start()
        expect(h.log.reports == [reportUnavailable(.noBuiltInDisplay)], "start reports the unavailable reason")

        h.monitor.setEnabled(true)
        expect(h.scheduler.liveRepeating == 0 && h.backend.reads == 0,
               "enabling while unavailable arms no timer and reads nothing")
        let fired = h.scheduler.fireRepeating()
        expect(fired == 0 && h.backend.reads == 0, "no tick runs while unavailable")

        h.backend.statusValue = BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)
        h.backend.level = 40.2
        h.monitor.refreshAvailability()
        expect(h.log.reports.last == reportAvailable(40), "availability returning while enabled reports the level")
        expect(h.scheduler.liveRepeating == 1, "availability returning arms the timer")

        let readsBeforeTick = h.backend.reads
        h.scheduler.fireRepeating()
        expect(h.backend.reads == readsBeforeTick + 1, "positive control: the armed timer ticks after availability returns")
    }

    private static func stopCancelsEverything() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)
        h.backend.level = 45
        h.scheduler.fireRepeating()
        expect(h.scheduler.liveRepeating == 1 && h.scheduler.liveOnce == 1,
               "positive control: timer and debounce are live before stop")

        let readsBefore = h.backend.reads
        h.monitor.stop()
        expect(h.scheduler.liveRepeating == 0 && h.scheduler.liveOnce == 0,
               "stop cancels the timer and the pending debounce")

        let reportsAfterStop = h.log.reports.count
        h.backend.level = 70
        let fired = h.scheduler.fireRepeating() + h.scheduler.fireOnce()
        expect(fired == 0, "no timer or debounce runs after stop")
        expect(h.backend.reads == readsBefore && h.log.reports.count == reportsAfterStop,
               "stop leaves no read or report behind")
    }

    private static func tickDetectsAvailabilityLoss() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)
        expect(h.scheduler.liveRepeating == 1, "positive control: the timer is live before availability is lost")

        h.backend.statusValue = BuiltInDisplayStatus(available: false, reason: .brightnessNotAdjustable, displayID: 1)
        let readsBefore = h.backend.reads
        let fired = h.scheduler.fireRepeating()
        expect(fired == 1, "positive control: the timer ticks without refreshAvailability")
        expect(h.log.reports.last == reportUnavailable(.brightnessNotAdjustable) && h.log.reports.count == 3,
               "a tick that sees availability loss reports it at once")
        expect(h.scheduler.liveRepeating == 0 && h.backend.reads == readsBefore,
               "a tick that sees availability loss cancels the timer and reads nothing")

        let later = h.scheduler.fireRepeating() + h.scheduler.fireOnce()
        expect(later == 0 && h.backend.reads == readsBefore, "no tick runs after the tick saw availability loss")
    }

    private static func deliveryDropsSupersededReports() {
        let h = Harness()
        // A newer report arrives first; the older one arriving afterwards must be dropped.
        h.monitor.deliver(reportAvailable(45, calibrating: true), seq: 2)
        h.monitor.deliver(reportAvailable(44), seq: 1)
        expect(h.log.reports == [reportAvailable(45, calibrating: true)],
               "a stale report arriving after a newer one is dropped")

        h.monitor.deliver(reportAvailable(45, calibrating: true), seq: 2)
        expect(h.log.reports.count == 1, "a repeated sequence number is not delivered again")

        h.monitor.deliver(reportAvailable(46), seq: 3)
        expect(h.log.reports == [reportAvailable(45, calibrating: true), reportAvailable(46)],
               "positive control: a higher sequence number is still delivered")
    }

    private static func offlineReasonClassifiesLidAndSleep() {
        // A closed lid takes precedence over the built-in display lookup.
        expect(DisplayServicesBackend.offlineReason(builtInFound: false, lidClosed: true,
                                                    online: false, active: false, asleep: false) == .displayOffline,
               "a closed lid with no built-in display is offline, not missing")
        expect(DisplayServicesBackend.offlineReason(builtInFound: false, lidClosed: false,
                                                    online: false, active: false, asleep: false) == .noBuiltInDisplay,
               "an open lid with no built-in display reports the missing display")
        expect(DisplayServicesBackend.offlineReason(builtInFound: true, lidClosed: false,
                                                    online: true, active: true, asleep: true) == .displayOffline,
               "an asleep built-in display is offline")
        expect(DisplayServicesBackend.offlineReason(builtInFound: true, lidClosed: false,
                                                    online: true, active: false, asleep: false) == .displayOffline,
               "an inactive built-in display is offline")
        expect(DisplayServicesBackend.offlineReason(builtInFound: true, lidClosed: false,
                                                    online: true, active: true, asleep: false) == nil,
               "an online, active, awake built-in display has no offline reason")
        expect(DisplayServicesBackend.offlineReason(builtInFound: true, lidClosed: true,
                                                    online: true, active: true, asleep: false) == .displayOffline,
               "a closed lid is offline even when everything else is healthy")
    }

    private static func pausesWhileDisplayIsOffAndResumesWithFreshLevel() {
        let h = Harness()
        h.backend.level = 40
        h.monitor.start()
        h.monitor.setEnabled(true)
        expect(h.scheduler.liveRepeating == 1 && h.log.reports.last == reportAvailable(40),
               "positive control: enabling arms the timer and reports 40")

        h.backend.statusValue = BuiltInDisplayStatus(available: false, reason: .displayOffline, displayID: 1)
        let readsBeforeOffline = h.backend.reads
        h.monitor.refreshAvailability()
        expect(h.log.reports.last == reportUnavailable(.displayOffline) && h.log.reports.count == 3,
               "a display going offline reports the reason at once")
        expect(h.scheduler.liveRepeating == 0 && h.backend.reads == readsBeforeOffline,
               "a display going offline cancels the timer and reads nothing")

        h.monitor.refreshAvailability()
        expect(h.log.reports.count == 3, "unchanged offline status reports nothing")

        h.backend.statusValue = BuiltInDisplayStatus(available: true, reason: nil, displayID: 1)
        h.backend.level = 60
        h.monitor.refreshAvailability()
        expect(h.log.reports.last == reportAvailable(60) && h.log.reports.count == 4,
               "a display coming back reports the fresh level at once")
        expect(h.scheduler.liveRepeating == 1, "a display coming back restarts the timer")

        h.monitor.refreshAvailability()
        expect(h.log.reports.count == 4, "unchanged availability after return reports nothing")
    }
}
