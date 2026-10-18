import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("ManualMonitorControlsTests: \(message)") }
}

private let idleDraft = ControlsDraft(hasDraft: false, draft: 0, status: .ready)

/// Records slider calls and answers queries from scripted drafts, standing in for the core's controls machine.
private final class FakeDriver: ManualControlsDriving {
    private struct Key: Hashable {
        let monitorID: String
        let feature: HardwareKeyFeature
    }
    private var drafts: [Key: ControlsDraft] = [:]
    private(set) var sliderCalls: [SliderCall] = []
    private(set) var queryCount = 0

    func script(_ draft: ControlsDraft, monitorID: String = "monitor-a", feature: HardwareKeyFeature = .brightness) {
        drafts[Key(monitorID: monitorID, feature: feature)] = draft
    }

    @discardableResult
    func slider(monitorID: String, feature: HardwareKeyFeature, value: Double) -> ControlsDraft? {
        sliderCalls.append(SliderCall(monitorID: monitorID, feature: feature, value: value))
        return drafts[Key(monitorID: monitorID, feature: feature)]
    }

    func queryDraft(monitorID: String, feature: HardwareKeyFeature) -> ControlsDraft {
        queryCount += 1
        return drafts[Key(monitorID: monitorID, feature: feature)] ?? idleDraft
    }
}

private struct SliderCall: Equatable {
    let monitorID: String
    let feature: HardwareKeyFeature
    let value: Double
}

@main
private struct ManualMonitorControlsTests {
    static func main() {
        idleControlHasNoDraft()
        sliderForwardsClampedValue()
        invalidInputIsIgnored()
        draftStatusMapsToControlStatus()
        draftValueReplacesDisplayValue()
        draftChangedRepaintsFromBackgroundQueue()
        protectionBrightnessMaximumFollowsPendingDrafts()
        shutdownStopsForwarding()
        print("Manual monitor control tests passed")
    }

    private static func idleControlHasNoDraft() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display()
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == nil,
               "idle control has no draft, so the view shows the confirmed value")
        expect(controls.status(monitor: monitor, feature: .brightness) == .ready, "idle control reports ready")
        expect(driver.sliderCalls.isEmpty, "idle control sends nothing")
    }

    private static func sliderForwardsClampedValue() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display(id: "monitor-b")
        let before = controls.changeToken
        controls.setValue(62, monitor: monitor, feature: .brightness)
        expect(driver.sliderCalls == [SliderCall(monitorID: "monitor-b", feature: .brightness, value: 62)],
               "slider forwards monitor id, feature and value")
        expect(controls.changeToken == before &+ 1, "slider bumps the repaint token")
        controls.setValue(140, monitor: monitor, feature: .volume)
        controls.setValue(-5, monitor: monitor, feature: .volume)
        expect(driver.sliderCalls.map { $0.value } == [62, 100, 0], "values are clamped to 0...100")
        expect(driver.sliderCalls[1].feature == .volume, "volume slider is forwarded as volume")
        controls.setEditing(true, monitor: monitor, feature: .brightness)
        expect(driver.sliderCalls.count == 3, "editing state does not reach the driver")
        expect(controls.changeToken == before &+ 4, "editing state only requests a repaint")
    }

    private static func invalidInputIsIgnored() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display()
        controls.setValue(Double.nan, monitor: monitor, feature: .brightness)
        controls.setValue(Double.infinity, monitor: monitor, feature: .brightness)
        expect(driver.sliderCalls.isEmpty, "non-finite values never reach the driver")

        let unmanaged = display(generation: nil)
        let before = controls.changeToken
        controls.setValue(50, monitor: unmanaged, feature: .brightness)
        expect(driver.sliderCalls.isEmpty, "a monitor without generation is ignored")
        expect(controls.changeToken == before, "ignored input does not repaint")
        expect(controls.displayValue(monitor: unmanaged, feature: .brightness) == nil,
               "a monitor without generation has no draft")
        expect(controls.status(monitor: unmanaged, feature: .brightness) == .ready,
               "a monitor without generation reports ready")
        expect(driver.queryCount == 0, "a monitor without generation is never queried")
    }

    private static func draftStatusMapsToControlStatus() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display()
        driver.script(controlsDraft(30, .pending))
        expect(controls.status(monitor: monitor, feature: .brightness) == .pending, "pending draft maps to pending")
        driver.script(controlsDraft(30, .failed))
        expect(controls.status(monitor: monitor, feature: .brightness) == .failed, "failed draft maps to failed")
        driver.script(controlsDraft(30, .ready))
        expect(controls.status(monitor: monitor, feature: .brightness) == .ready, "ready draft maps to ready")
    }

    private static func draftValueReplacesDisplayValue() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display()
        driver.script(controlsDraft(63, .pending))
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == 63, "the core draft is shown while it exists")
        driver.script(controlsDraft(12, .ready, hasDraft: false))
        expect(controls.displayValue(monitor: monitor, feature: .brightness) == nil,
               "without a draft the view falls back to the confirmed value")
    }

    private static func draftChangedRepaintsFromBackgroundQueue() {
        let controls = ManualMonitorControls(driver: FakeDriver())
        let before = controls.changeToken
        DispatchQueue.global(qos: .userInitiated).async { controls.draftChanged() }
        expect(waitUntil { controls.changeToken == before &+ 1 }, "background draft change repaints on the main queue")
    }

    private static func protectionBrightnessMaximumFollowsPendingDrafts() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display()
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 41, "no draft keeps the confirmed cap")
        driver.script(controlsDraft(20, .pending))
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 20, "pending decrease lowers the cap at once")
        driver.script(controlsDraft(60, .pending))
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 41,
               "pending increase does not widen the cap before confirmation")
        driver.script(controlsDraft(20, .failed))
        expect(controls.protectionBrightnessMaximum(monitor: monitor) == 41, "failed draft does not lower the cap")
        let unsupported = display(brightness: VcpValue(supported: false, value: nil, raw: nil, min: nil, max: nil,
                                                       desired: nil, status: "unsupported"))
        expect(controls.protectionBrightnessMaximum(monitor: unsupported) == nil, "unsupported brightness has no cap")
    }

    private static func shutdownStopsForwarding() {
        let driver = FakeDriver()
        let controls = ManualMonitorControls(driver: driver)
        let monitor = display()
        let before = controls.changeToken
        controls.shutdown()
        expect(controls.changeToken == before &+ 1, "shutdown bumps the repaint token")
        controls.setValue(40, monitor: monitor, feature: .brightness)
        expect(driver.sliderCalls.isEmpty, "setValue after shutdown does not reach the driver")
    }

    private static func controlsDraft(_ value: Double, _ status: ControlsDraftStatus, hasDraft: Bool = true) -> ControlsDraft {
        ControlsDraft(hasDraft: hasDraft, draft: value, status: status)
    }

    private static func display(id: String = "monitor-a", generation: UInt64? = 1,
                                brightness: VcpValue? = VcpValue(supported: true, value: 41, raw: 41, min: 0, max: 100,
                                                                 desired: nil, status: "ready")) -> CoreMonitor {
        CoreMonitor(id: id, name: id, presence_dimmed: false, identity_status: "stable", connection_path: nil,
                    generation: generation, display_state: "awake", brightness: brightness,
                    volume: VcpValue(supported: true, value: 50, raw: 50, min: 0, max: 100, desired: nil, status: "ready"))
    }

    private static func waitUntil(timeout: TimeInterval = 3, _ predicate: () -> Bool) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while !predicate() && Date() < deadline {
            _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.01))
        }
        return predicate()
    }
}
