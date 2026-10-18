// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "AboutPage.xaml.h"
#include "AdvancedPage.xaml.h"
#include "AutomaticBrightnessPage.xaml.h"
#include "GeneralPage.xaml.h"
#include "KeyboardPage.xaml.h"
#include "Host/Strings.h"
#include "microsoft.ui.xaml.window.h"

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <string_view>

#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

namespace winrt
{
    using namespace Microsoft::UI::Xaml;
}

namespace winrt::TelaViva::implementation
{
    namespace {
        // Sizes in DIP (effective pixels); the default is scaled to the target monitor's DPI.
        constexpr int kDefaultWidthDip = 880;
        constexpr int kDefaultHeightDip = 640;
        constexpr int kMinWidthDip = 640;
        constexpr int kMinHeightDip = 480;

        // Resource 1 of TelaViva.exe is the application icon (TelaViva.rc).
        constexpr WORD kAppIconResourceId = 1;

        HWND windowHandle(winrt::Microsoft::UI::Xaml::Window const& window)
        {
            HWND hwnd{};
            window.as<::IWindowNative>()->get_WindowHandle(&hwnd);
            return hwnd;
        }

        winrt::hstring localized(std::string_view key)
        {
            return winrt::hstring{ telaviva::host::Str(key) };
        }
    }

    SettingsWindow::SettingsWindow()
    {
        InitializeComponent();

        // The title bar is part of the content, so the Mica backdrop shows through it, as in Windows Settings.
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());
        // Mica is dropped in high contrast, where the system window color keeps text legible.
        bool highContrast = false;
        try {
            highContrast = winrt::Windows::UI::ViewManagement::AccessibilitySettings{}.HighContrast();
        }
        catch (const winrt::hresult_error&) {
            // Assume the normal theme when the setting cannot be read.
        }
        if (highContrast) {
            SystemBackdrop(nullptr);
        }
        else {
            SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
        }

