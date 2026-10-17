// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "PopupWindow.xaml.h"
#include "MonitorCard.xaml.h"
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
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if __has_include("PopupWindow.g.cpp")
#include "PopupWindow.g.cpp"
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

        winrt::hstring localized(std::string_view key)
        {
            return winrt::hstring{ telaviva::host::Str(key) };
        }

        // The Windows flyout stroke is subtle; the bright default border was the S11 defect.
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

        // High contrast replaces the flyout's transparent backdrop and its frame color with system colors.
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

        // Width and the height cap only; header, cards, notice and footer are measured in Relayout.
        telaviva::PopupMetrics flyoutMetrics()
        {
            return telaviva::PopupMetrics{};
        }

        // Keep in step with the RootGrid Padding in PopupWindow.xaml.
        constexpr double kRootPadding = 12;
    }

    PopupWindow::PopupWindow()
    {
        InitializeComponent();

        // Unpackaged WinUI gives the window its HWND only through IWindowNative.
        Window window = *this;
        window.as<::IWindowNative>()->get_WindowHandle(&hwnd_);

        auto appWindow = AppWindow();

        // The context-menu presenter is the flyout presenter: it owns the window style, so the
        // frame is configured through it rather than by editing the HWND style directly.
        auto presenter = Microsoft::UI::Windowing::OverlappedPresenter::CreateForContextMenu();
        presenter.IsAlwaysOnTop(true);
        appWindow.SetPresenter(presenter);

        // IsShownInSwitchers exists in this Windows App SDK (AppWindow), so the tool-window style is not needed.
        appWindow.IsShownInSwitchers(false);

        // Acrylic is replaced by the system page brush in high contrast, where transparency hurts legibility.
        if (isHighContrast()) {
            SystemBackdrop(nullptr);
            // try_as: a missing resource leaves the default background rather than throwing at construction.
            if (auto brush = Microsoft::UI::Xaml::Application::Current().Resources()
                .Lookup(winrt::box_value(winrt::hstring{ L"ApplicationPageBackgroundThemeBrush" }))
                .try_as<Microsoft::UI::Xaml::Media::Brush>()) {
                RootGrid().Background(brush);
            }
        }
        else {
            SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
        }

        // Static strings are set here because the XAML carries no literal text (ADR 0015).
        EmptyText().Text(localized("noDisplays"));
        EmptyCaptionText().Text(localized("ddcUnavailable"));
        NoticeText().Text(localized("flyout.coreunavailable"));
        AppTitleText().Text(localized("app"));

        const winrt::hstring refreshText = localized("flyout.refresh");
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(RefreshButton(), winrt::box_value(refreshText));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(RefreshButton(), refreshText);
        RefreshButton().Click([this](auto const&, auto const&) {
            if (actions_ != nullptr) {
                actions_->refresh();
            }
        });

        const winrt::hstring settingsText = localized("flyout.button.settings");
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(SettingsButton(), winrt::box_value(settingsText));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(SettingsButton(), settingsText);
        SettingsButton().Click([this](auto const&, auto const&) {
            if (actions_ != nullptr) {
                actions_->openSettings();
            }
        });

        // Opens the Windows camera privacy page; the status line itself is set in UpdateState.
        const auto cameraLink = Microsoft::UI::Xaml::Controls::HyperlinkButton{};
        cameraLink.Content(winrt::box_value(localized("cameraPrivacyLink")));
        cameraLink.Click([](auto const&, auto const&) {
            static_cast<void>(winrt::Windows::System::Launcher::LaunchUriAsync(
                winrt::Windows::Foundation::Uri{ L"ms-settings:privacy-webcam" }));
        });
        cameraLink_ = cameraLink;

        const winrt::hstring quitText = localized("flyout.button.quit");
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(QuitButton(), winrt::box_value(quitText));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(QuitButton(), quitText);
        QuitButton().Click([this](auto const&, auto const&) {
            if (actions_ != nullptr) {
                actions_->quit();
            }
        });

        // Escape is an accelerator on the root, so it closes the flyout wherever focus is inside it.
        Microsoft::UI::Xaml::Input::KeyboardAccelerator escape;
        escape.Key(winrt::Windows::System::VirtualKey::Escape);
        escape.Invoked([this](auto const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args) {
            AppWindow().Hide();
            args.Handled(true);
        });
        RootGrid().KeyboardAccelerators().Append(escape);

        // Size for the empty state until the first UpdateState.
        Relayout();

        // Light dismiss: hide as soon as another window takes activation.
        Activated([this](winrt::Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowActivatedEventArgs const& args)
            {
                if (args.WindowActivationState() == Microsoft::UI::Xaml::WindowActivationState::Deactivated)
                {
                    AppWindow().Hide();
                }
            });

        // DWM attributes are applied once the presenter is set. No separate first-Show hook exists
        // in this class, so they are not re-applied after Show.
        applyFrameAttributes(hwnd_);
    }

    void PopupWindow::SetActions(telaviva::host::IFlyoutActions* actions)
    {
        actions_ = actions;
        for (auto& entry : cards_) {
            auto card = entry.second.as<winrt::TelaViva::MonitorCard>();
            winrt::get_self<implementation::MonitorCard>(card)->Bind(actions_);
        }
    }

    void PopupWindow::UpdateState(const telaviva::host::FlyoutState& state)
    {
        // The presence row hides entirely when there is no text, and while the core is down.
        const bool hasPresence = state.coreAvailable && !state.presenceText.empty();
        PresenceRow().Visibility(hasPresence ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed);
        if (hasPresence) {
            PresenceText().Text(winrt::hstring{ state.presenceText });
        }
        PresenceGlyph().Opacity(state.presence == telaviva::host::PresenceIcon::Off ? 0.5 : 1.0);

        // The footer camera line is always shown; the warning above the cards only while the core is up.
        CameraStatusText().Text(winrt::hstring{ state.cameraText });
        const bool showNotice = state.coreAvailable && state.cameraNotice.visible;
        CameraInfoBar().Message(winrt::hstring{ state.cameraNotice.text });
        CameraInfoBar().Content(state.cameraNotice.permission ? cameraLink_
            : winrt::Windows::Foundation::IInspectable{ nullptr });
        CameraInfoBar().IsOpen(showNotice);

        // Drop cards for monitors that are gone, detaching their elements from the panel.
        auto children = CardsPanel().Children();
        for (auto it = cards_.begin(); it != cards_.end();) {
            const bool present = std::any_of(state.monitors.begin(), state.monitors.end(),
                [&](const telaviva::host::MonitorCardState& monitor) { return monitor.id == it->first; });
            if (present) {
                ++it;
                continue;
            }
            uint32_t index = 0;
            if (children.IndexOf(it->second.as<Microsoft::UI::Xaml::UIElement>(), index)) {
                children.RemoveAt(index);
            }
            it = cards_.erase(it);
        }

        // Existing cards are updated in place, so a control being dragged keeps its pointer capture.
        std::vector<winrt::Windows::Foundation::IInspectable> ordered;
        ordered.reserve(state.monitors.size());
        for (const auto& monitor : state.monitors) {
            auto found = cards_.find(monitor.id);
            if (found == cards_.end()) {
                winrt::TelaViva::MonitorCard created;
                winrt::get_self<implementation::MonitorCard>(created)->Bind(actions_);
                found = cards_.emplace(monitor.id, created).first;
            }
            auto card = found->second.as<winrt::TelaViva::MonitorCard>();
            winrt::get_self<implementation::MonitorCard>(card)->Update(monitor);
            ordered.push_back(found->second);
        }

        // Reorder only when the sequence differs from the panel; then move just the elements out of place.
        bool sameOrder = children.Size() == ordered.size();
        for (uint32_t i = 0; sameOrder && i < ordered.size(); ++i) {
            winrt::Windows::Foundation::IInspectable current = children.GetAt(i);
            sameOrder = current == ordered[i];
        }
        if (!sameOrder) {
            for (uint32_t i = 0; i < ordered.size(); ++i) {
                if (i < children.Size()) {
                    winrt::Windows::Foundation::IInspectable current = children.GetAt(i);
                    if (current == ordered[i]) {
                        continue;
                    }
                }
                uint32_t from = 0;
                if (children.IndexOf(ordered[i].as<Microsoft::UI::Xaml::UIElement>(), from)) {
                    children.RemoveAt(from);
                }
                children.InsertAt(i, ordered[i].as<Microsoft::UI::Xaml::UIElement>());
            }
        }

        // While the core is down the notice replaces the cards and the empty text.
        const bool core = state.coreAvailable;
        const bool empty = state.monitors.empty();
        EmptyPanel().Visibility(core && empty ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed);
        CardsScroll().Visibility(core && !empty ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed);
        NoticePanel().Visibility(core ? Microsoft::UI::Xaml::Visibility::Collapsed : Microsoft::UI::Xaml::Visibility::Visible);

        // Relayout only when something it measures changed: section visibility, the header and footer
        // texts, and each card's rows and texts. A plain slider change leaves the window size alone.
        std::vector<size_t> layoutKey;
        const std::hash<std::wstring> hash;
        auto flag = [&layoutKey](bool on) { layoutKey.push_back(on ? size_t{ 1 } : size_t{ 0 }); };
        flag(CardsScroll().Visibility() == Microsoft::UI::Xaml::Visibility::Visible);
        flag(EmptyPanel().Visibility() == Microsoft::UI::Xaml::Visibility::Visible);
        flag(NoticePanel().Visibility() == Microsoft::UI::Xaml::Visibility::Visible);
        flag(PresenceRow().Visibility() == Microsoft::UI::Xaml::Visibility::Visible);
        flag(CameraInfoBar().IsOpen());
        layoutKey.push_back(hash(state.presenceText));
        layoutKey.push_back(hash(state.cameraText));
        layoutKey.push_back(hash(state.cameraNotice.text));
        for (const auto& entry : ordered) {
            auto card = entry.as<winrt::TelaViva::MonitorCard>();
            layoutKey.push_back(winrt::get_self<implementation::MonitorCard>(card)->LayoutSignature());
        }
        // An empty key on the first call makes this differ, so the initial size is always applied.
        if (layoutKey != layoutKey_) {
            layoutKey_ = std::move(layoutKey);
            Relayout();
        }
    }

    void PopupWindow::Relayout()
    {
        const UINT dpi = GetDpiForWindow(hwnd_);
        const double scale = (dpi != 0 ? dpi : 96) / 96.0;
        const telaviva::PopupMetrics metrics = flyoutMetrics();

        // Measured at the width the RootGrid gives the content, with no height limit, so the
        // desired height includes text scale and wrapping.
        const double contentWidth = metrics.width - 2 * kRootPadding;
        const winrt::Windows::Foundation::Size available{
            static_cast<float>(contentWidth), std::numeric_limits<float>::infinity() };
        auto measure = [&](Microsoft::UI::Xaml::UIElement const& element) {
            element.Measure(available);
            return std::max(0.0, static_cast<double>(element.DesiredSize().Height));
        };

        // The card list, or the empty/notice panel that takes its place, is the one viewport row.
        std::vector<double> rows;
        if (CardsScroll().Visibility() == Microsoft::UI::Xaml::Visibility::Visible) {
            for (auto const& child : CardsPanel().Children()) {
                rows.push_back(measure(child));
            }
        }
        else if (EmptyPanel().Visibility() == Microsoft::UI::Xaml::Visibility::Visible) {
            rows.push_back(measure(EmptyPanel()));
        }
        else if (NoticePanel().Visibility() == Microsoft::UI::Xaml::Visibility::Visible) {
            rows.push_back(measure(NoticePanel()));
        }

        // The camera warning sits above the list and adds its own top margin.
        double notice = 0;
        if (CameraInfoBar().IsOpen()) {
            notice = CameraInfoBar().Margin().Top + measure(CameraInfoBar());
        }

        const double header = measure(HeaderRow());
        const double footer = measure(FooterPanel());

        // The work area is in pixels; the layout works in DIP.
        double workAreaHeight = std::numeric_limits<double>::infinity();
        MONITORINFO info{ sizeof(info) };
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info)) {
            workAreaHeight = (info.rcWork.bottom - info.rcWork.top) / scale;
        }

        const telaviva::PopupHeight layout = telaviva::computePopupHeight(rows, header, footer, notice,
            metrics.cardSpacing, 2 * kRootPadding, metrics.maxHeight, workAreaHeight);
        CardsScroll().VerticalScrollBarVisibility(layout.needsScroll
            ? Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto
            : Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled);
        CardsScroll().MaxHeight(layout.scrollViewportHeight);
        ApplySize(metrics.width, layout.windowHeight);
    }

    void PopupWindow::ApplySize(double widthDip, double heightDip)
    {
        const UINT dpi = GetDpiForWindow(hwnd_);
        const double scale = (dpi != 0 ? dpi : 96) / 96.0;
        const int32_t width = static_cast<int32_t>(std::lround(widthDip * scale));
        // Relayout already capped the height to the work area, so rounding cannot overflow it.
        const int32_t height = static_cast<int32_t>(std::lround(heightDip * scale));

        auto appWindow = AppWindow();
        if (appWindow.IsVisible()) {
            // The flyout sits above the tray, so growing or shrinking moves the top edge only.
            const auto position = appWindow.Position();
            const auto current = appWindow.Size();
            const int32_t bottom = position.Y + current.Height;
            appWindow.MoveAndResize(winrt::Windows::Graphics::RectInt32{ position.X, bottom - height, width, height });
        }
        else {
            appWindow.Resize(winrt::Windows::Graphics::SizeInt32{ width, height });
        }
    }
}
