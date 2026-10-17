// Tests for the flyout size rules in native/windows/Platform/PopupLayout.h.
// Expected values follow the default metrics: chrome 68 (12 padding + 56 footer),
// cards 132 / 132 / 88, spacing 8, maxHeight 640.
#include "TestHarness.h"

#include "../Platform/PopupLayout.h"

#include <cmath>
#include <span>
#include <vector>

namespace {

using telaviva::CardKind;
using telaviva::PopupMetrics;
using telaviva::PopupSize;

// Heights are whole DIP values, so a tight tolerance is enough.
constexpr double kTolerance = 1e-9;

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) < kTolerance;
}

}  // namespace

TEST(popupLayout_zeroCardsGiveEmptyStateSize)
{
    const PopupSize size = telaviva::computePopupSize(std::span<const CardKind>{});
    // 12 padding + 0 header + 96 empty state + 56 footer.
    CHECK(nearlyEqual(size.height, 164.0));
    CHECK(nearlyEqual(size.width, 360.0));
    CHECK(!size.scrolls);
    CHECK_EQ(size.visibleCards, 0);
}

TEST(popupLayout_emptyStateNeverSmallerThanFooterAndPadding)
{
    PopupMetrics metrics;
    metrics.emptyStateHeight = 0;
    const PopupSize size = telaviva::computePopupSize(std::span<const CardKind>{}, metrics);
    // Footer 56 + padding 12.
    CHECK(nearlyEqual(size.height, 68.0));
}

TEST(popupLayout_oneCardHasNoSpacing)
{
    const CardKind cards[] = {CardKind::BrightnessAndVolume};
    const PopupSize size = telaviva::computePopupSize(cards);
    // 68 chrome + 132 card.
    CHECK(nearlyEqual(size.height, 200.0));
    CHECK(!size.scrolls);
    CHECK_EQ(size.visibleCards, 1);
}

TEST(popupLayout_twoMixedCardsAddOneSpacing)
{
    const CardKind cards[] = {CardKind::BrightnessAndVolume, CardKind::Followed};
    const PopupSize size = telaviva::computePopupSize(cards);
    // 68 chrome + 132 + 88 + 8 spacing.
    CHECK(nearlyEqual(size.height, 296.0));
    CHECK(!size.scrolls);
    CHECK_EQ(size.visibleCards, 2);
}

TEST(popupLayout_threeMixedCardsAddTwoSpacings)
{
    const CardKind cards[] = {CardKind::BrightnessAndVolume, CardKind::FollowedWithProtection,
                              CardKind::Followed};
    const PopupSize size = telaviva::computePopupSize(cards);
    // 68 chrome + 132 + 132 + 88 + 16 spacing.
    CHECK(nearlyEqual(size.height, 436.0));
    CHECK(!size.scrolls);
    CHECK_EQ(size.visibleCards, 3);
}

TEST(popupLayout_fourCardsUseThreeCardPrefix)
{
    const CardKind cards4[] = {CardKind::BrightnessAndVolume, CardKind::FollowedWithProtection,
                               CardKind::Followed, CardKind::BrightnessAndVolume};
    const CardKind cards3[] = {CardKind::BrightnessAndVolume, CardKind::FollowedWithProtection,
                               CardKind::Followed};
    const PopupSize size4 = telaviva::computePopupSize(cards4);
    const PopupSize size3 = telaviva::computePopupSize(cards3);
    CHECK(nearlyEqual(size4.height, size3.height));
    CHECK(nearlyEqual(size4.height, 436.0));
    CHECK(size4.scrolls);
    CHECK_EQ(size4.visibleCards, 3);
}

TEST(popupLayout_tenCardsScrollAfterThreeVisible)
{
    const CardKind cards[] = {CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume,
                              CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume,
                              CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume,
                              CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume,
                              CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume};
    const PopupSize size = telaviva::computePopupSize(cards);
    // 68 chrome + 3 * 132 + 2 * 8 spacing.
    CHECK(nearlyEqual(size.height, 480.0));
    CHECK(size.scrolls);
    CHECK_EQ(size.visibleCards, 3);
}

