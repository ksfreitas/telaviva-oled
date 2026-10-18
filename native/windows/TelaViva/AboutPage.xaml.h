// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "AboutPage.g.h"
#include "pch.h"
#include "Host/SettingsHost.h"

#include <memory>

namespace winrt::TelaViva::implementation
{
    struct AboutPage : AboutPageT<AboutPage>
    {
        AboutPage();
        ~AboutPage();

        // Implementation-only: not part of the IDL, reached through get_self.
        void SetHost(telaviva::host::ISettingsHost* host);
        // Rebuilds the rows from Str() and refreshes the version and the update state.
        void RefreshStrings();

    private:
        // Builds the update card; its status and buttons are kept as members for refreshUpdate.
        winrt::Microsoft::UI::Xaml::FrameworkElement buildUpdateCard();
        // Starts the check when the page appears, and subscribes to the checker's changes.
        void startChecking();
        // Ends the subscription when the page goes away. The alive flag stops late callbacks.
        void stopChecking();
        // Shows the checker state in the status row and the buttons. UI thread only.
        void refreshUpdate();
        void setVersionText();

        telaviva::host::ISettingsHost* host_ = nullptr;
        // Set false before the subscription ends, so a queued refresh does nothing.
        std::shared_ptr<bool> alive_;

        winrt::Microsoft::UI::Xaml::Controls::FontIcon statusIcon_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ProgressRing statusRing_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel actionsPanel_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button checkButton_{ nullptr };
        // Present only while an update is available.
        winrt::Microsoft::UI::Xaml::Controls::Button openButton_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct AboutPage : AboutPageT<AboutPage, implementation::AboutPage>
    {
    };
}
