import Foundation
import AppKit

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("SettingsWindowLayoutTests: \(message)\n".utf8))
        exit(1)
    }
}

@main
private struct SettingsWindowLayoutTests {
    static func main() {
        contentSizeIsFixedAt760By720()
        styleMaskIsNotResizable()
        styleMaskKeepsTitleCloseAndMiniaturize()
        print("Settings window layout tests passed")
    }

    private static func contentSizeIsFixedAt760By720() {
        expect(SettingsWindowLayout.contentSize.width == 760 && SettingsWindowLayout.contentSize.height == 720,
               "settings window content size is 760 by 720 points")
    }

    private static func styleMaskIsNotResizable() {
        expect(!SettingsWindowLayout.styleMask.contains(.resizable),
               "settings window is not resizable")
    }

    private static func styleMaskKeepsTitleCloseAndMiniaturize() {
        expect(SettingsWindowLayout.styleMask.contains(.titled),
               "settings window keeps its title bar")
        expect(SettingsWindowLayout.styleMask.contains(.closable),
               "settings window keeps its close button")
        expect(SettingsWindowLayout.styleMask.contains(.miniaturizable),
               "settings window keeps its minimize button")
    }
}
