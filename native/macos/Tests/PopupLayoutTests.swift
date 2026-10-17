import Foundation
import CoreGraphics

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else {
        FileHandle.standardError.write(Data("PopupLayoutTests: \(message)\n".utf8))
        exit(1)
    }
}

@main
private struct PopupLayoutTests {
    static func main() {
        maxVisibleCardsIsThree()
        emptyListHasNoHeight()
        oneToThreeCardsSumWithSpacing()
        fourAndFiveCardsStopAtThreeVisible()
        differingHeightsSumInOrder()
        screenCapClampsNaturalHeight()
        largerScreenCapLeavesHeight()
        nilScreenCapDoesNotClamp()
        nonPositiveScreenCapClampsToZero()
        listCapLeavesRoomForChromeAndMenuBar()
        listCapHasMinimumOnTinyScreens()
        print("Popup layout tests passed")
    }

    private static func maxVisibleCardsIsThree() {
        expect(PopupLayout.maxVisibleCards == 3, "popup shows at most three cards before scrolling")
    }

    private static func emptyListHasNoHeight() {
        expect(PopupLayout.listHeight(cardHeights: [], spacing: 8, screenCap: nil) == 0,
               "empty card list has zero height")
    }

    private static func oneToThreeCardsSumWithSpacing() {
        expect(PopupLayout.listHeight(cardHeights: [40], spacing: 8, screenCap: nil) == 40,
               "one card has no spacing")
        expect(PopupLayout.listHeight(cardHeights: [40, 40], spacing: 8, screenCap: nil) == 88,
               "two cards add one spacing")
        expect(PopupLayout.listHeight(cardHeights: [40, 40, 40], spacing: 8, screenCap: nil) == 136,
               "three cards add two spacings")
    }

    private static func fourAndFiveCardsStopAtThreeVisible() {
        // First three cards: 40 + 60 + 50 = 150, plus two spacings of 8.
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50, 70], spacing: 8, screenCap: nil) == 166,
               "fourth card scrolls instead of growing the list")
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50, 70, 90], spacing: 8, screenCap: nil) == 166,
               "fifth card scrolls instead of growing the list")
    }

    private static func differingHeightsSumInOrder() {
        expect(PopupLayout.listHeight(cardHeights: [30, 70, 20], spacing: 4, screenCap: nil) == 128,
               "differing card heights are summed with spacing")
        expect(PopupLayout.listHeight(cardHeights: [10, 20, 30, 1000], spacing: 4, screenCap: nil) == 68,
               "only the first three differing heights contribute")
    }

    private static func screenCapClampsNaturalHeight() {
        // Natural height of the 40/60/50 cards with spacing 8 is 166.
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50], spacing: 8, screenCap: 100) == 100,
               "screen cap below natural height clamps the list")
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50], spacing: 8, screenCap: 150) == 150,
               "screen cap between zero and natural height clamps the list")
    }

    private static func largerScreenCapLeavesHeight() {
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50], spacing: 8, screenCap: 500) == 166,
               "screen cap above natural height does not change the list")
    }

    private static func nilScreenCapDoesNotClamp() {
        expect(PopupLayout.listHeight(cardHeights: [500, 500, 500], spacing: 8, screenCap: nil) == 1516,
               "nil screen cap leaves the natural height unclamped")
    }

    private static func nonPositiveScreenCapClampsToZero() {
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50], spacing: 8, screenCap: 0) == 0,
               "zero screen cap clamps the list to zero")
        expect(PopupLayout.listHeight(cardHeights: [40, 60, 50], spacing: 8, screenCap: -10) == 0,
               "negative screen cap clamps the list to zero")
    }

    private static func listCapLeavesRoomForChromeAndMenuBar() {
        expect(PopupLayout.listCap(visibleScreenHeight: 900) == 900 - PopupLayout.chromeHeight - 100,
               "900 pt screen leaves room for chrome and the menu bar margin")
    }

    private static func listCapHasMinimumOnTinyScreens() {
        expect(PopupLayout.listCap(visibleScreenHeight: 0) == 120,
               "list cap never drops below 120 on a zero-height screen")
        expect(PopupLayout.listCap(visibleScreenHeight: 100) == 120,
               "list cap floors at 120 when chrome exceeds the screen")
    }
}
