// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "AboutPage.xaml.h"
#include "Host/SettingRow.h"
#include "Host/Strings.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#if __has_include("AboutPage.g.cpp")
#include "AboutPage.g.cpp"
#endif

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace xc = winrt::Microsoft::UI::Xaml::Controls;

constexpr wchar_t kProductName[] = L"TelaViva OLED";
constexpr wchar_t kProjectUrl[] = L"https://github.com/ksfreitas/telaviva-oled";
// Not localized. The a-acute is escaped so the source stays ASCII (the project has no /utf-8).
constexpr wchar_t kArchitectName[] = L"C\U000000E1ssio de Freitas e Silva";

// Segoe Fluent Icons code points for the update states.
constexpr wchar_t kGlyphUpToDate[] = L"\xE73E";
constexpr wchar_t kGlyphAvailable[] = L"\xE896";
constexpr wchar_t kGlyphInfo[] = L"\xE946";
constexpr wchar_t kGlyphWarning[] = L"\xE7BA";
constexpr wchar_t kGlyphFailed[] = L"\xE783";

std::wstring wstr(std::string_view key)
{
    return telaviva::host::Str(key);
}

winrt::hstring loc(std::string_view key)
{
    return winrt::hstring{ telaviva::host::Str(key) };
}

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

// Application resources resolve theme keys for the active theme, so the page follows
// light, dark and high contrast without a palette of its own.
xaml::Media::Brush themeBrush(std::wstring_view key)
{
    return xaml::Application::Current().Resources().Lookup(winrt::box_value(winrt::hstring{ key })).as<xaml::Media::Brush>();
}

xaml::Style themeStyle(std::wstring_view key)
{
    return xaml::Application::Current().Resources().Lookup(winrt::box_value(winrt::hstring{ key })).as<xaml::Style>();
}

// file:/// URI of Assets\AppIcon256.png beside the executable, with backslashes turned into slashes
// and spaces escaped, so the folder may sit anywhere.
std::wstring appIconUri()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    const std::wstring path(buffer.data(), length);
    const std::wstring folder = path.substr(0, path.find_last_of(L'\\'));

    std::wstring uri = L"file:///";
    for (const wchar_t c : folder + L"\\Assets\\AppIcon256.png") {
        if (c == L'\\') {
            uri += L'/';
        } else if (c == L' ') {
            uri += L"%20";
        } else {
            uri += c;
        }
    }
    return uri;
}

}  // namespace

namespace winrt::TelaViva::implementation
{
    AboutPage::AboutPage()
    {
        InitializeComponent();
        ProductName().Text(winrt::hstring{ kProductName });

        // The icon is optional: if it cannot be loaded, the image collapses and the page still works.
        xc::Image icon = AppIconImage();
        try {
            xaml::Media::Imaging::BitmapImage bitmap;
            bitmap.UriSource(winrt::Windows::Foundation::Uri{ appIconUri() });
            bitmap.ImageFailed([icon](auto const&, auto const&) { icon.Visibility(xaml::Visibility::Collapsed); });
            icon.Source(bitmap);
        } catch (winrt::hresult_error const&) {
            icon.Visibility(xaml::Visibility::Collapsed);
        }

        Loaded([this](auto const&, auto const&) { startChecking(); });
        Unloaded([this](auto const&, auto const&) { stopChecking(); });
        RefreshStrings();
    }

    AboutPage::~AboutPage()
    {
        if (alive_) {
            *alive_ = false;
        }
    }

    void AboutPage::SetHost(telaviva::host::ISettingsHost* host)
    {
        host_ = host;
        RefreshStrings();
    }

    void AboutPage::RefreshStrings()
    {
        PageTitle().Text(loc("settings.tab.about"));
        setVersionText();

        // Rebuilt rather than edited in place, so the labels follow the language.
        RowsPanel().Children().Clear();

        // Project link, opened in the default browser.
        xc::HyperlinkButton link;
        link.Content(winrt::box_value(winrt::hstring{ kProjectUrl }));
        link.Click([](auto const&, auto const&) {
            static_cast<void>(winrt::Windows::System::Launcher::LaunchUriAsync(
                winrt::Windows::Foundation::Uri{ kProjectUrl }));
        });
        xaml::Automation::AutomationProperties::SetName(link, loc("about.project"));
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(loc("about.project"), winrt::hstring{}, link));

