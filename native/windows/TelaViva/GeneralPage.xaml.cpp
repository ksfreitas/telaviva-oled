// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "GeneralPage.xaml.h"
#include "Host/SettingRow.h"
#include "Host/Strings.h"
#include "Platform/CameraSelection.h"
#include "Platform/SettingsModel.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Threading.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __has_include("GeneralPage.g.cpp")
#include "GeneralPage.g.cpp"
#endif

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace xc = winrt::Microsoft::UI::Xaml::Controls;

// Picker order: system (auto), English (US), Portuguese (Brazil). The core takes these values.
constexpr const char* kLocales[] = { "auto", "en-US", "pt-BR" };
constexpr int kLocaleCount = 3;

// Mirrors the core limits in crates/telaviva-core/src/settings.rs validate().
constexpr double kPresenceIntervalMin = 5.0;
constexpr double kPresenceIntervalMax = 300.0;
constexpr double kReturnIntervalMin = 0.25;
constexpr double kReturnIntervalMax = 5.0;
constexpr double kMinBrightnessMin = 10.0;
constexpr double kMinBrightnessMax = 90.0;

// One second in 100 ns ticks, the unit of TimeSpan.
constexpr std::int64_t kOneSecondTicks = 10'000'000;

int indexOfLocale(std::string_view locale)
{
    for (int i = 0; i < kLocaleCount; ++i) {
        if (locale == kLocales[i]) {
            return i;
        }
    }
    // Empty before the first snapshot, and any unknown value, shows the system choice.
    return 0;
}

winrt::hstring loc(std::string_view key)
{
    return winrt::hstring{ telaviva::host::Str(key) };
}

winrt::hstring utf8(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return winrt::hstring{ out };
}

// Numeric entry with the core's range. Step buttons move by one step; typing outside the range is clamped.
xc::NumberBox makeNumberBox(double minimum, double maximum, double step, double value)
{
    xc::NumberBox box;
    box.Minimum(minimum);
    box.Maximum(maximum);
    box.SmallChange(step);
    box.LargeChange(step * 10);
    box.Value(value);
    box.SpinButtonPlacementMode(xc::NumberBoxSpinButtonPlacementMode::Compact);
    box.ValidationMode(xc::NumberBoxValidationMode::InvalidInputOverwritten);
    box.Width(120);
    return box;
}

// Places a unit caption after a control. The unit carries its own space where the spec has one ("N s"), and none for "N%".
xaml::FrameworkElement withUnit(xaml::UIElement const& control, std::wstring_view unit)
{
    xc::TextBlock label;
    label.Text(winrt::hstring{ unit });
    label.VerticalAlignment(xaml::VerticalAlignment::Center);

    xc::StackPanel row;
    row.Orientation(xc::Orientation::Horizontal);
    row.Children().Append(control);
    row.Children().Append(label);
    return row;
}

// Switch with empty On and Off text; the row header already names the setting.
xc::ToggleSwitch makeSwitch(bool on, winrt::hstring const& name)
{
    xc::ToggleSwitch toggle;
    toggle.OnContent(winrt::box_value(winrt::hstring{}));
    toggle.OffContent(winrt::box_value(winrt::hstring{}));
    toggle.IsOn(on);
    xaml::Automation::AutomationProperties::SetName(toggle, name);
    return toggle;
}

xc::Slider makeSlider(double maximum, double value)
{
    xc::Slider slider;
    slider.Minimum(0.0);
    slider.Maximum(maximum);
    slider.StepFrequency(1.0);
    slider.Value(value);
    slider.Width(160);
    return slider;
}

std::wstring percentText(double value)
{
    return std::to_wstring(static_cast<int>(std::lround(value))) + L"%";
}