TEST(popupLayout_onlyFirstThreeHeightsCount)
{
    // macOS reproduces heights [10, 20, 30, 1000] with three visible cards. CardKind has
    // only three heights, so this uses [10, 20, 10, 1000]: the tall fourth card must not count.
    PopupMetrics metrics;
    metrics.cardHeightFollowed = 10;
    metrics.cardHeightFollowedWithProtection = 20;
    metrics.cardHeightBrightnessAndVolume = 1000;
    metrics.cardSpacing = 4;
    metrics.maxHeight = 10000;
    const CardKind cards[] = {CardKind::Followed, CardKind::FollowedWithProtection,
                              CardKind::Followed, CardKind::BrightnessAndVolume};
    const PopupSize size = telaviva::computePopupSize(cards, metrics);
    // 12 + 56 chrome + (10 + 20 + 10) + 2 * 4 spacing.
    CHECK(nearlyEqual(size.height, 116.0));
    CHECK(size.scrolls);
    CHECK_EQ(size.visibleCards, 3);
}

TEST(popupLayout_maxHeightReducesVisibleCards)
{
    PopupMetrics metrics;
    metrics.maxHeight = 400;
    const CardKind cards[] = {CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume,
                              CardKind::BrightnessAndVolume};
    const PopupSize size = telaviva::computePopupSize(cards, metrics);
    // Three cards need 480, so two cards (68 + 264 + 8 = 340) are used.
    CHECK(nearlyEqual(size.height, 340.0));
    CHECK(size.scrolls);
    CHECK_EQ(size.visibleCards, 2);
}

TEST(popupLayout_oversizedCardClampsToMaxHeight)
{
    PopupMetrics metrics;
    metrics.cardHeightBrightnessAndVolume = 1000;
    const CardKind cards[] = {CardKind::BrightnessAndVolume};
    const PopupSize size = telaviva::computePopupSize(cards, metrics);
    CHECK(nearlyEqual(size.height, 640.0));
    CHECK(size.scrolls);
    CHECK_EQ(size.visibleCards, 1);
}

TEST(popupLayout_oversizedCardsKeepOneVisible)
{
    PopupMetrics metrics;
    metrics.cardHeightBrightnessAndVolume = 1000;
    const CardKind cards[] = {CardKind::BrightnessAndVolume, CardKind::BrightnessAndVolume,
                              CardKind::BrightnessAndVolume};
    const PopupSize size = telaviva::computePopupSize(cards, metrics);
    CHECK(nearlyEqual(size.height, 640.0));
    CHECK(size.scrolls);
    CHECK_EQ(size.visibleCards, 1);
}

TEST(popupLayout_followedCardsUseSmallerHeight)
{
    const PopupMetrics metrics{};
    CHECK(nearlyEqual(telaviva::cardHeight(CardKind::Followed, metrics), 88.0));
    CHECK(nearlyEqual(telaviva::cardHeight(CardKind::FollowedWithProtection, metrics), 132.0));
    CHECK(nearlyEqual(telaviva::cardHeight(CardKind::BrightnessAndVolume, metrics), 132.0));

    const CardKind cards[] = {CardKind::Followed};
    const PopupSize size = telaviva::computePopupSize(cards, metrics);
    // 68 chrome + 88 card.
    CHECK(nearlyEqual(size.height, 156.0));
}

TEST(popupLayout_widthPassesThrough)
{
    PopupMetrics metrics;
    metrics.width = 420;
    const CardKind cards[] = {CardKind::Followed};
    CHECK(nearlyEqual(telaviva::computePopupSize(std::span<const CardKind>{}, metrics).width, 420.0));
    CHECK(nearlyEqual(telaviva::computePopupSize(cards, metrics).width, 420.0));
}

TEST(popupLayout_isDeterministic)
{
    const CardKind cards[] = {CardKind::BrightnessAndVolume, CardKind::Followed,
                              CardKind::FollowedWithProtection, CardKind::Followed};
    const PopupSize first = telaviva::computePopupSize(cards);
    const PopupSize second = telaviva::computePopupSize(cards);
    CHECK(nearlyEqual(first.width, second.width));
    CHECK(nearlyEqual(first.height, second.height));
    CHECK(first.scrolls == second.scrolls);
    CHECK_EQ(first.visibleCards, second.visibleCards);
}

