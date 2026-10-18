// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "App.xaml.g.h"
#include "pch.h"
#include <winrt/TelaViva.h>

#include "Host/AppController.h"
#include "Host/AppLifecycle.h"
#include "Host/FlyoutState.h"
#include "Host/PlatformWindow.h"
#include "Host/TrayHost.h"
#include "SettingsWindow.xaml.h"

#include <memory>
#include <optional>

namespace winrt::TelaViva::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        // Created on first use and kept hidden between uses.
        void showFlyout();
        // Created on first use and kept hidden between uses; the flyout hides when it shows.
        void showTrayMenu(POINT anchor, bool fromKeyboard);
        // Created on demand and released when closed; a second open re-centers and activates the existing window.
        // A requested page is selected in both cases.
        void openSettings(std::optional<SettingsPage> page = std::nullopt);
        // Reports a window's show or hide to the controller, once per transition (Efficiency mode).
        void reportVisibility(bool& shown, bool visible);

        // Members are destroyed in reverse order: the tray icon is removed first, then the
        // lifecycle drains, then the controller shuts down, then the window is destroyed,
        // and the instance mutex goes last.
        std::unique_ptr<::telaviva::host::SingleInstance> singleInstance_;
        std::unique_ptr<::telaviva::host::PlatformWindow> platformWindow_;
        std::unique_ptr<::telaviva::host::AppController> controller_;
        std::unique_ptr<::telaviva::host::AppLifecycle> lifecycle_;
        std::unique_ptr<::telaviva::host::TrayHost> trayHost_;
        winrt::TelaViva::PopupWindow popup_{ nullptr };
        winrt::TelaViva::TrayMenuWindow trayMenu_{ nullptr };
        winrt::TelaViva::SettingsWindow settings_{ nullptr };
        // Captured on the UI thread so the controller's bridge-thread callbacks can post back.
        winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        // Latest state from the controller; the popup receives it on creation.
        ::telaviva::host::FlyoutState flyoutState_;
        // Last visibility reported to the controller for each window.
        bool popupShown_ = false;
        bool trayMenuShown_ = false;
        bool settingsShown_ = false;
    };
}
