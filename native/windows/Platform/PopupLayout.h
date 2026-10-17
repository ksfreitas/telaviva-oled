#pragma once
// Flyout size rules in device-independent pixels (DIP). The caller multiplies the
// result by the DPI scale, so the outcome does not depend on DPI or text scale.
// Header-only and free of windows.h so the Tests project can include it.
// Mirrors the card-list rules in native/macos/TelaViva/PopupLayout.swift.

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace telaviva {

// Matches PopupMetrics::maxVisibleCards; the measured layout below uses it directly.
inline constexpr std::size_t kMeasuredVisibleCards = 3;

// Placeholder values. Tune them against real Windows 11 flyouts in a later prototype.
struct PopupMetrics {
    double width = 360;
    double headerHeight = 0;
    double footerHeight = 56;
    double cardSpacing = 8;
    double outerPadding = 12;
    double emptyStateHeight = 96;
    double cardHeightBrightnessAndVolume = 132;
    double cardHeightFollowedWithProtection = 132;
    double cardHeightFollowed = 88;
    int maxVisibleCards = 3;
    double maxHeight = 640;
};

enum class CardKind { BrightnessAndVolume, FollowedWithProtection, Followed };

struct PopupSize {
    double width;
    double height;
    bool scrolls;
    int visibleCards;
};

inline double cardHeight(CardKind kind, const PopupMetrics& metrics)
{
    switch (kind) {
    case CardKind::BrightnessAndVolume:
        return metrics.cardHeightBrightnessAndVolume;
    case CardKind::FollowedWithProtection:
        return metrics.cardHeightFollowedWithProtection;
    case CardKind::Followed:
        break;
    }
    return metrics.cardHeightFollowed;
}

inline PopupSize computePopupSize(std::span<const CardKind> cards, const PopupMetrics& metrics = {})
{
    // Padding is counted once, matching the single outerPadding metric.
    const double chrome = metrics.outerPadding + metrics.headerHeight + metrics.footerHeight;

    if (cards.empty()) {
        // The empty state never collapses below footer and padding.
        const double minimum = metrics.footerHeight + metrics.outerPadding;
        const double height =
            std::min(std::max(chrome + metrics.emptyStateHeight, minimum), metrics.maxHeight);
        return PopupSize{metrics.width, height, false, 0};
    }

    const std::size_t total = cards.size();
    // Floor at one so a non-positive maxVisibleCards cannot hide every card.
    const std::size_t maxVisible = static_cast<std::size_t>(std::max(metrics.maxVisibleCards, 1));
    std::size_t visible = std::min(total, maxVisible);

    // Only the first n cards count, the same prefix rule as listHeight in the macOS code.
    auto heightFor = [&](std::size_t n) {
        double sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            sum += cardHeight(cards[i], metrics);
        }
        return chrome + sum + metrics.cardSpacing * static_cast<double>(n - 1);
    };

    // Drop cards from the bottom until the flyout fits, but keep at least one.
    while (visible > 1 && heightFor(visible) > metrics.maxHeight) {
        --visible;
    }

    double height = heightFor(visible);
    const bool clamped = height > metrics.maxHeight;
    if (clamped) {
        // Only reached when a single card is taller than maxHeight.
        height = metrics.maxHeight;
    }
    return PopupSize{metrics.width, height, clamped || visible < total, static_cast<int>(visible)};
}

struct PopupHeight {
    double windowHeight;
    double scrollViewportHeight;
    bool needsScroll;
};

// Measured layout, all in DIP. cardHeights are the desired heights of the cards (or of the
// empty/notice panel, passed as the only entry) in display order. The viewport is the first
// kMeasuredVisibleCards entries plus the spacing between them, so three cards fit without
// scrolling and only a fourth card scrolls. Unlike computePopupSize, this trusts the caller's
// measurements, so text scale and wrapping are already counted.
inline PopupHeight computePopupHeight(const std::vector<double>& cardHeights, double headerHeight,
    double footerHeight, double noticeHeight, double cardSpacing, double padding,
    double maxHeightCap, double workAreaHeight)
{
    const std::size_t total = cardHeights.size();
    const std::size_t visible = std::min(total, kMeasuredVisibleCards);

    double viewport = 0;
    for (std::size_t i = 0; i < visible; ++i) {
        viewport += std::max(0.0, cardHeights[i]);
    }
    if (visible > 1) {
        viewport += cardSpacing * static_cast<double>(visible - 1);
    }

    const double chrome = std::max(0.0, padding) + std::max(0.0, headerHeight) +
                          std::max(0.0, noticeHeight) + std::max(0.0, footerHeight);
    const double wanted = chrome + viewport;

    // The work area can be shorter than the cap; the list then scrolls even with few cards.
    const double cap = std::min(maxHeightCap, workAreaHeight);
    const bool clamped = wanted > cap;
    const double windowHeight = clamped ? cap : wanted;

    return PopupHeight{windowHeight, std::max(0.0, windowHeight - chrome),
        clamped || total > kMeasuredVisibleCards};
}

}  // namespace telaviva
