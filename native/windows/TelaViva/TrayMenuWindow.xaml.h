// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "TrayMenuWindow.g.h"
#include "pch.h"

#include <functional>

namespace winrt::TelaViva::implementation
{
    struct TrayMenuWindow : TrayMenuWindowT<TrayMenuWindow>
    {
        TrayMenuWindow();

        // Implementation-only: called on the UI thread. Every action runs after the menu has hidden.
        void SetActions(std::function<void()> openSettings, std::function<void()> openAbout, std::function<void()> quit);
        // Implementation-only: anchor is in screen pixels. fromKeyboard moves focus to the first item.
        void ShowAt(POINT anchor, bool fromKeyboard);

    private:
        // Sets the item texts and automation names from Str(key), so a language change applies on the next show.
        void applyStrings();

        HWND hwnd_{};
        std::function<void()> openSettings_;
        std::function<void()> openAbout_;
        std::function<void()> quit_;
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct TrayMenuWindow : TrayMenuWindowT<TrayMenuWindow, implementation::TrayMenuWindow>
    {
    };
}
