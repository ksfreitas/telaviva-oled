import AppKit
import SwiftUI
import Combine
import IOKit.ps

final class MenuBarController: NSObject, NSApplicationDelegate {
    let core = CoreBridge()
    private let camera = PresenceCamera()
    private lazy var ddc = MacDDCController(core: core)
    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private var settingsWindow: NSWindow?
    private var powerSource: CFRunLoopSource?
    private var snapshotSubscription: AnyCancellable?
    private var terminationReply: ((Bool) -> Void)?
    private var terminationDeadline: DispatchWorkItem?
    private var terminationPoll: DispatchWorkItem?
    private var terminating = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.image = NSImage(systemSymbolName: "display", accessibilityDescription: "TelaViva OLED")
        statusItem.button?.target = self
        statusItem.button?.action = #selector(togglePopover(_:))
        statusItem.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])
        popover = NSPopover()
        popover.behavior = .transient
        popover.contentSize = NSSize(width: 360, height: 420)
        let openSettingsAction: () -> Void = openSettings
        let refreshDisplaysAction: () -> Void = ddc.refresh
        let popupView = MonitorPopup(core: core, openSettings: openSettingsAction,
                                     refreshDisplays: refreshDisplaysAction)
        popover.contentViewController = NSHostingController(rootView: popupView)
        snapshotSubscription = core.$snapshot.receive(on: DispatchQueue.main).sink { [weak self] snapshot in
            guard let self else { return }
            self.camera.apply(snapshot, core: self.core)
            self.ddc.process(snapshot)
        }
        ddc.start()
        observeScreensaver()
        powerSource = IOPSNotificationCreateRunLoopSource(powerSourceChanged, Unmanaged.passUnretained(self).toOpaque())?.takeRetainedValue()
        if let powerSource { CFRunLoopAddSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        reportPower()
    }

    @objc private func togglePopover(_ sender: Any?) {
        guard let button = statusItem.button else { return }
        if popover.isShown { popover.performClose(sender) }
        else { core.refresh(); popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY); NSApp.activate(ignoringOtherApps: true) }
    }

    private func openSettings() {
        let invokingScreen = popover.contentViewController?.view.window?.screen
            ?? statusItem.button?.window?.screen
            ?? settingsWindow?.screen
            ?? NSScreen.main
        popover.performClose(nil)
        let window: NSWindow
        if let settingsWindow {
            window = settingsWindow
        } else {
            let settingsView = SettingsView(core: core)
                .frame(minWidth: 420, minHeight: 420)
            let created = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 560),
                                   styleMask: [.titled, .closable, .miniaturizable],
                                   backing: .buffered, defer: false)
            created.title = "TelaViva OLED"
            created.contentViewController = NSHostingController(rootView: settingsView)
            created.isReleasedWhenClosed = false
            settingsWindow = created
            window = created
        }
        if let visibleFrame = invokingScreen?.visibleFrame {
            let frame = window.frame
            window.setFrameOrigin(NSPoint(
                x: visibleFrame.midX - frame.width / 2,
                y: visibleFrame.midY - frame.height / 2
            ))
        }
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func observeScreensaver() {
        let center = DistributedNotificationCenter.default()
        center.addObserver(forName: Notification.Name("com.apple.screensaver.didstart"), object: nil, queue: .main) { [weak self] _ in self?.core.submit(["type": "screensaver", "state": "active"]) }
        center.addObserver(forName: Notification.Name("com.apple.screensaver.didstop"), object: nil, queue: .main) { [weak self] _ in self?.core.submit(["type": "screensaver", "state": "inactive"]) }
        core.submit(["type": "screensaver", "state": "unknown"])
    }

    fileprivate func reportPower() {
        let sources = IOPSCopyPowerSourcesInfo().takeRetainedValue()
        guard let powerType = IOPSGetProvidingPowerSourceType(sources) else {
            core.submit(["type": "power", "on_battery": NSNull()]); return
        }
        let onBattery = (powerType.takeUnretainedValue() as String) == kIOPSBatteryPowerValue
        core.submit(["type": "power", "on_battery": onBattery])
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard !terminating else { return .terminateLater }
        terminating = true
        terminationReply = { [weak self] allow in
            guard let self else { return }
            self.terminationPoll?.cancel(); self.terminationDeadline?.cancel()
            self.ddc.shutdown()
            self.terminationReply = nil
            NSApp.reply(toApplicationShouldTerminate: allow)
        }
        if let powerSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        camera.shutdown()
        core.beginShutdown()
        pollShutdownState()
        let deadline = DispatchWorkItem { [weak self] in
            NSLog("TelaViva: shutdown restoration wait expired; terminating with any remaining hardware actions pending")
            self?.terminationReply?(true)
        }
        terminationDeadline = deadline
        DispatchQueue.main.asyncAfter(deadline: .now() + 8, execute: deadline)
        return .terminateLater
    }

    private func pollShutdownState() {
        guard terminating else { return }
        core.refresh()
        if core.snapshot.pending_actions?.isEmpty != false {
            terminationReply?(true)
            return
        }
        let work = DispatchWorkItem { [weak self] in self?.pollShutdownState() }
        terminationPoll = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: work)
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let powerSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        camera.shutdown()
        ddc.shutdown()
    }
}

private let powerSourceChanged: IOPowerSourceCallbackType = { context in
    guard let context else { return }
    Unmanaged<MenuBarController>.fromOpaque(context).takeUnretainedValue().reportPower()
}
