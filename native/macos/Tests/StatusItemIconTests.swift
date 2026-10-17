import AppKit
import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("StatusItemIconTests: \(message)\n".utf8))
        exit(1)
    }
}

private func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("StatusItemIconTests: \(message)\n".utf8))
    exit(1)
}

@main
private struct StatusItemIconTests {
    private static let allStates: [PresenceIconState] = [.detected, .absent, .dark]
    private static let keys: [PresenceIconState: String] = [
        .detected: "statusItem.presence.detected",
        .absent: "statusItem.presence.absent",
        .dark: "statusItem.presence.dark",
    ]

    static func main() {
        presenceMapping()
        imagesAreTemplatesAt18Points()
        detectedShowsFilledDot()
        absentHasNoDot()
        darkShowsRing()
        screenOutlineIsOpaque()
        accessibilityDescriptionsAreLocalized()
        print("Status item icon tests passed")
    }

    private static func presenceMapping() {
        expect(PresenceIconState(presence: nil) == .absent, "missing presence maps to absent")
        expect(PresenceIconState(presence: "present") == .detected, "present maps to detected")
        expect(PresenceIconState(presence: "absent") == .absent, "absent maps to absent")
        expect(PresenceIconState(presence: "insufficient_light") == .dark, "insufficient_light maps to dark")
        expect(PresenceIconState(presence: "weird") == .absent, "unknown presence maps to absent")
    }

    private static func imagesAreTemplatesAt18Points() {
        for state in allStates {
            let image = StatusItemIcon.image(for: state)
            expect(image.isTemplate, "\(state) icon is a template image")
            expect(image.size.width == 18 && image.size.height == 18, "\(state) icon is 18x18 points")
        }
    }

    private static func detectedShowsFilledDot() {
        let rep = render(.detected)
        expect(alpha(rep, x: 36, y: 30) > 0.9, "detected icon has a filled dot at the center")
    }

    private static func absentHasNoDot() {
        let rep = render(.absent)
        expect(alpha(rep, x: 36, y: 30) < 0.05, "absent icon has no dot at the center")
        expect(alpha(rep, x: 44, y: 30) < 0.05, "absent icon has no ring on the right of the center")
    }

    private static func darkShowsRing() {
        let rep = render(.dark)
        expect(alpha(rep, x: 36, y: 30) < 0.05, "dark icon has no filled dot at the center")
        expect(alpha(rep, x: 44, y: 30) > 0.5, "dark icon has a ring on the right of the center")
    }

    private static func screenOutlineIsOpaque() {
        for state in allStates {
            let rep = render(state)
            expect(alpha(rep, x: 6, y: 40) > 0.9, "\(state) icon has an opaque screen outline")
        }
    }

    private static func accessibilityDescriptionsAreLocalized() {
        for locale in ["en-US", "pt-BR"] {
            var seen: Set<String> = []
            for state in allStates {
                guard let key = keys[state] else { fail("missing key for \(state)") }
                let description = StatusItemIcon.accessibilityDescription(for: state, locale: locale)
                expect(!description.isEmpty, "\(locale) description is not empty for \(state)")
                expect(description != key, "\(locale) description is translated for \(state)")
                let isNew = seen.insert(description).inserted
                expect(isNew, "\(locale) descriptions are distinct per state")
            }
        }
    }

    /// Draws the state's template image into a 72x72 pixel bitmap (4x of 18 points).
    private static func render(_ state: PresenceIconState) -> NSBitmapImageRep {
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 72, pixelsHigh: 72,
                                         bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                         colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else {
            fail("could not create a 72x72 bitmap")
        }
        guard let context = NSGraphicsContext(bitmapImageRep: rep) else {
            fail("could not create a bitmap graphics context")
        }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        StatusItemIcon.image(for: state).draw(in: NSRect(x: 0, y: 0, width: 72, height: 72))
        NSGraphicsContext.restoreGraphicsState()
        return rep
    }

    /// Alpha at a pixel; the origin is the top-left corner.
    private static func alpha(_ rep: NSBitmapImageRep, x: Int, y: Int) -> CGFloat {
        rep.colorAt(x: x, y: y)?.alphaComponent ?? 0
    }
}