// Measured layout. Header 44, footer 72, padding 24, spacing 8, maxHeight 640, work area 1000.
namespace {

telaviva::PopupHeight measured(const std::vector<double>& cards, double notice = 0,
    double workArea = 1000)
{
    return telaviva::computePopupHeight(cards, 44, 72, notice, 8, 24, 640, workArea);
}

}  // namespace

TEST(popupLayout_measuredEmptyListHasChromeOnly)
{
    const telaviva::PopupHeight layout = measured({});
    // 24 padding + 44 header + 72 footer.
    CHECK(nearlyEqual(layout.windowHeight, 140.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 0.0));
    CHECK(!layout.needsScroll);
}

TEST(popupLayout_measuredOneCardNoScroll)
{
    const telaviva::PopupHeight layout = measured({200});
    // 140 chrome + 200 card.
    CHECK(nearlyEqual(layout.windowHeight, 340.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 200.0));
    CHECK(!layout.needsScroll);
}

TEST(popupLayout_measuredTwoCardsNoScroll)
{
    const telaviva::PopupHeight layout = measured({200, 150});
    // 140 chrome + 200 + 150 + 8 spacing.
    CHECK(nearlyEqual(layout.windowHeight, 498.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 358.0));
    CHECK(!layout.needsScroll);
}

TEST(popupLayout_measuredExactlyThreeCardsNoScroll)
{
    const telaviva::PopupHeight layout = measured({100, 100, 100});
    // 140 chrome + 300 + 16 spacing.
    CHECK(nearlyEqual(layout.windowHeight, 456.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 316.0));
    CHECK(!layout.needsScroll);
}

TEST(popupLayout_measuredFourthCardScrollsWithThreeCardViewport)
{
    const telaviva::PopupHeight layout = measured({100, 100, 100, 100});
    CHECK(nearlyEqual(layout.windowHeight, 456.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 316.0));
    CHECK(layout.needsScroll);
}

TEST(popupLayout_measuredTallCardsClampedByCap)
{
    const telaviva::PopupHeight layout = measured({300, 300});
    // 140 + 608 = 748 wanted, capped at 640; the viewport shrinks to fit.
    CHECK(nearlyEqual(layout.windowHeight, 640.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 500.0));
    CHECK(layout.needsScroll);
}

TEST(popupLayout_measuredClampedByWorkArea)
{
    const telaviva::PopupHeight layout = measured({200, 200, 200}, 0, 400);
    // 140 + 616 = 756 wanted, work area 400.
    CHECK(nearlyEqual(layout.windowHeight, 400.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 260.0));
    CHECK(layout.needsScroll);
}

TEST(popupLayout_measuredUsesPrefixNotTallestThree)
{
    // The tallest card is fourth, so it must not count toward the viewport.
    const telaviva::PopupHeight layout = measured({50, 200, 60, 900});
    // 140 chrome + (50 + 200 + 60) + 2 * 8 spacing.
    CHECK(nearlyEqual(layout.windowHeight, 466.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 326.0));
    CHECK(layout.needsScroll);
}

TEST(popupLayout_measuredSpacingCountedNMinusOneTimes)
{
    const telaviva::PopupHeight three = measured({10, 10, 10});
    const telaviva::PopupHeight two = measured({10, 10});
    CHECK(nearlyEqual(three.scrollViewportHeight, 46.0));
    CHECK(nearlyEqual(two.scrollViewportHeight, 28.0));
}

TEST(popupLayout_measuredNoticeHeightIsIncluded)
{
    const telaviva::PopupHeight layout = measured({200}, 30);
    // 170 chrome (with notice) + 200 card.
    CHECK(nearlyEqual(layout.windowHeight, 370.0));
    CHECK(nearlyEqual(layout.scrollViewportHeight, 200.0));
    CHECK(!layout.needsScroll);
}
