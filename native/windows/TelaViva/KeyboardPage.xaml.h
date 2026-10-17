// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "KeyboardPage.g.h"
#include "pch.h"
#include "Host/SettingsHost.h"
#include "Platform/SettingsModel.h"

#include <cstdint>
#include <functional>

namespace winrt::TelaViva::implementation
{
    struct KeyboardPage : KeyboardPageT<KeyboardPage>
    {
        KeyboardPage();

        // Implementation-only: not part of the IDL, reached through get_self.
        void SetHost(telaviva::host::ISettingsHost* host);
        // Rebuilds the rows from Str() and keeps the current settings.
        void RefreshStrings();

    private:
        // Applies one change to a fresh draft of the current settings and submits the full payload.
        void edit(const std::function<void(telaviva::SettingsDraft&)>& change);
        // Starts the one-second poll that picks up settings changed elsewhere.
        void startPolling();
        void onPoll();
        // Writes the settings into the controls without submitting them.
        void syncControls(const telaviva::SettingsSnapshot& settings);
        // Sets the status line under the keys toggle from the hook state. Polled, since the hook state moves on its own.
        void updateKeysStatus();

        telaviva::host::ISettingsHost* host_ = nullptr;
        // Snapshot revision the controls last matched; 0 until the first snapshot has arrived.
        std::uint64_t revision_ = 0;
        // Set while a control is written in code, so the write is not submitted.
        bool suppress_ = false;

        winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch keysToggle_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock keysStatus_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox volumeBox_{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer poll_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct KeyboardPage : KeyboardPageT<KeyboardPage, implementation::KeyboardPage>
    {
    };
}
