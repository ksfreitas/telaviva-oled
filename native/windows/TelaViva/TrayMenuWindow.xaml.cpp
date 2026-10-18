// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "TrayMenuWindow.xaml.h"
#include "Host/Strings.h"
#include "microsoft.ui.xaml.window.h"

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#if __has_include("TrayMenuWindow.g.cpp")
#include "TrayMenuWindow.g.cpp"
#endif

namespace winrt
{
    using namespace Microsoft::UI::Xaml;
}

namespace winrt::TelaViva::implementation
{
    namespace {
        // The DWM enums are referenced by value: the SDK headers gate them behind newer NTDDI versions.
        constexpr DWORD kDwmwaWindowCornerPreference = 33;
        constexpr DWORD kDwmwcpRound = 2;
        constexpr DWORD kDwmwaBorderColor = 34;
        constexpr DWORD kDwmwaColorNone = 0xFFFFFFFE;

        // Gap between the menu and the screen or taskbar edge, in DIP.
        constexpr int kMenuMarginDip = 4;

        winrt::hstring localized(std::string_view key)
        {
            return winrt::hstring{ telaviva::host::Str(key) };
        }

        // Same rule as the flyout: the system theme decides the frame color.
        bool isDarkTheme()
        {
            try {
                winrt::Windows::UI::ViewManagement::UISettings settings;
                const auto background = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Background);
                const double luminance = 0.299 * background.R + 0.587 * background.G + 0.114 * background.B;
                return luminance < 128.0;
            }
            catch (const winrt::hresult_error&) {
                return true;
            }
        }

        // High contrast replaces the transparent backdrop and the frame color with system colors.
        bool isHighContrast()
        {
            try {
                return winrt::Windows::UI::ViewManagement::AccessibilitySettings{}.HighContrast();
            }
            catch (const winrt::hresult_error&) {
                return false;
            }
        }

