// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "AdvancedPage.g.h"
#include "pch.h"
#include "Host/SettingsHost.h"
#include "Platform/DeviceSelection.h"
#include "Platform/SettingsModel.h"

#include <functional>
#include <string>
#include <vector>

namespace winrt::TelaViva::implementation
{
    struct AdvancedPage : AdvancedPageT<AdvancedPage>
    {
        AdvancedPage();

        // Implementation-only: not part of the IDL, reached through get_self.
        void SetHost(telaviva::host::ISettingsHost* host);
        // Rebuilds the rows from Str() and refills the device list from the stored choice.
        void RefreshStrings();

    private:
        // Applies one change to a fresh draft of the current settings and submits the full payload.
        void edit(const std::function<void(telaviva::SettingsDraft&)>& change);
        // Refills the device picker and selects the stored choice. Runs on show, host set and language change, not on the poll.
        void fillDeviceBox();
        // Starts the one-second poll that updates the in-use line and the notice.
        void startPolling();
        void updateReport();

        telaviva::host::ISettingsHost* host_ = nullptr;
        // Set while the picker is written in code, so the write is not submitted.
        bool suppress_ = false;
        // Id of each picker item in item order. A stored id missing from the device list is the last item.
        std::vector<std::string> deviceIds_;

        winrt::Microsoft::UI::Xaml::Controls::ComboBox deviceBox_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock inUseText_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::InfoBar noticeBar_{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer poll_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct AdvancedPage : AdvancedPageT<AdvancedPage, implementation::AdvancedPage>
    {
    };
}