// Return interval with up to two decimals, so 1.25 shows as "1.25" (pt-BR "1,25"). Auto uses the user's regional
// settings; an explicit language passes its tag and region. A formatting failure never throws: the caller gets
// a plain formatter, or none when even that cannot be built.
winrt::Windows::Globalization::NumberFormatting::INumberFormatter2 intervalFormatter(int localeIndex)
{
    namespace nf = winrt::Windows::Globalization::NumberFormatting;
    nf::DecimalFormatter formatter;
    try {
        if (localeIndex > 0 && localeIndex < kLocaleCount) {
            const std::string tag = kLocales[localeIndex];
            // The region is the part after '-', uppercased ("pt-BR" gives "BR"); a tag without one uses the home region.
            winrt::hstring region;
            const auto dash = tag.find('-');
            if (dash != std::string::npos && dash + 1 < tag.size()) {
                std::string code = tag.substr(dash + 1);
                for (auto& c : code) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                region = utf8(code);
            }
            else {
                region = winrt::Windows::System::UserProfile::GlobalizationPreferences::HomeGeographicRegion();
            }
            formatter = nf::DecimalFormatter{ winrt::single_threaded_vector<winrt::hstring>({ utf8(tag) }), region };
        }

        nf::IncrementNumberRounder rounder;
        rounder.Increment(0.01);
        rounder.RoundingAlgorithm(nf::RoundingAlgorithm::RoundHalfUp);
        formatter.FractionDigits(0);
        formatter.IsGrouped(false);
        formatter.NumberRounder(rounder);
        return formatter.as<nf::INumberFormatter2>();
    }
    catch (const winrt::hresult_error&) {
        // Keep the default formatter; the NumberBox still accepts and shows the value.
    }
    try {
        return nf::DecimalFormatter{}.as<nf::INumberFormatter2>();
    }
    catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

// Identity of the connected monitors. A change rebuilds the monitor rows.
std::string monitorSignature(const telaviva::Snapshot& snapshot)
{
    std::string signature;
    for (const auto& monitor : snapshot.monitors) {
        signature += monitor.id;
        signature.push_back('\n');
        signature += monitor.name;
        signature.push_back('\n');
    }
    return signature;
}

// Highest screensaver target for a monitor: its confirmed brightness, or 0 without a brightness control.
double brightnessCeiling(const telaviva::MonitorSnapshot& monitor)
{
    const auto& observed = monitor.brightness.observed;
    if (!monitor.brightness.supported || !observed || !std::isfinite(*observed)) {
        return 0.0;
    }
    return std::clamp(*observed, 0.0, 100.0);
}

// The display status when present, else the raw status, as AppText.cameraStatus does on macOS.
std::string cameraStatusKey(const telaviva::Snapshot& snapshot)
{
    const auto& camera = snapshot.camera;
    if (!camera.displayStatus.empty()) {
        return camera.displayStatus;
    }
    if (!camera.status.empty()) {
        return camera.status;
    }
    return "unknown";
}

// The picker's list: selectable cameras with no repeated id. Every path that fills the ComboBox uses it.
std::vector<telaviva::CameraInfo> pickerCameras(const std::vector<telaviva::CameraInfo>& all)
{
    return telaviva::dedupeCameras(telaviva::selectableCameras(all), {});
}

bool sameCameras(const std::vector<telaviva::CameraInfo>& a, const std::vector<telaviva::CameraInfo>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace winrt::TelaViva::implementation
{
    GeneralPage::GeneralPage()
    {
        InitializeComponent();
        // The host arrives after construction, so the settings load when the page is first shown.
        Loaded([this](auto const&, auto const&) {
            tryLoad();
            reloadCameras();
            startPolling();
            // Programmatic focus on the first control, so arriving from the navigation pane lands in the page.
            if (languageBox_) {
                static_cast<void>(languageBox_.Focus(xaml::FocusState::Programmatic));
            }
        });
        Unloaded([this](auto const&, auto const&) {
            if (poll_) {
                poll_.Stop();
            }
        });
        RefreshStrings();
    }

    void GeneralPage::SetHost(telaviva::host::ISettingsHost* host)
    {
        host_ = host;
        tryLoad();
    }

    void GeneralPage::SetOnLanguageChanged(std::function<void()> callback)
    {
        onLanguageChanged_ = std::move(callback);
    }

    void GeneralPage::tryLoad()
    {
        // Loads once: later changes made elsewhere are not picked up, as on macOS.
        if (loaded_ || !host_ || !IsLoaded()) {
            return;
        }
        loaded_ = true;
        localeIndex_ = indexOfLocale(host_->currentSettings().locale);
        launchAtLogin_ = host_->launchAtLoginEnabled();
        RefreshStrings();
    }

    void GeneralPage::RefreshStrings()
    {
        PageTitle().Text(loc("settings.tab.general"));

        // Rebuilt rather than edited in place: the row helper does not expose its header text.
        // The selections live in members, so a rebuild keeps them.
        RowsPanel().Children().Clear();
        const telaviva::Snapshot snapshot = host_ ? host_->latestSnapshot() : telaviva::Snapshot{};
        const telaviva::SettingsSnapshot& settings = snapshot.settings;
        builtRevision_ = snapshot.revision;
        monitorSignature_.clear();
        targets_.clear();

        // General: language and launch at login.
        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("general")));

        const winrt::hstring languageHeader = loc("language");
        xc::ComboBox languageBox;
        languageBox.Items().Append(winrt::box_value(loc("systemLanguage")));
        languageBox.Items().Append(winrt::box_value(loc("english")));
        languageBox.Items().Append(winrt::box_value(loc("portuguese")));
        // The selection is set before the handler is attached, so building the picker raises no change.
        languageBox.SelectedIndex(localeIndex_);
        xaml::Automation::AutomationProperties::SetName(languageBox, languageHeader);
        languageBox_ = languageBox;
        languageBox.SelectionChanged([this](auto const& sender, auto const&) {
            const int index = sender.as<xc::ComboBox>().SelectedIndex();
            if (index < 0 || index >= kLocaleCount || index == localeIndex_) {
                return;
            }
            localeIndex_ = index;
            const std::string locale = kLocales[index];
            if (host_) {
                auto draft = telaviva::draftFromSnapshotSettings(host_->currentSettings());
                draft.locale = locale;
                host_->submit(telaviva::buildSettingsPayload(draft));
            }
            telaviva::host::setLanguage(locale);
            notifyLanguageChanged();
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(languageHeader, winrt::hstring{}, languageBox));

        const winrt::hstring launchHeader = loc("launch");
        xc::ToggleSwitch launchSwitch = makeSwitch(launchAtLogin_, launchHeader);
        launchSwitch.Toggled([this](auto const& sender, auto const&) {
            if (suppressToggle_ || !host_) {
                return;
            }
            const auto toggle = sender.as<xc::ToggleSwitch>();
            const bool enable = toggle.IsOn();
            const auto result = host_->setLaunchAtLogin(enable);
            if (result.ok) {
                launchAtLogin_ = enable;
                auto draft = telaviva::draftFromSnapshotSettings(host_->currentSettings());
                draft.launchAtLogin = enable;
                host_->submit(telaviva::buildSettingsPayload(draft));
                return;
            }

            // Reverts to the verified state. The guard keeps the revert from running this handler again.
            launchAtLogin_ = result.enabled;
            suppressToggle_ = true;
            toggle.IsOn(result.enabled);
            suppressToggle_ = false;

            xc::ContentDialog dialog;
            dialog.Title(winrt::box_value(loc("loginItemFailureTitle")));
            dialog.Content(winrt::box_value(winrt::hstring{ telaviva::host::Str("loginItemFailure") + L" " + result.error }));
            dialog.CloseButtonText(loc("ok"));
            dialog.XamlRoot(XamlRoot());
            dialog.ShowAsync();
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(launchHeader, winrt::hstring{}, launchSwitch));

        // OLED protection. Camera problems sit at the top, where the presence controls start.
        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("protection.title")));

        cameraLink_ = xc::HyperlinkButton{};
        cameraLink_.Content(winrt::box_value(loc("cameraPrivacyLink")));
        cameraLink_.Click([](auto const&, auto const&) {
            static_cast<void>(winrt::Windows::System::Launcher::LaunchUriAsync(
                winrt::Windows::Foundation::Uri{ L"ms-settings:privacy-webcam" }));
        });
        cameraInfoBar_ = xc::InfoBar{};
        cameraInfoBar_.Severity(xc::InfoBarSeverity::Warning);
        cameraInfoBar_.IsClosable(false);
        cameraInfoBar_.IsOpen(false);
        cameraInfoBar_.Content(cameraLink_);
        RowsPanel().Children().Append(cameraInfoBar_);

        const bool presenceOn = settings.presence_protection_enabled;
        const winrt::hstring presenceHeader = loc("presence");
        xc::ToggleSwitch presenceSwitch = makeSwitch(presenceOn, presenceHeader);
        presenceSwitch.Toggled([this](auto const& sender, auto const&) {
            const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
            if (thresholdBox_) {
                thresholdBox_.IsEnabled(enabled);
            }
            edit([enabled](telaviva::SettingsDraft& draft) { draft.presenceProtectionEnabled = enabled; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(presenceHeader, loc("presenceHelp"), presenceSwitch));

        // Brightness at or below which the camera stays off. Editable only while presence protection is on.
        thresholdBox_ = makeNumberBox(kMinBrightnessMin, kMinBrightnessMax, 1.0, settings.presence_min_brightness_percent);
        thresholdBox_.IsEnabled(presenceOn);
        xaml::Automation::AutomationProperties::SetName(thresholdBox_, loc("presenceMinBrightness"));
        thresholdBox_.ValueChanged([this](xc::NumberBox const&, xc::NumberBoxValueChangedEventArgs const& args) {
            const double value = args.NewValue();
            if (std::isnan(value)) {
                return;
            }
            edit([value](telaviva::SettingsDraft& draft) { draft.presenceMinBrightnessPercent = value; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(
            loc("presenceMinBrightness"), loc("presenceMinBrightnessHelp"), withUnit(thresholdBox_, L"%")));

        const winrt::hstring alwaysOnHeader = loc("alwaysOnCamera");
        xc::ToggleSwitch alwaysOnSwitch = makeSwitch(settings.always_on_camera, alwaysOnHeader);
        alwaysOnSwitch.Toggled([this](auto const& sender, auto const&) {
            const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
            edit([enabled](telaviva::SettingsDraft& draft) { draft.alwaysOnCamera = enabled; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(alwaysOnHeader, loc("alwaysOnCameraHelp"), alwaysOnSwitch));

        const winrt::hstring saverHeader = loc("saver");
        xc::ToggleSwitch saverSwitch = makeSwitch(settings.screensaver_protection_enabled, saverHeader);
        saverSwitch.Toggled([this](auto const& sender, auto const&) {
            const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
            edit([enabled](telaviva::SettingsDraft& draft) { draft.screensaverProtectionEnabled = enabled; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(saverHeader, winrt::hstring{}, saverSwitch));
        // One target row per connected monitor, filled by buildMonitorRows.
        targetsPanel_ = xc::StackPanel{};
        targetsPanel_.Spacing(8);
        RowsPanel().Children().Append(targetsPanel_);

        const winrt::hstring batteryHeader = loc("battery");
        xc::ToggleSwitch batterySwitch = makeSwitch(settings.suspend_presence_on_battery, batteryHeader);
        batterySwitch.Toggled([this](auto const& sender, auto const&) {
            const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
            edit([enabled](telaviva::SettingsDraft& draft) { draft.suspendPresenceOnBattery = enabled; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(batteryHeader, winrt::hstring{}, batterySwitch));

        xc::NumberBox checkBox = makeNumberBox(kPresenceIntervalMin, kPresenceIntervalMax, 1.0, settings.presence_sampling_interval_s);
        xaml::Automation::AutomationProperties::SetName(checkBox, loc("check"));
        checkBox.ValueChanged([this](xc::NumberBox const&, xc::NumberBoxValueChangedEventArgs const& args) {
            const double value = args.NewValue();
            if (std::isnan(value)) {
                return;
            }
            edit([value](telaviva::SettingsDraft& draft) { draft.presenceSamplingIntervalS = value; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(loc("check"), winrt::hstring{}, withUnit(checkBox, L" s")));

        xc::NumberBox returnBox = makeNumberBox(kReturnIntervalMin, kReturnIntervalMax, 0.25, settings.return_sampling_interval_s);
        if (const auto formatter = intervalFormatter(localeIndex_)) {
            returnBox.NumberFormatter(formatter);
        }
        xaml::Automation::AutomationProperties::SetName(returnBox, loc("return"));
        returnBox.ValueChanged([this](xc::NumberBox const&, xc::NumberBoxValueChangedEventArgs const& args) {
            const double value = args.NewValue();
            if (std::isnan(value)) {
                return;
            }
            edit([value](telaviva::SettingsDraft& draft) { draft.returnSamplingIntervalS = value; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(loc("return"), winrt::hstring{}, withUnit(returnBox, L" s")));

        // Monitors in protection: one switch per connected monitor, filled by buildMonitorRows.
        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("protection.monitors.title")));
        RowsPanel().Children().Append(telaviva::host::MakeCaption(loc("protection.monitors.help")));
        monitorsPanel_ = xc::StackPanel{};
        monitorsPanel_.Spacing(8);
        RowsPanel().Children().Append(monitorsPanel_);

        // Availability: the camera picker, its status, and the two help captions.
        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("availability")));

        cameraBox_ = xc::ComboBox{};
        xaml::Automation::AutomationProperties::SetName(cameraBox_, loc("camera"));
        cameraBox_.SelectionChanged([this](auto const& sender, auto const&) {
            if (suppress_ || !host_) {
                return;
            }
            const int index = sender.as<xc::ComboBox>().SelectedIndex();
            if (index < 0 || index > static_cast<int>(cameras_.size())) {
                return;
            }
            // Index 0 is the default camera, which clears the selection.
            std::optional<std::string> id;
            if (index > 0) {
                id = cameras_[static_cast<std::size_t>(index - 1)].id;
            }
            host_->submit(telaviva::buildSelectCameraPayload(id));
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(loc("camera"), winrt::hstring{}, cameraBox_));

        // Camera glyph (U+E722), escaped so the source stays ASCII.
        // Decorative: the status text beside it already says what the line is.
        xc::FontIcon cameraGlyph;
        cameraGlyph.Glyph(L"\xE722");
        cameraGlyph.FontFamily(xaml::Media::FontFamily{ L"Segoe Fluent Icons" });
        cameraGlyph.FontSize(14);
        cameraGlyph.VerticalAlignment(xaml::VerticalAlignment::Center);
        xaml::Automation::AutomationProperties::SetAccessibilityView(cameraGlyph, xaml::Automation::Peers::AccessibilityView::Raw);

        cameraStatusText_ = xc::TextBlock{};
        cameraStatusText_.VerticalAlignment(xaml::VerticalAlignment::Center);

        xc::StackPanel statusLine;
        statusLine.Orientation(xc::Orientation::Horizontal);
        statusLine.Spacing(8);
        statusLine.Margin(xaml::Thickness{ 4, 4, 0, 0 });
        statusLine.Children().Append(cameraGlyph);
        statusLine.Children().Append(cameraStatusText_);
        RowsPanel().Children().Append(statusLine);

        RowsPanel().Children().Append(telaviva::host::MakeCaption(loc("cameraHelp")));
        RowsPanel().Children().Append(telaviva::host::MakeCaption(loc("displayHelp")));

        // Fills everything from the latest snapshot, the same data the poll reads from.
        cameras_ = pickerCameras(host_ ? host_->cameras() : std::vector<telaviva::CameraInfo>{});
        fillCameraBox(settings.selected_camera_id);
        buildMonitorRows(snapshot);
        updateCameraStatus(snapshot);
    }

    void GeneralPage::edit(const std::function<void(telaviva::SettingsDraft&)>& change)
    {
        if (!host_) {
            return;
        }
        auto draft = telaviva::draftFromSnapshotSettings(host_->currentSettings());
        change(draft);
        host_->submit(telaviva::buildSettingsPayload(draft));
    }

    void GeneralPage::notifyLanguageChanged()
    {
        if (!onLanguageChanged_) {
            return;
        }
        // Deferred past the picker's change handler: the callback rebuilds this page, including the picker raising it.
        DispatcherQueue().TryEnqueue([callback = onLanguageChanged_] { callback(); });
    }

    void GeneralPage::startPolling()
    {
        if (!poll_) {
            poll_ = DispatcherQueue().CreateTimer();
            poll_.Interval(winrt::Windows::Foundation::TimeSpan{ kOneSecondTicks });
            poll_.Tick([this](auto const&, auto const&) { onPoll(); });
        }
        poll_.Start();
    }

    void GeneralPage::onPoll()
    {
        if (!host_) {
            return;
        }
        const telaviva::Snapshot snapshot = host_->latestSnapshot();
        if (builtRevision_ == 0 && snapshot.revision != 0) {
            // Built before the first snapshot arrived, so the controls hold placeholder values.
            RefreshStrings();
            return;
        }
        syncCameras(snapshot.settings.selected_camera_id);
        updateCameraStatus(snapshot);
        if (monitorSignature(snapshot) != monitorSignature_) {
            buildMonitorRows(snapshot);
        } else {
            updateTargets(snapshot);
        }
    }

    void GeneralPage::reloadCameras()
    {
        if (!host_) {
            return;
        }
        telaviva::host::ISettingsHost* host = host_;
        // Enumeration blocks on WinRT calls, so it runs on the thread pool; the poll reads the cache.
        winrt::Windows::System::Threading::ThreadPool::RunAsync([host](auto const&) { host->refreshCameras(); });
    }

    void GeneralPage::syncCameras(const std::optional<std::string>& selected)
    {
        std::vector<telaviva::CameraInfo> found = pickerCameras(host_->cameras());
        if (sameCameras(found, cameras_)) {
            return;
        }
        cameras_ = std::move(found);
        fillCameraBox(selected);
    }

    void GeneralPage::fillCameraBox(const std::optional<std::string>& selected)
    {
        if (!cameraBox_) {
            return;
        }
        // Defensive: one item per id, so the index-to-camera mapping stays one to one.
        cameras_ = telaviva::dedupeCameras(cameras_, {});
        // suppress_ keeps the programmatic selection from submitting a change.
        suppress_ = true;
        cameraBox_.Items().Clear();
        cameraBox_.Items().Append(winrt::box_value(loc("cameraPicker")));
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            cameraBox_.Items().Append(winrt::box_value(winrt::hstring{ cameras_[i].name }));
        }
        // An id missing from the list (unplugged camera) shows the default item; the stored setting is left as is.
        const std::optional<std::size_t> found = telaviva::findCameraIndex(cameras_, selected);
        cameraBox_.SelectedIndex(found ? static_cast<int>(*found) + 1 : 0);
        suppress_ = false;
    }

    void GeneralPage::updateCameraStatus(const telaviva::Snapshot& snapshot)
    {
        const std::string key = cameraStatusKey(snapshot);
        // "Camera: <status>", the same line the status row and the warning show.
        const winrt::hstring line{ telaviva::host::Str("camera") + L": " + telaviva::host::Str("cameraStatus." + key) };
        cameraStatusText_.Text(line);

        // Denied, busy, unavailable and error get the warning; only denial links to the Windows setting.
        const bool attention = key == "permission" || key == "busy" || key == "unavailable" || key == "error";
        cameraInfoBar_.Message(line);
        cameraInfoBar_.IsOpen(attention);
        cameraLink_.Visibility(key == "permission" ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    }

    void GeneralPage::buildMonitorRows(const telaviva::Snapshot& snapshot)
    {
        monitorSignature_ = monitorSignature(snapshot);
        monitorsPanel_.Children().Clear();
        targetsPanel_.Children().Clear();
        targets_.clear();
        if (snapshot.monitors.empty()) {
            monitorsPanel_.Children().Append(telaviva::host::MakeCaption(loc("protection.monitors.none")));
            return;
        }

        const auto& disabled = snapshot.settings.protection_disabled;
        for (const auto& monitor : snapshot.monitors) {
            const std::string id = monitor.id;
            const winrt::hstring name = utf8(monitor.name.empty() ? monitor.id : monitor.name);

            // Protected unless listed in protection_disabled. Each change writes the full set.
            xc::ToggleSwitch toggle = makeSwitch(disabled.count(id) == 0, name);
            toggle.Toggled([this, id](auto const& sender, auto const&) {
                const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
                edit([id, enabled](telaviva::SettingsDraft& draft) {
                    if (enabled) {
                        draft.protectionDisabled.erase(id);
                    } else {
                        draft.protectionDisabled.insert(id);
                    }
                });
            });
            monitorsPanel_.Children().Append(telaviva::host::MakeSettingRow(name, winrt::hstring{}, toggle));

            // Screensaver target for this monitor, capped at its current brightness.
            const double ceiling = brightnessCeiling(monitor);
            const auto& targets = snapshot.settings.screensaver_targets;
            const auto found = targets.find(id);
            const double target = found == targets.end() ? 0.0 : found->second;

            TargetRow row;
            row.monitorId = id;
            row.slider = makeSlider(ceiling > 0 ? ceiling : 1.0, std::min(target, ceiling));
            row.slider.IsEnabled(ceiling > 0);
            // Named after the monitor, so the slider reads as this monitor's target; the percent is its item status.
            const std::wstring sliderName = telaviva::host::Str("protectionBrightness") + L", " + name.c_str();
            xaml::Automation::AutomationProperties::SetName(row.slider, winrt::hstring{ sliderName });
            xaml::Automation::AutomationProperties::SetItemStatus(row.slider, winrt::hstring{ percentText(row.slider.Value()) });
            row.readout = xc::TextBlock{};
            row.readout.Text(percentText(row.slider.Value()));
            row.readout.MinWidth(48);
            row.readout.VerticalAlignment(xaml::VerticalAlignment::Center);
            xaml::Automation::AutomationProperties::SetAccessibilityView(row.readout, xaml::Automation::Peers::AccessibilityView::Raw);

            xc::StackPanel line;
            line.Orientation(xc::Orientation::Horizontal);
            line.Spacing(8);
            line.Children().Append(row.slider);
            line.Children().Append(row.readout);

            row.slider.ValueChanged([this, id, readout = row.readout](auto const& sender, auto const&) {
                const double value = sender.as<xc::Slider>().Value();
                readout.Text(percentText(value));
                xaml::Automation::AutomationProperties::SetItemStatus(sender.as<xc::Slider>(), winrt::hstring{ percentText(value) });
                if (suppress_) {
                    return;
                }
                edit([id, value](telaviva::SettingsDraft& draft) { draft.screensaverTargets[id] = value; });
            });
            targets_.push_back(row);
            targetsPanel_.Children().Append(telaviva::host::MakeSettingRow(loc("protectionBrightness"), name, line));
        }
    }

    void GeneralPage::updateTargets(const telaviva::Snapshot& snapshot)
    {
        for (auto& row : targets_) {
            const auto monitor = std::find_if(snapshot.monitors.begin(), snapshot.monitors.end(),
                [&row](const telaviva::MonitorSnapshot& candidate) { return candidate.id == row.monitorId; });
            if (monitor == snapshot.monitors.end()) {
                continue;
            }
            // Brightness changes move the cap; the stored target stays as the user set it.
            const double ceiling = brightnessCeiling(*monitor);
            suppress_ = true;
            row.slider.Maximum(ceiling > 0 ? ceiling : 1.0);
            row.slider.IsEnabled(ceiling > 0);
            if (ceiling <= 0) {
                row.slider.Value(0.0);
            }
            suppress_ = false;
            row.readout.Text(percentText(row.slider.Value()));
        }
    }
}