        void applyBorderColor(HWND hwnd)
        {
            COLORREF color = isDarkTheme() ? RGB(58, 58, 58) : RGB(200, 200, 200);
            // DWM takes a COLORREF, not a theme resource, so high contrast uses the system frame color.
            if (isHighContrast()) {
                color = GetSysColor(COLOR_WINDOWFRAME);
            }
            // Fall back to no border when the attribute is rejected, rather than keep the bright default.
            if (FAILED(DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &color, sizeof(color)))) {
                DWORD none = kDwmwaColorNone;
                DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &none, sizeof(none));
            }
        }

        // Pre-Windows 11 builds ignore these attributes, so the failures are not actionable.
        void applyFrameAttributes(HWND hwnd)
        {
            DWORD corner = kDwmwcpRound;
            DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
            applyBorderColor(hwnd);
        }

        // Places the menu above the cursor on a bottom taskbar, and beside it on a side taskbar.
        // The taskbar edge is the side where the work area is inset from the monitor; with no
        // inset (auto-hidden taskbar) the bottom applies. The result is clamped to the work area.
        POINT menuPosition(POINT anchor, int width, int height, UINT dpi)
        {
            MONITORINFO info{ sizeof(info) };
            if (!GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &info)) {
                return anchor;
            }
            const RECT& work = info.rcWork;
            const RECT& monitor = info.rcMonitor;
            const int margin = MulDiv(kMenuMarginDip, static_cast<int>(dpi), 96);

            // Max-then-min keeps the result defined on work areas smaller than the menu.
            const auto fit = [](LONG value, LONG low, LONG high) { return std::max(low, std::min(value, high)); };

            const bool bottomTaskbar = work.bottom < monitor.bottom;
            const bool topTaskbar = !bottomTaskbar && work.top > monitor.top;
            const bool rightTaskbar = !bottomTaskbar && !topTaskbar && work.right < monitor.right;
            const bool leftTaskbar = !bottomTaskbar && !topTaskbar && !rightTaskbar && work.left > monitor.left;

            // Bottom edge at the cursor or the taskbar top, whichever is higher; right edge at the cursor.
            LONG x = anchor.x - width;
            LONG y = std::min(anchor.y, work.bottom) - height - margin;
            if (topTaskbar) {
                y = std::max(anchor.y, work.top) + margin;
            }
            else if (leftTaskbar) {
                x = std::max(anchor.x, work.left) + margin;
            }
            else if (rightTaskbar) {
                x = std::min(anchor.x, work.right) - width - margin;
            }
            return POINT{ fit(x, work.left, work.right - width), fit(y, work.top, work.bottom - height) };
        }
    }

    TrayMenuWindow::TrayMenuWindow()
    {
        InitializeComponent();

        // Unpackaged WinUI gives the window its HWND only through IWindowNative.
        Window window = *this;
        window.as<::IWindowNative>()->get_WindowHandle(&hwnd_);

        auto appWindow = AppWindow();

        // Same chrome as the flyout: the context-menu presenter owns the frame, with no title bar.
        auto presenter = Microsoft::UI::Windowing::OverlappedPresenter::CreateForContextMenu();
        presenter.IsAlwaysOnTop(true);
        appWindow.SetPresenter(presenter);

        // The menu is not a window to switch to, and it has no taskbar button.
        appWindow.IsShownInSwitchers(false);
        appWindow.Title(localized("app"));

        // Acrylic is replaced by the system page brush in high contrast, where transparency hurts legibility.
        if (isHighContrast()) {
            SystemBackdrop(nullptr);
            // try_as: a missing resource leaves the default background rather than throwing at construction.
            if (auto brush = Microsoft::UI::Xaml::Application::Current().Resources()
                .Lookup(winrt::box_value(winrt::hstring{ L"ApplicationPageBackgroundThemeBrush" }))
                .try_as<Microsoft::UI::Xaml::Media::Brush>()) {
                MenuPanel().Background(brush);
            }
        }
        else {
            SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
        }

        applyStrings();

        SettingsItem().Click([this](auto const&, auto const&) {
            // Hidden first, so the action never runs while the menu is still on screen.
            AppWindow().Hide();
            if (openSettings_) {
                openSettings_();
            }
        });

        AboutItem().Click([this](auto const&, auto const&) {
            AppWindow().Hide();
            if (openAbout_) {
                openAbout_();
            }
        });

        QuitItem().Click([this](auto const&, auto const&) {
            AppWindow().Hide();
            if (quit_) {
                quit_();
            }
        });

        // Up and Down move between the rows in visual order; Tab is handled by the focus engine.
        MenuPanel().KeyDown([this](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
            const auto key = args.Key();
            if (key != winrt::Windows::System::VirtualKey::Up && key != winrt::Windows::System::VirtualKey::Down) {
                return;
            }
            const std::array<Microsoft::UI::Xaml::Controls::Button, 3> rows{ SettingsItem(), AboutItem(), QuitItem() };
            // Down moves to the next row and Up to the previous one, wrapping at either end. With no row
            // focused, Down takes the first row and Up the last.
            const bool down = key == winrt::Windows::System::VirtualKey::Down;
            size_t target = down ? 0 : rows.size() - 1;
            const auto source = args.OriginalSource().try_as<Microsoft::UI::Xaml::Controls::Button>();
            for (size_t i = 0; i < rows.size(); ++i) {
                if (source && source == rows[i]) {
                    target = down ? (i + 1) % rows.size() : (i + rows.size() - 1) % rows.size();
                    break;
                }
            }
            static_cast<void>(rows[target].Focus(Microsoft::UI::Xaml::FocusState::Keyboard));
            args.Handled(true);
        });

        // Escape is an accelerator on the root, so it closes the menu wherever focus is inside it.
        Microsoft::UI::Xaml::Input::KeyboardAccelerator escape;
        escape.Key(winrt::Windows::System::VirtualKey::Escape);
        escape.Invoked([this](auto const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args) {
            AppWindow().Hide();
            args.Handled(true);
        });
        MenuPanel().KeyboardAccelerators().Append(escape);

        // Light dismiss: hide as soon as another window takes activation.
        Activated([this](winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowActivatedEventArgs const& args)
            {
                if (args.WindowActivationState() == Microsoft::UI::Xaml::WindowActivationState::Deactivated)
                {
                    AppWindow().Hide();
                }
            });

        // DWM attributes are applied once the presenter is set, as in the flyout.
        applyFrameAttributes(hwnd_);
    }

    void TrayMenuWindow::SetActions(std::function<void()> openSettings, std::function<void()> openAbout, std::function<void()> quit)
    {
        openSettings_ = std::move(openSettings);
        openAbout_ = std::move(openAbout);
        quit_ = std::move(quit);
    }

    void TrayMenuWindow::applyStrings()
    {
        const winrt::hstring settingsText = localized("tray.menu.settings");
        SettingsText().Text(settingsText);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(SettingsItem(), settingsText);

        const winrt::hstring aboutText = localized("tray.menu.about");
        AboutText().Text(aboutText);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(AboutItem(), aboutText);

        const winrt::hstring quitText = localized("tray.menu.quit");
        QuitText().Text(quitText);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(QuitItem(), quitText);
    }

    void TrayMenuWindow::ShowAt(POINT anchor, bool fromKeyboard)
    {
        // Labels are re-read on each show, so a language change applies without a restart.
        applyStrings();

        // The menu takes the size of its content, measured at the current text scale.
        const float unbounded = std::numeric_limits<float>::infinity();
        MenuPanel().Measure(winrt::Windows::Foundation::Size{ unbounded, unbounded });
        const auto desired = MenuPanel().DesiredSize();

        const UINT dpi = GetDpiForWindow(hwnd_);
        const UINT effectiveDpi = dpi != 0 ? dpi : 96;
        const double scale = effectiveDpi / 96.0;
        const int32_t width = static_cast<int32_t>(std::lround(desired.Width * scale));
        const int32_t height = static_cast<int32_t>(std::lround(desired.Height * scale));
        const POINT position = menuPosition(anchor, width, height, effectiveDpi);

        auto appWindow = AppWindow();
        appWindow.MoveAndResize(winrt::Windows::Graphics::RectInt32{
            static_cast<int32_t>(position.x), static_cast<int32_t>(position.y), width, height });
        appWindow.Show();
        Activate();
        // Without foreground activation the menu does not dismiss on outside clicks.
        SetForegroundWindow(hwnd_);

        // Only a keyboard invocation moves focus into the menu; a mouse-opened menu shows no highlight.
        if (fromKeyboard) {
            static_cast<void>(SettingsItem().Focus(Microsoft::UI::Xaml::FocusState::Keyboard));
        }
    }
}
