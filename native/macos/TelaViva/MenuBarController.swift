import AppKit
import ApplicationServices
import SwiftUI
import Combine
import IOKit.ps

final class MenuBarController: NSObject, NSApplicationDelegate {
    let core = CoreBridge()
    private lazy var manualControls = ManualMonitorControls(submit: core.submitManualControl)
    private let camera = PresenceCamera()
    private lazy var ddc = MacDDCController(core: core)
    private lazy var hardwareKeyRouter = HardwareKeyRouter(submit: { [weak self] command, completion in
        guard let self else { completion(false); return }
        self.ddc.submitHardwareKey(command) { [weak self] accepted in
            completion(accepted)
            guard !accepted else { return }
            DispatchQueue.main.async { [weak self] in self?.rejectExternalKeyboardIntent(command) }
        }
    })
    private let defaultAudioOutput = DefaultAudioOutputMonitor()
    private var hardwareKeyDisplays: [HardwareKeyDisplay] = []
    private var externalKeyboardDrafts: [UInt64: (monitorID: String, generation: UInt64, feature: HardwareKeyFeature, revision: UInt64)] = [:]
    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private var settingsWindow: NSWindow?
    private var powerSource: CFRunLoopSource?
    private var snapshotSubscription: AnyCancellable?
    private var terminationReply: ((Bool) -> Void)?
    private var terminationDeadline: DispatchWorkItem?
    private var terminationPoll: DispatchWorkItem?
    private var terminating = false
    private var activationObserver: NSObjectProtocol?
    private var accessibilityObserver: NSObjectProtocol?

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
        let popupView = MonitorPopup(core: core, controls: manualControls, openSettings: openSettingsAction,
                                     refreshDisplays: refreshDisplaysAction)
        popover.contentViewController = NSHostingController(rootView: popupView)
        snapshotSubscription = core.$snapshot.receive(on: DispatchQueue.main).sink { [weak self] snapshot in
            guard let self else { return }
            self.pruneExternalKeyboardDrafts(using: snapshot)
            self.camera.apply(snapshot, core: self.core)
            self.manualControls.apply(snapshot)
            self.ddc.process(snapshot)
            guard !self.terminating else { return }
            self.hardwareKeyRouter.apply(enabled: snapshot.settings?.hardware_keys_enabled ?? false,
                                         displays: self.hardwareKeyDisplays)
            if snapshot.settings?.hardware_keys_enabled ?? false {
                self.defaultAudioOutput.start()
            } else {
                self.defaultAudioOutput.stop()
            }
        }
        core.hardwareKeyValidator = { [weak self] command in
            guard let self, !self.terminating, AXIsProcessTrusted() else { return false }
            return self.hardwareKeyRouter.isCommandCurrent(command)
        }
        manualControls.onIntent = { [weak self] monitorID, generation, feature, value, revision in
            guard let self else { return }
            self.externalKeyboardDrafts = self.externalKeyboardDrafts.filter {
                $0.value.monitorID != monitorID || $0.value.generation != generation || $0.value.feature != feature
            }
            self.hardwareKeyRouter.manualControlChanged(monitorID: monitorID, generation: generation,
                                                        feature: feature, value: value, revision: revision)
        }
        manualControls.onAccepted = { [weak self] monitorID, generation, feature, value, revision, accepted in
            guard accepted else { return }
            self?.hardwareKeyRouter.manualControlAccepted(monitorID: monitorID, generation: generation,
                                                          feature: feature, value: value, revision: revision)
        }
        core.onHardwareActionCompleted = { [weak manualControls] monitorID, actionID, actionGeneration, connectionGeneration, feature, success, rawValue in
            manualControls?.actionCompleted(monitorID: monitorID, actionID: actionID,
                                             actionGeneration: actionGeneration,
                                             connectionGeneration: connectionGeneration, feature: feature,
                                             success: success, confirmedValue: rawValue)
        }
        core.onHardwareKeyAccepted = { [weak self] command, accepted, actionGeneration in
            guard let self else { return }
            let keyRevision = command.intentRevision
            guard let draft = self.externalKeyboardDrafts.removeValue(forKey: keyRevision),
                  self.hardwareKeyRouter.isCommandCurrent(command) else { return }
            if accepted, let actionGeneration {
                self.manualControls.bindExternalReceipt(monitorID: draft.monitorID, generation: draft.generation,
                                                        feature: draft.feature, revision: draft.revision,
                                                        actionGeneration: actionGeneration)
            } else {
                self.manualControls.rejectExternalIntent(monitorID: draft.monitorID, generation: draft.generation,
                                                         feature: draft.feature, revision: draft.revision)
            }
        }
        // Feed the current sound-output UID into the router; see DefaultAudioOutputMonitor.
        defaultAudioOutput.onChange = { [weak self] uid in self?.hardwareKeyRouter.setDefaultOutput(uid: uid) }
        hardwareKeyRouter.onReservedTarget = { [weak self] command in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.hardwareKeyRouter.isCommandCurrent(command) else { return }
                guard let revision = self.manualControls.adoptExternalIntent(
                    monitorID: command.monitorID, generation: command.generation,
                    feature: command.feature, value: command.target, sourceToken: command.intentRevision
                ) else { return }
                self.externalKeyboardDrafts = self.externalKeyboardDrafts.filter {
                    $0.value.monitorID != command.monitorID || $0.value.generation != command.generation ||
                        $0.value.feature != command.feature
                }
                guard self.externalKeyboardDrafts.count < 128 else {
                    self.manualControls.rejectExternalIntent(monitorID: command.monitorID, generation: command.generation,
                                                             feature: command.feature, revision: revision)
                    return
                }
                self.externalKeyboardDrafts[command.intentRevision] = (command.monitorID, command.generation,
                                                                         command.feature, revision)
            }
        }
        hardwareKeyRouter.onStatusChange = { [weak core = self.core] status in
            let key: String
            switch status {
            case .disabled: key = "hardwareKeys.status.disabled"
            case .enabled: key = "hardwareKeys.status.enabled"
            case .permissionRequired: key = "hardwareKeys.status.permissionRequired"
            case .permissionRevoked: key = "hardwareKeys.status.permissionRevoked"
            case .listenerUnavailable: key = "hardwareKeys.status.listenerUnavailable"
            case .inputMonitoringRequired: key = "hardwareKeys.status.inputMonitoringRequired"
            }
            core?.hardwareKeyStatusKey = key
        }
        hardwareKeyRouter.onPermissionsChange = { [weak core = self.core] permissions in core?.hardwareKeyPermissions = permissions }
        // Permissions are re-read on activation and after the Accessibility trust state changes; no polling.
        activationObserver = NotificationCenter.default.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.hardwareKeyRouter.refreshPermissions()
        }
        // The trust state lags this notification, so the re-check is delayed once.
        accessibilityObserver = DistributedNotificationCenter.default().addObserver(forName: NSNotification.Name("com.apple.accessibility.api"), object: nil, queue: .main) { [weak self] _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self?.hardwareKeyRouter.refreshPermissions() }
        }
        ddc.onHardwareKeyDisplays = { [weak self] displays in
            guard let self else { return }
            guard !self.terminating else { return }
            self.hardwareKeyDisplays = displays
            self.hardwareKeyRouter.apply(enabled: self.core.snapshot.settings?.hardware_keys_enabled ?? false,
                                         displays: displays)
        }
        ddc.start()
        observeScreensaver()
        powerSource = IOPSNotificationCreateRunLoopSource(powerSourceChanged, Unmanaged.passUnretained(self).toOpaque())?.takeRetainedValue()
        if let powerSource { CFRunLoopAddSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        reportPower()
    }

    func requestHardwareKeyPermission() {
        hardwareKeyRouter.requestPermissionFromUserAction()
        hardwareKeyRouter.apply(enabled: core.snapshot.settings?.hardware_keys_enabled ?? false,
                                 displays: hardwareKeyDisplays)
    }

    func requestAccessibilityPermission() { hardwareKeyRouter.requestAccessibilityFromUserAction() }

    func requestInputMonitoringPermission() { hardwareKeyRouter.requestInputMonitoringFromUserAction() }

    /// Removes the permission observers; safe to call more than once.
    private func removePermissionObservers() {
        if let activationObserver { NotificationCenter.default.removeObserver(activationObserver) }
        if let accessibilityObserver { DistributedNotificationCenter.default().removeObserver(accessibilityObserver) }
        activationObserver = nil
        accessibilityObserver = nil
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        hardwareKeyRouter.apply(enabled: core.snapshot.settings?.hardware_keys_enabled ?? false,
                                displays: hardwareKeyDisplays)
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
            let settingsView = SettingsView(core: core, requestHardwareKeyPermission: { [weak self] in
                self?.requestHardwareKeyPermission()
            })
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
        hardwareKeyRouter.refreshPermissions()
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
            self.defaultAudioOutput.stop()
            self.hardwareKeyRouter.shutdown()
            self.removePermissionObservers()
            self.ddc.shutdown()
            self.terminationReply = nil
            NSApp.reply(toApplicationShouldTerminate: allow)
        }
        if let powerSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        defaultAudioOutput.stop()
        hardwareKeyRouter.shutdown()
        removePermissionObservers()
        manualControls.shutdown()
        camera.shutdown()
        let deadline = DispatchWorkItem { [weak self] in
            NSLog("TelaViva: shutdown restoration wait expired; terminating with any remaining hardware actions pending")
            self?.terminationReply?(true)
        }
        terminationDeadline = deadline
        DispatchQueue.main.asyncAfter(deadline: .now() + 8, execute: deadline)
        core.beginShutdown { [weak self] in self?.pollShutdownState() }
        return .terminateLater
    }

    private func pollShutdownState() {
        guard terminating, terminationReply != nil else { return }
        core.refresh()
        if core.snapshot.pending_actions?.isEmpty != false {
            terminationReply?(true)
            return
        }
        let work = DispatchWorkItem { [weak self] in self?.pollShutdownState() }
        terminationPoll = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: work)
    }

    private func rejectExternalKeyboardIntent(_ command: HardwareKeyCommand) {
        guard let draft = externalKeyboardDrafts.removeValue(forKey: command.intentRevision) else { return }
        manualControls.rejectExternalIntent(monitorID: draft.monitorID, generation: draft.generation,
                                            feature: draft.feature, revision: draft.revision)
    }

    private func pruneExternalKeyboardDrafts(using snapshot: CoreSnapshot) {
        externalKeyboardDrafts = externalKeyboardDrafts.filter { _, draft in
            guard let monitor = snapshot.monitors?.first(where: { $0.id == draft.monitorID }),
                  monitor.generation == draft.generation,
                  monitor.display_state != "asleep", monitor.identity_status == "stable" else { return false }
            let value = draft.feature == .brightness ? monitor.brightness : monitor.volume
            return value?.supported == true
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let powerSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        camera.shutdown()
        defaultAudioOutput.stop()
        hardwareKeyRouter.shutdown()
        removePermissionObservers()
        ddc.shutdown()
    }
}

private let powerSourceChanged: IOPowerSourceCallbackType = { context in
    guard let context else { return }
    Unmanaged<MenuBarController>.fromOpaque(context).takeUnretainedValue().reportPower()
}
