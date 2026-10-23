import AppKit
import CoreGraphics
import SwiftUI

enum SettingsWindowLayout {
    /// Fixed content size of the Settings window, in points. The window is not resizable; tab content that
    /// does not fit scrolls inside it.
    static let contentSize = CGSize(width: 760, height: 720)
    /// Title bar controls without `.resizable`, so edge dragging and the zoom button are both unavailable.
    static let styleMask: NSWindow.StyleMask = [.titled, .closable, .miniaturizable]
}

extension View {
    /// Pins this view to `SettingsWindowLayout.contentSize` so a hosting window cannot grow or shrink it.
    func fixedSettingsWindowSize() -> some View {
        frame(width: SettingsWindowLayout.contentSize.width, height: SettingsWindowLayout.contentSize.height)
    }
}
