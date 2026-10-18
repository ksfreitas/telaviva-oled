import Foundation
import Combine

/// Tabs of the settings window.
enum SettingsTab: Hashable {
    case general, keyboard, ambient, advanced, about
}

/// Shared tab selection: menus write it, SettingsView binds to it.
final class SettingsNavigation: ObservableObject {
    @Published var tab: SettingsTab = .general

    init() {}
}
