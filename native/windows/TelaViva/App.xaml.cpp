// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "App.xaml.h"
#include "PopupWindow.xaml.h"
#include "SettingsWindow.xaml.h"
#include "TrayMenuWindow.xaml.h"
#include "Host/FlyoutState.h"
#include "Host/Strings.h"
#include "Host/TrayHost.h"
#include "Platform/AppPaths.h"
#include "Platform/Diagnostics.h"
#include "microsoft.ui.xaml.window.h"

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Graphics.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string_view>

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Microsoft::UI::Xaml;
}

namespace {

// Gap between the flyout and the screen or taskbar edge, in DIP.
constexpr int kFlyoutMarginDip = 12;

HWND windowHandle(winrt::Microsoft::UI::Xaml::Window const& window)
{
    HWND hwnd{};
    window.as<::IWindowNative>()->get_WindowHandle(&hwnd);
    return hwnd;
}

// Places the flyout beside the tray icon, on the side of the work area where the taskbar sits.
// An empty anchor (the shell could not report the icon) falls back to the primary work area's corner.
POINT flyoutPosition(RECT const& anchor, int width, int height, UINT dpi)
{
    const int margin = MulDiv(kFlyoutMarginDip, static_cast<int>(dpi), 96);

    MONITORINFO info{ sizeof(info) };
    if (IsRectEmpty(&anchor))
    {
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &info);
        const RECT& work = info.rcWork;
        return POINT{ work.right - width - margin, work.bottom - height - margin };
    }

    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &info);
    const RECT& work = info.rcWork;

    // Clamp against the work area; max-then-min keeps the result defined on tiny work areas.
    const auto fit = [](int value, int low, int high) { return std::max(low, std::min(value, high)); };
    const int alignedX = fit(anchor.right - width, work.left, work.right - width);
    const int alignedY = fit(anchor.bottom - height, work.top, work.bottom - height);

    // The taskbar is the part of the screen the icon sits in that lies outside the work area.
    if (anchor.top >= work.bottom)
    {
        return POINT{ alignedX, work.bottom - height - margin };
    }
    if (anchor.bottom <= work.top)
    {
        return POINT{ alignedX, work.top + margin };
    }
    if (anchor.left >= work.right)
    {
        return POINT{ work.right - width - margin, alignedY };
    }
    if (anchor.right <= work.left)
    {
        return POINT{ work.left + margin, alignedY };
    }
    // Icon inside the work area (auto-hidden taskbar): treat the taskbar as bottom.
    return POINT{ alignedX, work.bottom - height - margin };
}

// The tray has three icons. Protection off and an unknown presence both read as no face, as on macOS.
telaviva::host::TrayState trayStateFor(telaviva::host::PresenceIcon presence)
{
    switch (presence) {
    case telaviva::host::PresenceIcon::Detected: return telaviva::host::TrayState::Detected;
    case telaviva::host::PresenceIcon::LowLight: return telaviva::host::TrayState::LowLight;
    case telaviva::host::PresenceIcon::Absent:
    case telaviva::host::PresenceIcon::Off: break;
    }
    return telaviva::host::TrayState::Absent;
}

}  // namespace

