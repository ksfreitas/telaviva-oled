import AppKit
import ApplicationServices
import SwiftUI
import Combine
import IOKit.ps

final class MenuBarController: NSObject, NSApplicationDelegate {
    let core = CoreBridge()
    /// One navigation and one update checker for the whole app: the menus and the Settings scene share them.
    let settingsNavigation = SettingsNavigation()
    let installedVersion = (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "unknown"
    @MainActor lazy var updateChecker: UpdateChecker = UpdateChecker(installedVersion: installedVersion,
                                                                     fetcher: URLSessionUpdateFetcher(),
                                                                     evaluate: MenuBarController.updateEvaluator)
    /// The Rust controls core: manual sliders, hardware keys and the DDC attempts it asks for.
    private lazy var controls = ControlsDriver(stepper: ControlsBridge(), submit: { [weak self] json in
        self?.core.submitJSON(json) ?? false
    }, runAttempt: { [weak self] request, done in
        self?.ddc.runAttempt(request, completion: done)
    })
    private lazy var manualControls = ManualMonitorControls(driver: controls)
    let camera = PresenceCamera()
    private let keepAwakeBackend = MacKeepAwakeBackend()
    private lazy var keepAwakeController = KeepAwakeController(backend: keepAwakeBackend)
    /// Keeps the camera off the built-in camera while the lid is closed.
    private lazy var lidMonitor = LidStateMonitor { [weak self] closed in
        guard let self else { return }
        self.camera.setLidClosed(closed)
        self.keepAwakeBackend.setLidClosed(closed)
        self.reportKeepAwakeSession()
    }
    private lazy var ddc = MacDDCController(core: core)
    private lazy var hardwareKeyRouter = HardwareKeyRouter(controls: controls)
    private let defaultAudioOutput = DefaultAudioOutputMonitor()
    private let ambientAvailability = AmbientAvailability()
    private let builtInBackend = DisplayServicesBackend()
    /// Application Support folder shared by the calibration session and the built-in protection record.
    private let appSupportDirectory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("TelaVivaOLED", isDirectory: true)
    /// One owner of the system's automatic brightness, so calibration and protection never each read and restore it.
    private lazy var autoBrightnessOwner = BuiltInAutoBrightnessOwner(backend: builtInBackend, store: AutoBrightnessHoldStore(directory: appSupportDirectory))
    /// Guards the cached report and the holding flag: the monitor reports from its queue, holding changes on main.
    private let builtInLock = NSLock()
    private var lastBuiltInReport: BuiltInDisplayReport?
    private var protectionHolding = false
    private lazy var builtInMonitor = BuiltInBrightnessMonitor(backend: builtInBackend, scheduler: DispatchBuiltInScheduler(), submit: { [weak self] report in
        guard let self else { return }
        self.builtInLock.lock()
        self.lastBuiltInReport = report
        self.builtInLock.unlock()
        self.publishBuiltIn()
        DispatchQueue.main.async { [weak self] in
            self?.ambientAvailability.reason = report.available ? nil : report.reason
            if !report.available { self?.calibration.end(.builtInUnavailable) }
        }
    })
    private lazy var protection = BuiltInProtectionController(backend: builtInBackend, scheduler: DispatchBuiltInScheduler(),
                                                              store: BuiltInProtectionStore(directory: appSupportDirectory),
                                                              holder: autoBrightnessOwner.handle(for: .protection),
                                                              reportHolding: { [weak self] holding in self?.setProtectionHolding(holding) })
    /// The card's and Settings' built-in hooks. Called only on main, the queue that owns the backend.
    /// Internal, not private: the Settings scene in App.swift reads it.
    lazy var builtInControls = BuiltInControls(protection: protection,
                                                       readLevel: { [weak self] in self?.builtInBackend.readLevel() },
                                                       writeLevel: { [weak self] in self?.builtInBackend.writeLevel($0) ?? false })
    private lazy var calibration: AmbientCalibrationController = {
        let hooks = CalibrationHooks(
            monitorBrightness: { [weak self] id in
                guard let value = self?.core.snapshot.monitors?.first(where: { $0.id == id })?.brightness, value.supported else { return nil }
                return value.value
            },
            setMonitorBrightness: { [weak self] id, value in
                guard let self, let monitor = self.core.snapshot.monitors?.first(where: { $0.id == id }) else { return }
                self.manualControls.setValue(value, monitor: monitor, feature: .brightness)
            },
            curve: { [weak self] id in self?.core.snapshot.settings?.ambientCurves?[id] ?? AmbientCurve() },
            saveCurve: { [weak self] id, curve in
                guard let self, var settings = self.core.snapshot.settings else { return }
                var curves = settings.ambientCurves ?? [:]
                curves[id] = curve
                settings.ambientCurves = curves
                self.core.updateSettings(settings)
            },
            setCalibrating: { [weak self] flag in self?.builtInMonitor.setCalibrating(flag) },
            now: { ISO8601DateFormatter().string(from: Date()) },
            // Calibration is refused while protection holds the built-in screen (architecture Decision 9).
            isProtectionHolding: { [weak self] in self?.protection.isHolding ?? false },
            // Gives up a monitor restore that is still pending after launch (see AmbientCalibrationController).
            after: { delay, block in DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: block) }
        )
        return AmbientCalibrationController(backend: builtInBackend, store: CalibrationSessionStore(directory: appSupportDirectory),
                                            autoBrightness: autoBrightnessOwner.handle(for: .calibration), hooks: hooks)
    }()
    private var hardwareKeyDisplays: [HardwareKeyDisplay] = []
    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private var settingsWindow: NSWindow?
    private var powerSource: CFRunLoopSource?
    private var snapshotSubscription: AnyCancellable?
    private var terminationReply: ((Bool) -> Void)?
    private var terminationDeadline: DispatchWorkItem?
    private var terminationPoll: DispatchWorkItem?
    private var terminating = false
    private var lastFollowedIDs: Set<String> = []
    private var lastInferenceDevice: String?
    private var activationObserver: NSObjectProtocol?
    private var accessibilityObserver: NSObjectProtocol?
    private var screenObserver: NSObjectProtocol?
    private var wakeObserver: NSObjectProtocol?
    private var screensWakeObserver: NSObjectProtocol?
    private var displaySleepObserver: NSObjectProtocol?
    private var systemSleepObserver: NSObjectProtocol?
    private var sessionResignObserver: NSObjectProtocol?
    private var sessionBecomeObserver: NSObjectProtocol?
    private var lastKeepAwakeDiagnostic: String?
    private var lockedObserver: NSObjectProtocol?
    private var unlockedObserver: NSObjectProtocol?
    private var saverStartObserver: NSObjectProtocol?
    private var saverStopObserver: NSObjectProtocol?
    private var settingsCloseObserver: NSObjectProtocol?
    /// Last state and locale applied to the status item; the icon is only reset when one of them changes.
    private var appliedIconState: PresenceIconState?
    private var appliedIconLocale: String?
    /// Evaluates the update response in the Rust core; without the core no result is available.
    private static let updateEvaluator: (String, Int, Data) -> UpdateOutcome? = { installed, status, body in
        #if TELAVIVA_CORE
        return CoreUpdate.evaluate(installed: installed, status: status, body: body)
        #else
        return nil
        #endif
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        updateStatusItemIcon(presence: nil, locale: nil)
        statusItem.button?.target = self
        statusItem.button?.action = #selector(togglePopover(_:))
        statusItem.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])
        keepAwakeController.onStatusChange = { [weak core = self.core] status in
            DispatchQueue.main.async {
                let key: String
                switch status {
                case .disabled: key = "keepAwake.status.disabled"
                case .awaitingPresence: key = "keepAwake.status.awaiting"
                case .active: key = "keepAwake.status.active"
                case .permissionRequired: key = "keepAwake.status.permission"
                case .unavailable: key = "keepAwake.status.unavailable"
                case .suspended: key = "keepAwake.status.suspended"
                }
                core?.keepAwakeStatusKey = key
            }
        }
        keepAwakeBackend.setLidClosed(DisplayServicesBackend.lidClosed())
        keepAwakeBackend.setDisplayAsleep(Self.currentDisplaysAreAsleep())
        reconcileKeepAwakeSession()
        popover = NSPopover()
        popover.behavior = .transient
        let openSettingsAction: () -> Void = { [weak self] in self?.openSettings() }
        let refreshDisplaysAction: () -> Void = ddc.refresh
        let popupView = MonitorPopup(core: core, controls: manualControls, openSettings: openSettingsAction,
                                     refreshDisplays: refreshDisplaysAction, builtIn: builtInControls)
        // The popover now follows the SwiftUI content size; a hardware check of the resize is pending.
        let host = NSHostingController(rootView: popupView)
        host.sizingOptions = [.preferredContentSize]
        popover.contentViewController = host
        snapshotSubscription = core.$snapshot.receive(on: DispatchQueue.main).sink { [weak self] snapshot in
            guard let self else { return }
            self.updateStatusItemIcon(presence: snapshot.camera?.presence, locale: snapshot.settings?.locale)
            let inferenceDevice = snapshot.settings?.inference_device ?? "auto"
            if inferenceDevice != self.lastInferenceDevice {
                self.lastInferenceDevice = inferenceDevice
                self.camera.setInferenceDevice(inferenceDevice)
            }
            self.camera.apply(snapshot, core: self.core)
            if snapshot.settings?.keep_awake_with_presence == true,
               snapshot.camera?.mode == "suspended", snapshot.camera?.display_status == "suspended" {
                let localSafety = self.keepAwakeBackend.safetyState()
                let localMonitoringAllowed = KeepAwakeSafety.cameraMonitoringAllowed(for: localSafety)
                if let active = KeepAwakeSessionReconciliation.sessionValueToResubmit(
                    keepAwakeEnabled: true, cameraMode: snapshot.camera?.mode,
                    displayStatus: snapshot.camera?.display_status,
                    localMonitoringAllowed: localMonitoringAllowed
                ) {
                    DiagnosticLog.core("keep_awake_session_reconcile core_display_status=suspended local_active=\(active)")
                    self.reportKeepAwakeSession(safety: localSafety)
                }
            }
            let keepAwake = snapshot.keepAwake
            self.keepAwakeController.update(enabled: keepAwake.enabled, requested: keepAwake.requested,
                                            generation: keepAwake.generation, observationID: keepAwake.observationID,
                                            validForMilliseconds: snapshot.keepAwakeRemainingMilliseconds())
            // Before the terminating guard, so snapshots keep reaching the core during shutdown restoration.
            self.controls.observe(snapshot)
            self.ddc.process(snapshot)
            guard !self.terminating else { return }
            self.calibration.monitorsChanged()
            self.protection.reconcile(snapshot.builtinProtection)
            self.hardwareKeyRouter.apply(enabled: snapshot.settings?.hardware_keys_enabled ?? false,
                                         displays: self.hardwareKeyDisplays)
            self.syncControlsConfiguration()
            let followedIDs: Set<String> = snapshot.ambient?.active == true
                ? Set((snapshot.monitors ?? []).filter { $0.follow == "eligible" }.map(\.id))
                : []
            if followedIDs != self.lastFollowedIDs {
                self.lastFollowedIDs = followedIDs
                self.hardwareKeyRouter.setFollowedBrightnessDisplays(followedIDs)
            }
            if snapshot.settings?.hardware_keys_enabled ?? false {
                self.defaultAudioOutput.start()
            } else {
                self.defaultAudioOutput.stop()
            }
            self.builtInMonitor.setEnabled((snapshot.settings?.ambientFollowEnabled ?? false)
                || (snapshot.builtinProtection.protectable && snapshot.builtinProtection.protected))
        }
        // Feed the current sound-output UID into the router; see DefaultAudioOutputMonitor.
        defaultAudioOutput.onChange = { [weak self] uid in
            self?.hardwareKeyRouter.setDefaultOutput(uid: uid)
            self?.syncControlsConfiguration()
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
            self.syncControlsConfiguration()
        }
        ddc.controls = controls
        controls.onDraftChanged = { [weak self] _, _ in self?.manualControls.draftChanged() }
        ddc.start()
        // Before protection, which may hold automatic brightness again once its own record is recovered.
        autoBrightnessOwner.recoverIfNeeded()
        protection.recoverIfNeeded()
        builtInMonitor.start()
        calibration.recoverIfNeeded()
        // Availability is re-read on display changes, system wake, and display sleep and wake; there is no polling for it.
        screenObserver = NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            self?.builtInMonitor.refreshAvailability()
            self?.keepAwakeBackend.setDisplayAsleep(Self.currentDisplaysAreAsleep())
            self?.reportKeepAwakeSession()
        }
        wakeObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.builtInMonitor.refreshAvailability()
            self?.keepAwakeBackend.setSystemSleeping(false)
            self?.keepAwakeBackend.setDisplayAsleep(Self.currentDisplaysAreAsleep())
            self?.reportKeepAwakeSession()
            // A lid change can be missed around sleep, so the state is read again on wake.
            self?.camera.setLidClosed(DisplayServicesBackend.lidClosed())
        }
        displaySleepObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: .main) { [weak self] _ in
            self?.calibration.end(.displaySleep)
            self?.builtInMonitor.refreshAvailability()
            self?.keepAwakeBackend.setDisplayAsleep(true)
            self?.reportKeepAwakeSession()
        }
        screensWakeObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.builtInMonitor.refreshAvailability()
            self?.keepAwakeBackend.setDisplayAsleep(Self.currentDisplaysAreAsleep())
            self?.reportKeepAwakeSession()
            self?.camera.setLidClosed(DisplayServicesBackend.lidClosed())
            // The display can still read asleep when the notification arrives, so availability is checked again shortly after.
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.builtInMonitor.refreshAvailability() }
        }
        systemSleepObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            self?.keepAwakeBackend.setSystemSleeping(true)
            self?.reportKeepAwakeSession()
        }
        sessionResignObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.sessionDidResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.keepAwakeBackend.setSessionActive(false)
            self?.reportKeepAwakeSession()
        }
        sessionBecomeObserver = NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.sessionDidBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.keepAwakeBackend.setSessionActive(true)
            self?.reportKeepAwakeSession()
        }
        lockedObserver = DistributedNotificationCenter.default().addObserver(forName: Notification.Name("com.apple.screenIsLocked"), object: nil, queue: .main) { [weak self] _ in
            self?.keepAwakeBackend.setSessionLocked(true)
            self?.reportKeepAwakeSession()
        }
        unlockedObserver = DistributedNotificationCenter.default().addObserver(forName: Notification.Name("com.apple.screenIsUnlocked"), object: nil, queue: .main) { [weak self] _ in
            self?.keepAwakeBackend.setSessionLocked(false)
            self?.keepAwakeBackend.setSessionActive(true)
            self?.reportKeepAwakeSession()
        }
        observeScreensaver()
        // Started after the camera exists; the first callback sets the lid state the camera starts with.
        lidMonitor.start()
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

    func requestKeepAwakePermission() {
        keepAwakeController.requestPostingPermissionFromUserAction()
    }

    private func reconcileKeepAwakeSession() {
        reportKeepAwakeSession()
    }

    /// Applies the local veto before sending it to the core so an already queued positive snapshot cannot
    /// post activity after a manual lock, saver, sleep, display-off or lid transition.
    private func reportKeepAwakeSession() {
        let safety = keepAwakeBackend.safetyState()
        reportKeepAwakeSession(safety: safety)
    }

    private func reportKeepAwakeSession(safety: KeepAwakeSafety) {
        let active = KeepAwakeSafety.cameraMonitoringAllowed(for: safety)
        let diagnostic = keepAwakeBackend.diagnosticState()
        if diagnostic != lastKeepAwakeDiagnostic {
            lastKeepAwakeDiagnostic = diagnostic
            DiagnosticLog.core("keep_awake_guard \(diagnostic)")
        }
        keepAwakeController.setSessionEligible(active)
        keepAwakeController.safetyDidChange()
        let accepted = core.submit(["type": "keep_awake_session", "active": active])
        DiagnosticLog.core("keep_awake_session_submit active=\(active) accepted=\(accepted)")
    }

    private static func currentDisplaysAreAsleep() -> Bool {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return true }
        var displays = [CGDirectDisplayID](repeating: 0, count: Int(count))
        var actual = count
        guard CGGetActiveDisplayList(count, &displays, &actual) == .success, actual > 0 else { return true }
        return displays.prefix(Int(actual)).contains { CGDisplayIsAsleep($0) != 0 }
    }

    /// Removes the permission observers; safe to call more than once.
    private func removePermissionObservers() {
        if let activationObserver { NotificationCenter.default.removeObserver(activationObserver) }
        if let accessibilityObserver { DistributedNotificationCenter.default().removeObserver(accessibilityObserver) }
        activationObserver = nil
        accessibilityObserver = nil
    }

    private func removeKeepAwakeObservers() {
        let workspace = NSWorkspace.shared.notificationCenter
        for observer in [wakeObserver, displaySleepObserver, screensWakeObserver, systemSleepObserver,
                         sessionResignObserver, sessionBecomeObserver] {
            if let observer { workspace.removeObserver(observer) }
        }
        let distributed = DistributedNotificationCenter.default()
        for observer in [lockedObserver, unlockedObserver, saverStartObserver, saverStopObserver] {
            if let observer { distributed.removeObserver(observer) }
        }
        wakeObserver = nil; displaySleepObserver = nil; screensWakeObserver = nil; systemSleepObserver = nil
        sessionResignObserver = nil; sessionBecomeObserver = nil
        lockedObserver = nil; unlockedObserver = nil; saverStartObserver = nil; saverStopObserver = nil
    }

    /// Stops the built-in monitor and removes its observers; safe to call more than once.
    private func stopBuiltInMonitor() {
        if let screenObserver { NotificationCenter.default.removeObserver(screenObserver) }
        if let wakeObserver { NSWorkspace.shared.notificationCenter.removeObserver(wakeObserver) }
        if let displaySleepObserver { NSWorkspace.shared.notificationCenter.removeObserver(displaySleepObserver) }
        if let screensWakeObserver { NSWorkspace.shared.notificationCenter.removeObserver(screensWakeObserver) }
        if let settingsCloseObserver { NotificationCenter.default.removeObserver(settingsCloseObserver) }
        screenObserver = nil
        wakeObserver = nil
        displaySleepObserver = nil
        screensWakeObserver = nil
        settingsCloseObserver = nil
        builtInMonitor.stop()
    }

    /// Called by the protection controller on main when it starts or finishes holding the built-in display dimmed.
    private func setProtectionHolding(_ holding: Bool) {
        // Read before the lock, which the monitor queue also takes; the level is needed only when the hold ends.
        let level: Double? = holding ? nil : builtInBackend.readLevel()
        builtInLock.lock()
        protectionHolding = holding
        // The cached report still carries the level the poll reported while the screen was dimmed. When the hold
        // ends, that level is replaced with the current one, so the core does not store the dimmed value as the level.
        if !holding, var report = lastBuiltInReport, report.available {
            report.brightness = MenuBarController.wholePercent(level)
            lastBuiltInReport = report
        }
        builtInLock.unlock()
        publishBuiltIn()
    }

    /// Whole percent clamped to 0...100, or nil when the level is unknown. Same rounding as `BuiltInBrightnessMonitor`.
    private static func wholePercent(_ level: Double?) -> Int? {
        guard let level, level.isFinite else { return nil }
        return Int(min(max(level, 0), 100).rounded())
    }

    /// Sends the last built-in report and the holding flag to the core. The lock is held across the submit so that
    /// reports from the monitor queue and holding changes from main reach the core in the order they were set.
    private func publishBuiltIn() {
        builtInLock.lock()
        defer { builtInLock.unlock() }
        guard let report = lastBuiltInReport else { return }
        core.submitBuiltInDisplay(available: report.available, brightness: report.brightness, calibrating: report.calibrating,
                                  protectable: BuiltInProtectionController.isProtectable(reason: report.reason),
                                  holding: protectionHolding)
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        hardwareKeyRouter.apply(enabled: core.snapshot.settings?.hardware_keys_enabled ?? false,
                                displays: hardwareKeyDisplays)
    }

    /// Applies the presence icon and its accessibility label; does nothing unless the state or locale changed.
    private func updateStatusItemIcon(presence: String?, locale: String?) {
        let state = PresenceIconState(presence: presence)
        guard state != appliedIconState || locale != appliedIconLocale else { return }
        appliedIconState = state
        appliedIconLocale = locale
        let description = StatusItemIcon.accessibilityDescription(for: state, locale: locale)
        let image = StatusItemIcon.image(for: state)
        image.accessibilityDescription = description
        statusItem.button?.image = image
        statusItem.button?.setAccessibilityLabel(description)
    }

    @objc private func togglePopover(_ sender: Any?) {
        guard let button = statusItem.button else { return }
        if popover.isShown { popover.performClose(sender) }
        else { core.refresh(); popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY); NSApp.activate(ignoringOtherApps: true) }
    }

    @MainActor private func openSettings(tab: SettingsTab? = nil) {
        let invokingScreen = popover.contentViewController?.view.window?.screen
            ?? statusItem.button?.window?.screen
            ?? settingsWindow?.screen
            ?? NSScreen.main
        popover.performClose(nil)
        if let tab { settingsNavigation.tab = tab }
        let window: NSWindow
        if let settingsWindow {
            window = settingsWindow
        } else {
            // Share the camera's live report with the native Settings scene and menu-bar Settings window.
            let settingsView = SettingsView(core: core, navigation: settingsNavigation, updateChecker: updateChecker,
                                            installedVersion: installedVersion, requestHardwareKeyPermission: { [weak self] in
                self?.requestHardwareKeyPermission()
            }, ambientAvailability: ambientAvailability, inferenceReport: camera.inferenceReport, calibration: calibration,
                                            builtIn: builtInControls)
                .fixedSettingsWindowSize()
            let created = NSWindow(contentRect: NSRect(origin: .zero, size: SettingsWindowLayout.contentSize),
                                   styleMask: SettingsWindowLayout.styleMask,
                                   backing: .buffered, defer: false)
            created.contentMinSize = SettingsWindowLayout.contentSize
            created.contentMaxSize = SettingsWindowLayout.contentSize
            created.title = "TelaViva OLED"
            created.contentViewController = NSHostingController(rootView: settingsView)
            created.isReleasedWhenClosed = false
            settingsCloseObserver = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: created, queue: .main) { [weak self] _ in
                self?.calibration.end(.windowClosed)
            }
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
        saverStartObserver = center.addObserver(forName: Notification.Name("com.apple.screensaver.didstart"), object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.keepAwakeBackend.setScreensaverActive(true)
            self.reportKeepAwakeSession()
            self.core.submit(["type": "screensaver", "state": "active"])
        }
        saverStopObserver = center.addObserver(forName: Notification.Name("com.apple.screensaver.didstop"), object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.keepAwakeBackend.setScreensaverActive(false)
            self.reportKeepAwakeSession()
            self.core.submit(["type": "screensaver", "state": "inactive"])
        }
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
        keepAwakeBackend.setSessionActive(false)
        reportKeepAwakeSession()
        keepAwakeController.shutdown()
        terminationReply = { [weak self] allow in
            guard let self else { return }
            self.terminationPoll?.cancel(); self.terminationDeadline?.cancel()
            self.defaultAudioOutput.stop()
            self.hardwareKeyRouter.shutdown()
            self.removePermissionObservers()
            // The driver serves the core's shutdown restoration until the DDC controller stops.
            self.controls.shutdown()
            self.ddc.shutdown()
            self.terminationReply = nil
            NSApp.reply(toApplicationShouldTerminate: allow)
        }
        if let powerSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        defaultAudioOutput.stop()
        hardwareKeyRouter.shutdown()
        removePermissionObservers()
        removeKeepAwakeObservers()
        calibration.end(.appQuit)
        protection.restoreNow()
        stopBuiltInMonitor()
        lidMonitor.stop()
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

    /// What was last sent with `configure`, so an unchanged state is not re-sent.
    private struct ControlsConfiguration: Equatable {
        let brightnessStep: Double
        let volumeStep: Double
        let keysEnabled: Bool
        let volumeOwner: String?
    }
    private var lastControlsConfiguration: ControlsConfiguration?

    /// Sends the key steps, key routing and the monitor that owns the volume keys to the core when any of them changed.
    /// A different volume owner makes the core forget remembered mute levels. Main thread only.
    private func syncControlsConfiguration() {
        guard Thread.isMainThread else {
            DispatchQueue.main.async { [weak self] in self?.syncControlsConfiguration() }
            return
        }
        let settings = core.snapshot.settings
        let keysEnabled = settings?.hardware_keys_enabled ?? false
        let configuration = ControlsConfiguration(
            brightnessStep: Double(settings?.brightness_key_step ?? 5),
            volumeStep: Double(settings?.volume_key_step ?? 5),
            keysEnabled: keysEnabled,
            volumeOwner: keysEnabled ? hardwareKeyRouter.volumeOwnerMonitorID() : nil
        )
        guard configuration != lastControlsConfiguration else { return }
        lastControlsConfiguration = configuration
        controls.configure(brightnessStep: configuration.brightnessStep, volumeStep: configuration.volumeStep,
                           keysEnabled: configuration.keysEnabled,
                           volumeOwner: configuration.volumeOwner.map { .monitor($0) } ?? .none)
    }

    func applicationWillTerminate(_ notification: Notification) {
        calibration.end(.appQuit)
        // Idempotent: a restore that already completed at quit is a no-op here.
        protection.restoreNow()
        if let powerSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), powerSource, .defaultMode) }
        camera.shutdown()
        defaultAudioOutput.stop()
        hardwareKeyRouter.shutdown()
        removePermissionObservers()
        stopBuiltInMonitor()
        lidMonitor.stop()
        // The driver serves the core's shutdown restoration until the DDC controller stops.
        controls.shutdown()
        ddc.shutdown()
    }
}

private let powerSourceChanged: IOPowerSourceCallbackType = { context in
    guard let context else { return }
    Unmanaged<MenuBarController>.fromOpaque(context).takeUnretainedValue().reportPower()
}