        // Credits. The architect name is a constant; the coder line is a translated key.
        xc::TextBlock architect;
        architect.Text(winrt::hstring{ kArchitectName });
        architect.TextWrapping(xaml::TextWrapping::Wrap);
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(
            loc("about.credits.architect"), winrt::hstring{}, architect));

        xc::TextBlock coders;
        coders.Text(loc("about.credits.coders"));
        coders.TextWrapping(xaml::TextWrapping::Wrap);
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(
            loc("about.credits.coder"), winrt::hstring{}, coders));

        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("about.update.title")));
        RowsPanel().Children().Append(buildUpdateCard());

        refreshUpdate();
    }

    xaml::FrameworkElement AboutPage::buildUpdateCard()
    {
        // Status row: a glyph or a ring on the left, the status text on the right.
        // The rows are rebuilt, so the open button of the previous build is gone with them.
        openButton_ = nullptr;
        statusIcon_ = xc::FontIcon();
        statusIcon_.FontFamily(xaml::Media::FontFamily{ L"Segoe Fluent Icons" });
        statusIcon_.FontSize(16);
        statusIcon_.VerticalAlignment(xaml::VerticalAlignment::Center);
        xaml::Automation::AutomationProperties::SetAccessibilityView(statusIcon_, xaml::Automation::Peers::AccessibilityView::Raw);

        statusRing_ = xc::ProgressRing();
        statusRing_.IsActive(true);
        statusRing_.Width(16);
        statusRing_.Height(16);
        statusRing_.VerticalAlignment(xaml::VerticalAlignment::Center);

        xc::Grid iconSlot;
        iconSlot.Children().Append(statusIcon_);
        iconSlot.Children().Append(statusRing_);

        statusText_ = xc::TextBlock();
        statusText_.Style(themeStyle(L"BodyTextBlockStyle"));
        statusText_.TextWrapping(xaml::TextWrapping::Wrap);
        statusText_.VerticalAlignment(xaml::VerticalAlignment::Center);

        xc::Grid statusRow;
        statusRow.ColumnSpacing(12);
        xc::ColumnDefinition iconColumn;
        iconColumn.Width(xaml::GridLength{ 0, xaml::GridUnitType::Auto });
        xc::ColumnDefinition textColumn;
        textColumn.Width(xaml::GridLength{ 1, xaml::GridUnitType::Star });
        statusRow.ColumnDefinitions().Append(iconColumn);
        statusRow.ColumnDefinitions().Append(textColumn);
        xc::Grid::SetColumn(iconSlot, 0);
        xc::Grid::SetColumn(statusText_, 1);
        statusRow.Children().Append(iconSlot);
        statusRow.Children().Append(statusText_);

        // Check now is kept for the life of the card, so it keeps focus while the check runs.
        checkButton_ = xc::Button();
        checkButton_.Content(winrt::box_value(loc("about.update.checkNow")));
        checkButton_.Click([this](auto const&, auto const&) {
            if (host_) {
                host_->updateChecker().checkNow();
            }
        });
        xaml::Automation::AutomationProperties::SetName(checkButton_, loc("about.update.checkNow"));

        actionsPanel_ = xc::StackPanel();
        actionsPanel_.Orientation(xc::Orientation::Horizontal);
        actionsPanel_.Spacing(8);
        actionsPanel_.Children().Append(checkButton_);

        xc::StackPanel body;
        body.Spacing(12);
        body.Children().Append(statusRow);
        body.Children().Append(actionsPanel_);

        xc::Border card;
        card.Background(themeBrush(L"CardBackgroundFillColorDefaultBrush"));
        card.BorderBrush(themeBrush(L"CardStrokeColorDefaultBrush"));
        card.BorderThickness(xaml::Thickness{ 1, 1, 1, 1 });
        card.CornerRadius(xaml::CornerRadius{ 4, 4, 4, 4 });
        card.Padding(xaml::Thickness{ 16, 12, 16, 12 });
        card.Child(body);
        return card;
    }

    void AboutPage::startChecking()
    {
        if (!host_) {
            return;
        }
        alive_ = std::make_shared<bool>(true);
        auto dispatcher = DispatcherQueue();
        // Called from the checker's worker thread: the UI work is queued and skipped once the page stops.
        host_->updateChecker().setOnChanged([alive = alive_, dispatcher, this]() {
            static_cast<void>(dispatcher.TryEnqueue([alive, this]() {
                if (*alive) {
                    refreshUpdate();
                }
            }));
        });
        host_->updateChecker().checkIfNeeded();
        refreshUpdate();
    }

    void AboutPage::stopChecking()
    {
        if (alive_) {
            *alive_ = false;
        }
        if (host_) {
            host_->updateChecker().setOnChanged({});
        }
    }

    void AboutPage::refreshUpdate()
    {
        if (!statusText_) {
            return;
        }
        const telaviva::UpdateState state = host_ ? host_->updateChecker().state() : telaviva::UpdateState{};

        const bool checking = state.kind == telaviva::UpdateKind::Idle || state.kind == telaviva::UpdateKind::Checking;
        statusRing_.Visibility(checking ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        statusIcon_.Visibility(checking ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
        checkButton_.IsEnabled(state.kind != telaviva::UpdateKind::Checking);

        std::wstring label;
        const wchar_t* glyph = kGlyphInfo;
        std::wstring_view brush = L"TextFillColorSecondaryBrush";
        switch (state.kind) {
        case telaviva::UpdateKind::Idle:
        case telaviva::UpdateKind::Checking:
            label = wstr("about.update.checking");
            break;
        case telaviva::UpdateKind::UpToDate:
            glyph = kGlyphUpToDate;
            brush = L"SystemFillColorSuccessBrush";
            label = wstr("about.update.upToDate");
            break;
        case telaviva::UpdateKind::UpdateAvailable: {
            glyph = kGlyphAvailable;
            brush = L"AccentFillColorDefaultBrush";
            label = wstr("about.update.available");
            const std::wstring placeholder = L"{version}";
            const std::size_t at = label.find(placeholder);
            if (at != std::wstring::npos) {
                label.replace(at, placeholder.size(), utf8ToWide(state.remoteVersion));
            }
            break;
        }
        case telaviva::UpdateKind::NoRelease:
            label = wstr("about.update.noRelease");
            break;
        case telaviva::UpdateKind::RateLimited:
            glyph = kGlyphWarning;
            brush = L"SystemFillColorCautionBrush";
            label = wstr("about.update.rateLimited");
            break;
        case telaviva::UpdateKind::Failed:
            glyph = kGlyphFailed;
            label = wstr("about.update.failed");
            break;
        }

        statusIcon_.Glyph(glyph);
        statusIcon_.Foreground(themeBrush(brush));
        statusText_.Text(winrt::hstring{ label });
        xaml::Automation::AutomationProperties::SetName(statusText_, winrt::hstring{ label });

        // The open button is added and removed here, so the Check now button keeps its place.
        if (state.kind == telaviva::UpdateKind::UpdateAvailable) {
            if (!openButton_) {
                openButton_ = xc::Button();
                openButton_.Style(themeStyle(L"AccentButtonStyle"));
                openButton_.Content(winrt::box_value(loc("about.update.openRelease")));
                openButton_.Click([this](auto const&, auto const&) {
                    if (!host_) {
                        return;
                    }
                    const std::string url = host_->updateChecker().state().releaseUrl;
                    if (!url.empty()) {
                        static_cast<void>(winrt::Windows::System::Launcher::LaunchUriAsync(
                            winrt::Windows::Foundation::Uri{ utf8ToWide(url) }));
                    }
                });
                xaml::Automation::AutomationProperties::SetName(openButton_, loc("about.update.openRelease"));
                actionsPanel_.Children().Append(openButton_);
            }
        } else if (openButton_) {
            uint32_t index = 0;
            if (actionsPanel_.Children().IndexOf(openButton_, index)) {
                actionsPanel_.Children().RemoveAt(index);
            }
            openButton_ = nullptr;
        }
    }

    void AboutPage::setVersionText()
    {
        std::wstring line = wstr("about.version");
        if (host_) {
            line += L" " + utf8ToWide(host_->appVersion());
        }
        VersionText().Text(winrt::hstring{ line });
    }
}