        // The default presenter keeps the normal resizable frame and caption buttons.
        auto appWindow = AppWindow();
        appWindow.SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::Default);
        if (auto presenter = appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            presenter.PreferredMinimumWidth(winrt::Windows::Foundation::IReference<int32_t>{ kMinWidthDip });
            presenter.PreferredMinimumHeight(winrt::Windows::Foundation::IReference<int32_t>{ kMinHeightDip });
        }

        // AppWindow.SetIcon takes the IconId from the HICON, which keeps the title bar and taskbar on the app icon.
        if (HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconResourceId)))
        {
            appWindow.SetIcon(winrt::Microsoft::UI::GetIconIdFromIcon(icon));
        }

        general_ = winrt::make<winrt::TelaViva::implementation::GeneralPage>();
        keyboard_ = winrt::make<winrt::TelaViva::implementation::KeyboardPage>();
        ambient_ = winrt::make<winrt::TelaViva::implementation::AutomaticBrightnessPage>();
        advanced_ = winrt::make<winrt::TelaViva::implementation::AdvancedPage>();
        about_ =winrt::make<winrt::TelaViva::implementation::AboutPage>();

        Nav().SelectionChanged([this](auto const&, auto const& args)
            {
                const auto item = args.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>();
                if (!item)
                {
                    return;
                }
                if (item == GeneralItem())
                {
                    Nav().Content(general_);
                }
                else if (item == KeyboardItem())
                {
                    Nav().Content(keyboard_);
                }
                else if (item == AmbientItem())
                {
                    Nav().Content(ambient_);
                }
                else if (item == AdvancedItem())
                {
                    Nav().Content(advanced_);
                }
                else if (item == AboutItem())
                {
                    Nav().Content(about_);
                }
            });

        // Closing the window ends an active calibration, which restores the Laptop and monitor levels.
        Closed([this](auto const&, auto const&)
            {
                winrt::get_self<implementation::AutomaticBrightnessPage>(ambient_)->EndCalibration(telaviva::CalibrationEndReason::WindowClosed);
            });

        RefreshStrings();
        Nav().SelectedItem(GeneralItem());
    }

    void SettingsWindow::SetHost(telaviva::host::ISettingsHost* host)
    {
        winrt::get_self<implementation::GeneralPage>(general_)->SetHost(host);
        winrt::get_self<implementation::KeyboardPage>(keyboard_)->SetHost(host);
        winrt::get_self<implementation::AutomaticBrightnessPage>(ambient_)->SetHost(host);
        winrt::get_self<implementation::AdvancedPage>(advanced_)->SetHost(host);
        winrt::get_self<implementation::AboutPage>(about_)->SetHost(host);
    }

    void SettingsWindow::SetOnLanguageChanged(std::function<void()> callback)
    {
        winrt::get_self<implementation::GeneralPage>(general_)->SetOnLanguageChanged(std::move(callback));
    }

    void SettingsWindow::RefreshStrings()
    {
        const winrt::hstring title = localized("settings.title");
        Title(title);
        AppTitleBar().Title(title);
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(Nav(), title);

        GeneralItem().Content(winrt::box_value(localized("settings.tab.general")));
        KeyboardItem().Content(winrt::box_value(localized("settings.tab.keyboard")));
        AmbientItem().Content(winrt::box_value(localized("settings.tab.ambient")));
        AdvancedItem().Content(winrt::box_value(localized("settings.tab.advanced")));
        AboutItem().Content(winrt::box_value(localized("settings.tab.about")));

        winrt::get_self<implementation::GeneralPage>(general_)->RefreshStrings();
        winrt::get_self<implementation::KeyboardPage>(keyboard_)->RefreshStrings();
        winrt::get_self<implementation::AutomaticBrightnessPage>(ambient_)->RefreshStrings();
        winrt::get_self<implementation::AdvancedPage>(advanced_)->RefreshStrings();
        winrt::get_self<implementation::AboutPage>(about_)->RefreshStrings();
    }

    void SettingsWindow::SelectPage(SettingsPage page)
    {
        winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem target{ nullptr };
        switch (page)
        {
        case SettingsPage::General: target = GeneralItem(); break;
        case SettingsPage::Keyboard: target = KeyboardItem(); break;
        case SettingsPage::Ambient: target = AmbientItem(); break;
        case SettingsPage::Advanced: target = AdvancedItem(); break;
        case SettingsPage::About: target = AboutItem(); break;
        }

        const auto selected = Nav().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>();
        if (selected == target)
        {
            return;
        }
        Nav().SelectedItem(target);
    }

    void SettingsWindow::ShowOnMonitorOf(RECT anchorRect)
    {
        placeOnMonitorOf(anchorRect, true);
    }

    void SettingsWindow::CenterOnMonitorOf(RECT anchorRect)
    {
        placeOnMonitorOf(anchorRect, false);
    }

    void SettingsWindow::placeOnMonitorOf(RECT anchorRect, bool resetSize)
    {
        Window window = *this;
        const HWND hwnd = windowHandle(window);

        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST), &info);
        const RECT& work = info.rcWork;

        // Move onto the target monitor first, so GetDpiForWindow reports that monitor's scale.
        auto appWindow = AppWindow();
        appWindow.Move(winrt::Windows::Graphics::PointInt32{ work.left, work.top });
        const int dpi = static_cast<int>(GetDpiForWindow(hwnd));

        const int workWidth = work.right - work.left;
        const int workHeight = work.bottom - work.top;
        // A re-open keeps the size the user gave the window, so only its position changes.
        int width = MulDiv(kDefaultWidthDip, dpi, 96);
        int height = MulDiv(kDefaultHeightDip, dpi, 96);
        if (!resetSize) {
            const auto current = appWindow.Size();
            width = current.Width;
            height = current.Height;
        }
        width = std::min(width, workWidth);
        height = std::min(height, workHeight);
        const int left = work.left + (workWidth - width) / 2;
        const int top = work.top + (workHeight - height) / 2;
        appWindow.MoveAndResize(winrt::Windows::Graphics::RectInt32{ left, top, width, height });
    }
}