namespace winrt::TelaViva::implementation
{
    App::App()
    {
        InitializeComponent();

        // Registered in Release too: the XAML fail-fast otherwise ends the process with nothing logged.
        // The exception is left unhandled, so the process still fails fast after the line is written.
        UnhandledException([](winrt::IInspectable const&, winrt::UnhandledExceptionEventArgs const& e)
            {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
                if (IsDebuggerPresent())
                {
                    __debugbreak();
                }
#endif
                // A standalone logger: AppController's instance may not exist yet, or may already be gone.
                // Diagnostics writes and closes the file per line, so nothing is left buffered here.
                try
                {
                    ::telaviva::Diagnostics diagnostics{ ::telaviva::AppPaths().logFile() };
                    char code[16]{};
                    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned int>(e.Exception()));
                    diagnostics.log(::telaviva::LogLevel::Error,
                        "unhandled XAML exception: " + winrt::to_string(e.Message()) + " (HRESULT " + code + ")");
                }
                catch (...)
                {
                    // The handler must not throw; the process fails fast regardless.
                }
            });
    }

    void App::OnLaunched(winrt::LaunchActivatedEventArgs const&)
    {
        // Smoke-test switch: the process exits with code 0 right after launch.
        std::wstring_view commandLine{ GetCommandLineW() };
        if (commandLine.find(L"--exit-immediately") != std::wstring_view::npos)
        {
            Microsoft::UI::Xaml::Application::Current().Exit();
            return;
        }

        using ::telaviva::host::AppController;
        using ::telaviva::host::AppLifecycle;
        using ::telaviva::host::FlyoutState;
        using ::telaviva::host::PlatformWindow;
        using ::telaviva::host::SingleInstance;
        using ::telaviva::host::TrayCallbacks;
        using ::telaviva::host::TrayHost;
        using ::telaviva::host::TrayState;
        using ::telaviva::host::TrayStrings;

        // The instance check runs first so a second launch never builds a window.
        singleInstance_ = std::make_unique<SingleInstance>();
        if (!singleInstance_->isFirst())
        {
            SingleInstance::notifyFirstInstance();
            Microsoft::UI::Xaml::Application::Current().Exit();
            return;
        }

        platformWindow_ = std::make_unique<PlatformWindow>();
        platformWindow_->addHandler([](UINT message, WPARAM, LPARAM) -> std::optional<LRESULT>
            {
                if (message != SingleInstance::activateMessage())
                {
                    return std::nullopt;
                }
                OutputDebugStringW(L"TelaViva: second launch asked for the flyout\n");
                return LRESULT{ 0 };
            });

        // These callbacks read lifecycle_ only from the message loop, which starts after this function.
        const auto quit = [this] { lifecycle_->requestQuit(); };

        TrayCallbacks callbacks;
        callbacks.onOpenFlyout = [this] { showFlyout(); };
        callbacks.onContextMenu = [this](POINT anchor, bool fromKeyboard) { showTrayMenu(anchor, fromKeyboard); };

        trayHost_ = std::make_unique<TrayHost>(*platformWindow_, std::move(callbacks), [] {
            TrayStrings strings;
            strings.tooltipDetected = ::telaviva::host::Str("tray.tooltip.detected");
            strings.tooltipAbsent = ::telaviva::host::Str("tray.tooltip.absent");
            strings.tooltipLowLight = ::telaviva::host::Str("tray.tooltip.lowlight");
            return strings;
        });
        trayHost_->setState(TrayState::Absent);

        // The controller reports from the core's thread, so it gets a copy of the queue captured here.
        dispatcher_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        controller_ = std::make_unique<AppController>(
            *platformWindow_,
            [this, dispatcher = dispatcher_](FlyoutState state) {
                dispatcher.TryEnqueue([this, state = std::move(state)] {
                    // Set only when presence changes, since each set repaints the tray icon.
                    if (trayHost_ && state.presence != flyoutState_.presence)
                    {
                        trayHost_->setState(trayStateFor(state.presence));
                    }
                    flyoutState_ = state;
                    if (popup_)
                    {
                        winrt::get_self<winrt::TelaViva::implementation::PopupWindow>(popup_)->UpdateState(flyoutState_);
                    }
                });
            },
            [this] { openSettings(); },
            quit);
        controller_->start();

        // The lifecycle drains the core through the controller's bridge. Its exit callback runs
        // on the UI thread after the drain, so the controller can stop its sources before exit.
        lifecycle_ = std::make_unique<AppLifecycle>(
            controller_->bridge(),
            [this] {
                controller_->shutdown();
                Microsoft::UI::Xaml::Application::Current().Exit();
            },
            platformWindow_.get());
        platformWindow_->addHandler([lifecycle = lifecycle_.get()](UINT message, WPARAM wParam, LPARAM lParam)
            {
                return lifecycle->onSessionMessage(message, wParam, lParam);
            });

        // Screenshot aid for the flyout prototype; the icon may not be placed yet, so the fallback corner applies.
        if (commandLine.find(L"--show-flyout") != std::wstring_view::npos)
        {
            showFlyout();
        }

        // Screenshot aid for the Settings window.
        if (commandLine.find(L"--show-settings") != std::wstring_view::npos)
        {
            openSettings();
        }
    }

    void App::showFlyout()
    {
        if (!popup_)
        {
            popup_ = winrt::make<winrt::TelaViva::implementation::PopupWindow>();
            // The popup gets its actions and the latest state once, when it is created.
            auto popup = winrt::get_self<winrt::TelaViva::implementation::PopupWindow>(popup_);
            popup->SetActions(controller_.get());
            popup->UpdateState(flyoutState_);
            // AppWindow reports every show and hide, including light dismiss and Escape, so the
            // controller hears about the flyout however it closes.
            popup_.AppWindow().Changed([this](auto const& window, auto const& args) {
                if (args.DidVisibilityChange()) {
                    const bool visible = window.IsVisible();
                    const bool wasShown = popupShown_;
                    reportVisibility(popupShown_, visible);
                    // Flush a protection brightness still waiting for its trailing timer, once per hide.
                    if (wasShown && !visible && controller_) {
                        controller_->onFlyoutHidden();
                    }
                }
            });
        }

        const HWND hwnd = windowHandle(popup_);
        const UINT dpi = GetDpiForWindow(hwnd);
        const auto appWindow = popup_.AppWindow();
        const auto size = appWindow.Size();
        const RECT anchor = trayHost_ ? trayHost_->iconRect() : RECT{};
        const POINT position = flyoutPosition(anchor, size.Width, size.Height, dpi);

        appWindow.Move(Windows::Graphics::PointInt32{ position.x, position.y });
        appWindow.Show();
        // Activation makes the window eligible for Deactivated, which hides it on click-away.
        popup_.Activate();
        SetForegroundWindow(hwnd);
    }

    void App::showTrayMenu(POINT anchor, bool fromKeyboard)
    {
        // The flyout is also a light-dismiss surface, so only one of the two is shown at a time.
        if (popup_)
        {
            popup_.AppWindow().Hide();
        }

        if (!trayMenu_)
        {
            trayMenu_ = winrt::make<winrt::TelaViva::implementation::TrayMenuWindow>();
            winrt::get_self<winrt::TelaViva::implementation::TrayMenuWindow>(trayMenu_)->SetActions(
                [this] { openSettings(); },
                [this] { lifecycle_->requestQuit(); });
            // Same visibility reporting as the flyout: AppWindow sees every show and hide.
            trayMenu_.AppWindow().Changed([this](auto const& window, auto const& args) {
                if (args.DidVisibilityChange()) {
                    reportVisibility(trayMenuShown_, window.IsVisible());
                }
            });
        }

        winrt::get_self<winrt::TelaViva::implementation::TrayMenuWindow>(trayMenu_)->ShowAt(anchor, fromKeyboard);
    }

    void App::openSettings()
    {
        // The flyout is a light-dismiss surface, so it closes when Settings opens.
        if (popup_)
        {
            popup_.AppWindow().Hide();
        }

        // Every open centers on the monitor of the tray icon; the cursor stands in when the shell cannot report it.
        RECT anchor = trayHost_ ? trayHost_->iconRect() : RECT{};
        if (IsRectEmpty(&anchor))
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            anchor = RECT{ cursor.x, cursor.y, cursor.x + 1, cursor.y + 1 };
        }

        if (settings_)
        {
            // A minimized window is restored first, so the re-center and the activation are visible.
            const HWND hwnd = windowHandle(settings_);
            if (IsIconic(hwnd))
            {
                ShowWindow(hwnd, SW_RESTORE);
            }
            winrt::get_self<winrt::TelaViva::implementation::SettingsWindow>(settings_)->CenterOnMonitorOf(anchor);
            settings_.Activate();
            return;
        }

        settings_ = winrt::make<winrt::TelaViva::implementation::SettingsWindow>();
        auto settings = winrt::get_self<winrt::TelaViva::implementation::SettingsWindow>(settings_);
        settings->SetHost(controller_->settingsHost());
        // Every surface reads Str(), so a language change re-renders the Settings window, the flyout and the tray.
        // The page raises this on the UI thread after its change handler returns.
        settings->SetOnLanguageChanged([this] {
            if (settings_)
            {
                winrt::get_self<winrt::TelaViva::implementation::SettingsWindow>(settings_)->RefreshStrings();
            }
            // The popup has no RefreshStrings: UpdateState re-applies its texts from the cached state.
            if (popup_)
            {
                winrt::get_self<winrt::TelaViva::implementation::PopupWindow>(popup_)->UpdateState(flyoutState_);
            }
            if (trayHost_)
            {
                trayHost_->refreshStrings();
            }
        });
        settings->ShowOnMonitorOf(anchor);

        // The release is queued: the event fires inside the window that is being released.
        settings_.Closed([this](auto const&, auto const&) {
            reportVisibility(settingsShown_, false);
            dispatcher_.TryEnqueue([this] { settings_ = nullptr; });
        });
        settings_.Activate();
        reportVisibility(settingsShown_, true);
    }

    void App::reportVisibility(bool& shown, bool visible)
    {
        if (shown == visible || !controller_) {
            return;
        }
        shown = visible;
        controller_->onWindowVisibilityChanged(visible);
    }
}
