// Rasterizes an SVG to a square transparent PNG with AppKit.
// Used only by tools/branding/build-icons.sh to render the .icns fallback.
import AppKit
let a = CommandLine.arguments
guard a.count == 4, let img = NSImage(contentsOf: URL(fileURLWithPath: a[1])), let px = Int(a[3]) else {
    FileHandle.standardError.write(Data("usage: render-svg in.svg out.png pixels\n".utf8))
    exit(2)
}
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSColor.clear.set()
NSRect(x: 0, y: 0, width: px, height: px).fill(using: .copy)
img.draw(in: NSRect(x: 0, y: 0, width: px, height: px))
NSGraphicsContext.restoreGraphicsState()
guard let png = rep.representation(using: .png, properties: [:]) else { exit(1) }
do { try png.write(to: URL(fileURLWithPath: a[2])) } catch { exit(1) }
