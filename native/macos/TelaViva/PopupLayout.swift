import CoreGraphics
import SwiftUI

enum PopupLayout {
    /// The popup shows at most this many monitor cards before the list scrolls.
    static let maxVisibleCards = 3
    /// Header, divider, footer and paddings. Approximate; only used for the safety cap.
    static let chromeHeight: CGFloat = 150
    /// Card height assumed until a card has been measured.
    static let estimatedCardHeight: CGFloat = 140

    /// Height of the card list: the first `maxVisibleCards` heights plus the spacing between them,
    /// clamped to `screenCap` when one is given.
    static func listHeight(cardHeights: [CGFloat], spacing: CGFloat, screenCap: CGFloat?) -> CGFloat {
        let visible = cardHeights.prefix(maxVisibleCards)
        guard !visible.isEmpty else { return 0 }
        let natural = visible.reduce(0, +) + spacing * CGFloat(visible.count - 1)
        guard let cap = screenCap else { return natural }
        return min(natural, max(0, cap))
    }

    /// Upper bound for the list on a screen with the given visible height, leaving room for chrome and the menu bar.
    static func listCap(visibleScreenHeight: CGFloat) -> CGFloat {
        max(120, visibleScreenHeight - chromeHeight - 100)
    }
}

/// Card heights keyed by card ID, collected up the view tree. When keys repeat, the later value wins.
struct PopupCardHeightsKey: PreferenceKey {
    static var defaultValue: [String: CGFloat] = [:]

    static func reduce(value: inout [String: CGFloat], nextValue: () -> [String: CGFloat]) {
        value.merge(nextValue()) { _, next in next }
    }
}

extension View {
    /// Publishes this view's height under `id` through `PopupCardHeightsKey` without affecting layout.
    func reportPopupCardHeight(id: String) -> some View {
        background(
            GeometryReader { proxy in
                Color.clear.preference(key: PopupCardHeightsKey.self, value: [id: proxy.size.height])
            }
        )
    }
}
