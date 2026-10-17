// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "SettingsWindow.g.h"
#include "pch.h"
#include <winrt/TelaViva.h>

#include "Host/SettingsHost.h"

#include <functional>

namespace winrt::TelaViva::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        SettingsWindow();

        // Implementation-only: called on the UI thread.
        void SetHost(telaviva::host::ISettingsHost* host);
        // Re-applies the window title, nav labels and every page text from Str().
        void RefreshStrings();
        // Forwarded to the General page, which raises it after a language change.
        void SetOnLanguageChanged(std::function<void()> callback);
        // Sizes the window at the default size and centers it on the monitor holding anchorRect.
        void ShowOnMonitorOf(RECT anchorRect);
        // Centers the window on the monitor holding anchorRect, keeping the size the user gave it.
        // anchorRect is the tray icon rect (or the cursor when that is unavailable).
        void CenterOnMonitorOf(RECT anchorRect);

    private:
        void placeOnMonitorOf(RECT anchorRect, bool resetSize);

        winrt::TelaViva::GeneralPage general_{ nullptr };
        winrt::TelaViva::KeyboardPage keyboard_{ nullptr };
        winrt::TelaViva::AutomaticBrightnessPage ambient_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow>
    {
    };
}
