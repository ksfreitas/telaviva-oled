#pragma once
#include "pch.h"

namespace telaviva::host {

// Windows 11 setting card: header and optional description on the left, the control
// on the right. Colors and text styles come from system theme resources only.
// A ToggleSwitch control is shrunk to its track so it sits on the right edge.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeSettingRow(
    winrt::hstring const& header, winrt::hstring const& description,
    winrt::Microsoft::UI::Xaml::UIElement const& control);

// Secondary text for help under a row and for empty states.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeCaption(winrt::hstring const& text);

// Group label above a set of rows.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeSectionHeader(winrt::hstring const& text);

}  // namespace telaviva::host
