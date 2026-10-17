// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "Host/SettingRow.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include <string_view>

namespace telaviva::host {

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace xc = winrt::Microsoft::UI::Xaml::Controls;

// Application resources resolve theme keys for the active theme, so the card follows
// light, dark and high contrast without a palette of its own.
xaml::Media::Brush themeBrush(std::wstring_view key)
{
    return xaml::Application::Current().Resources().Lookup(winrt::box_value(winrt::hstring{ key })).as<xaml::Media::Brush>();
}

xaml::Style themeStyle(std::wstring_view key)
{
    return xaml::Application::Current().Resources().Lookup(winrt::box_value(winrt::hstring{ key })).as<xaml::Style>();
}

}  // namespace

xaml::FrameworkElement MakeSettingRow(winrt::hstring const& header, winrt::hstring const& description, xaml::UIElement const& control)
{
    xc::TextBlock title;
    title.Text(header);
    title.Style(themeStyle(L"BodyTextBlockStyle"));
    title.TextWrapping(xaml::TextWrapping::Wrap);

    xc::StackPanel labels;
    labels.VerticalAlignment(xaml::VerticalAlignment::Center);
    labels.Children().Append(title);

    xc::TextBlock caption;
    caption.Text(description);
    caption.Style(themeStyle(L"CaptionTextBlockStyle"));
    caption.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
    caption.TextWrapping(xaml::TextWrapping::Wrap);
    // An empty description collapses so a row without one stays as tall as its header.
    caption.Visibility(description.empty() ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
    labels.Children().Append(caption);

    xc::ColumnDefinition textColumn;
    textColumn.Width(xaml::GridLength{ 1, xaml::GridUnitType::Star });
    xc::ColumnDefinition controlColumn;
    controlColumn.Width(xaml::GridLength{ 0, xaml::GridUnitType::Auto });

    xc::Grid grid;
    grid.ColumnDefinitions().Append(textColumn);
    grid.ColumnDefinitions().Append(controlColumn);
    grid.ColumnSpacing(16);

    xc::Grid::SetColumn(labels, 0);
    grid.Children().Append(labels);

    // Grid attached properties take a FrameworkElement, so the UIElement is narrowed first.
    auto element = control.try_as<xaml::FrameworkElement>();
    if (element)
    {
        element.VerticalAlignment(xaml::VerticalAlignment::Center);
        element.HorizontalAlignment(xaml::HorizontalAlignment::Right);
        xc::Grid::SetColumn(element, 1);
    }
    grid.Children().Append(control);

    xc::Border card;
    card.Background(themeBrush(L"CardBackgroundFillColorDefaultBrush"));
    card.BorderBrush(themeBrush(L"CardStrokeColorDefaultBrush"));
    card.BorderThickness(xaml::Thickness{ 1, 1, 1, 1 });
    card.CornerRadius(xaml::CornerRadius{ 4, 4, 4, 4 });
    card.Padding(xaml::Thickness{ 16, 12, 16, 12 });
    card.MinHeight(64);
    card.Child(grid);

    xaml::Automation::AutomationProperties::SetName(card, header);
    return card;
}

xaml::FrameworkElement MakeCaption(winrt::hstring const& text)
{
    xc::TextBlock caption;
    caption.Text(text);
    caption.Style(themeStyle(L"CaptionTextBlockStyle"));
    caption.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
    caption.TextWrapping(xaml::TextWrapping::Wrap);
    return caption;
}

xaml::FrameworkElement MakeSectionHeader(winrt::hstring const& text)
{
    xc::TextBlock header;
    header.Text(text);
    header.Style(themeStyle(L"BodyStrongTextBlockStyle"));
    header.Margin(xaml::Thickness{ 0, 16, 0, 0 });
    // Screen readers jump between section headings; the page title is level 1.
    xaml::Automation::AutomationProperties::SetHeadingLevel(header, xaml::Automation::Peers::AutomationHeadingLevel::Level2);
    return header;
}

}  // namespace telaviva::host
