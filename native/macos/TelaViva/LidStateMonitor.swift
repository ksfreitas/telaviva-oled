import Foundation
import IOKit

/// Watches the MacBook lid through the power manager's clamshell notification and reports changes on the main queue.
/// The camera uses it to stay off the built-in camera while the lid is closed.
final class LidStateMonitor {
    /// `kIOPMMessageClamshellStateChange` is a macro that Swift cannot import: iokit_family_msg(sub_iokit_powermanagement, 0x100).
    private static let clamshellStateChangeMessage: UInt32 = 0xE0034100
    /// Bit 0 of the message argument is set while the lid is closed (`kClamshellStateBit`).
    private static let clamshellStateBit: UInt = 1

    /// Read by the file-scope callback below, so it is fileprivate rather than private.
    fileprivate let onChange: (Bool) -> Void
    // Main thread only.
    private var notificationPort: IONotificationPortRef?
    private var runLoopSource: CFRunLoopSource?
    private var registration: io_object_t = 0
    private var rootDomain: io_service_t = 0
    /// Set by the first `start()` and kept even when registration fails, so a repeated `start()` does not register
    /// again or report the state a second time. Cleared by `stop()`.
    private var started = false

    init(onChange: @escaping (Bool) -> Void) {
        self.onChange = onChange
    }

    deinit { stop() }

    /// Decodes a power manager message: nil for other message types, otherwise whether the lid is closed.
    static func closed(message: UInt32, argument: UInt) -> Bool? {
        guard message == clamshellStateChangeMessage else { return nil }
        return (argument & clamshellStateBit) != 0
    }

    /// Registers for lid changes and reports the current state once. A second call is ignored, even after a failed
    /// registration; the failure is logged so it is not silent.
    func start() {
        guard !started else { return }
        started = true
        if let failure = register() {
            DiagnosticLog.core("lid_monitor registration failed \(failure)")
        }
        // The notification fires only on change, so the state at launch is read once here, even if registration failed.
        onChange(DisplayServicesBackend.lidClosed())
    }

    /// Removes the registration and releases the port and service. Safe to call more than once.
    func stop() {
        if registration != 0 {
            IOObjectRelease(registration)
            registration = 0
        }
        if let runLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, .defaultMode)
            self.runLoopSource = nil
        }
        if let port = notificationPort {
            // Also disposes of the run loop source.
            IONotificationPortDestroy(port)
            notificationPort = nil
        }
        if rootDomain != 0 {
            IOObjectRelease(rootDomain)
            rootDomain = 0
        }
        // A later start() registers again, and its initial report is delivered again.
        started = false
    }

    /// Returns nil when registration succeeded, otherwise a `key=value` reason for the diagnostic line.
    private func register() -> String? {
        let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"))
        guard service != 0 else { return "reason=no_root_domain" }
        let createdPort: IONotificationPortRef? = IONotificationPortCreate(kIOMainPortDefault)
        guard let port = createdPort else {
            IOObjectRelease(service)
            return "reason=no_port"
        }
        var newRegistration: io_object_t = 0
        let refcon = Unmanaged.passUnretained(self).toOpaque()
        let result = IOServiceAddInterestNotification(port, service, kIOGeneralInterest, lidStateCallback, refcon, &newRegistration)
        guard result == KERN_SUCCESS else {
            IONotificationPortDestroy(port)
            IOObjectRelease(service)
            return "reason=add_interest kern_return=\(result)"
        }
        rootDomain = service
        notificationPort = port
        registration = newRegistration
        // Notifications arrive on the main run loop, so the callback runs on the main thread.
        // The SDK returns the source unretained (no annotation), which Swift imports as Unmanaged; the port keeps it alive.
        runLoopSource = IONotificationPortGetRunLoopSource(port)?.takeUnretainedValue()
        if let runLoopSource { CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, .defaultMode) }
        return nil
    }
}

/// Plain C callback for the power manager. It finds the monitor through refcon and hops to the main queue
/// before reporting, so the change is delivered the same way whatever run loop delivered it.
private let lidStateCallback: IOServiceInterestCallback = { refcon, _, messageType, messageArgument in
    guard let refcon,
          let closed = LidStateMonitor.closed(message: messageType, argument: UInt(bitPattern: messageArgument)) else { return }
    let monitor = Unmanaged<LidStateMonitor>.fromOpaque(refcon).takeUnretainedValue()
    DispatchQueue.main.async { monitor.onChange(closed) }
}
