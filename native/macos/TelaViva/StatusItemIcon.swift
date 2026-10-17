import AppKit

/// Presence state shown by the menu-bar icon.
enum PresenceIconState: Equatable, Hashable {
    /// A face is detected.
    case detected
    /// No face is detected. Unknown and missing presence values also map here.
    case absent
    /// The camera cannot see well enough to decide.
    case dark

    init(presence: String?) {
        switch presence {
        case "present": self = .detected
        case "insufficient_light": self = .dark
        default: self = .absent
        }
    }
}

/// Menu-bar icon: a display outline with a presence mark.
/// The geometry mirrors assets/branding/menubar/*.svg (y grows downward) and must be kept in sync with them.
enum StatusItemIcon {
    private static var cache: [PresenceIconState: NSImage] = [:]

    /// Must be called on the main thread. Images are cached per state.
    static func image(for state: PresenceIconState) -> NSImage {
        if let cached = cache[state] { return cached }
        let image = NSImage(size: NSSize(width: 18, height: 18), flipped: true) { _ in
            StatusItemIcon.drawIcon(state)
            return true
        }
        image.isTemplate = true
        cache[state] = image
        return image
    }

    static func accessibilityDescription(for state: PresenceIconState, locale: String?) -> String {
        let key: String
        switch state {
        case .detected: key = "statusItem.presence.detected"
        case .absent: key = "statusItem.presence.absent"
        case .dark: key = "statusItem.presence.dark"
        }
        return AppText.string(key, locale: locale)
    }

    private static func drawIcon(_ state: PresenceIconState) {
        NSColor.black.setFill()
        NSColor.black.setStroke()

        // Screen outline.
        let screen = NSBezierPath(roundedRect: NSRect(x: 1.5, y: 2.5, width: 15, height: 10), xRadius: 2.2, yRadius: 2.2)
        screen.lineWidth = 1.5
        screen.stroke()

        // Stand.
        let stand = NSBezierPath()
        stand.move(to: NSPoint(x: 9, y: 12.5))
        stand.line(to: NSPoint(x: 9, y: 15.5))
        stand.move(to: NSPoint(x: 5.5, y: 15.5))
        stand.line(to: NSPoint(x: 12.5, y: 15.5))
        stand.lineWidth = 1.5
        stand.lineCapStyle = .round
        stand.stroke()

        switch state {
        case .detected:
            // Filled dot at the screen center.
            NSBezierPath(ovalIn: NSRect(x: 6.8, y: 5.3, width: 4.4, height: 4.4)).fill()
        case .dark:
            // Hollow ring at the screen center.
            let ring = NSBezierPath(ovalIn: NSRect(x: 7, y: 5.5, width: 4, height: 4))
            ring.lineWidth = 1.1
            ring.stroke()
        case .absent:
            break
        }
    }
}
