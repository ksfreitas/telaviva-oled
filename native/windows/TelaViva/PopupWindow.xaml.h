// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "PopupWindow.g.h"
#include "pch.h"
#include "Host/FlyoutState.h"
#include "Platform/PopupLayout.h"

#include <map>
#include <string>
#include <vector>

namespace winrt::TelaViva::implementation
{
    struct PopupWindow : PopupWindowT<PopupWindow>
    {
        PopupWindow();

        // Implementation-only: called on the UI thread.
        void SetActions(telaviva::host::IFlyoutActions* actions);
        void UpdateState(const telaviva::host::FlyoutState& state);

    private:
        // Measures the header, cards, notice and footer, then sizes the window and card list.
        void Relayout();
        // Both in DIP; converted to pixels with the window DPI.
        void ApplySize(double widthDip, double heightDip);

        HWND hwnd_{};
        telaviva::host::IFlyoutActions* actions_ = nullptr;
        // Privacy-settings link shown in the camera warning on denial. Created once in the constructor.
        winrt::Windows::Foundation::IInspectable cameraLink_{ nullptr };
        // Monitor id to MonitorCard, kept across updates so cards are reused.
        std::map<std::string, winrt::Windows::Foundation::IInspectable> cards_;
        // Layout inputs at the last Relayout; a snapshot that leaves them unchanged skips the measure.
        std::vector<size_t> layoutKey_;
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct PopupWindow : PopupWindowT<PopupWindow, implementation::PopupWindow>
    {
    };
}
