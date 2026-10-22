// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "AdvancedPage.xaml.h"
#include "Host/SettingRow.h"
#include "Host/Strings.h"
#include "Platform/DeviceSelection.h"
#include "Platform/SettingsModel.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#if __has_include("AdvancedPage.g.cpp")
#include "AdvancedPage.g.cpp"
#endif

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace xc = winrt::Microsoft::UI::Xaml::Controls;

// One second in 100 ns ticks, the unit of TimeSpan.
constexpr std::int64_t kOneSecondTicks = 10'000'000;

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

// Replaces the first "{0}" in the pattern with the value.
winrt::hstring fillPlaceholder(winrt::hstring const& pattern, winrt::hstring const& value)
{
    std::wstring text{ pattern.c_str() };
    const std::wstring placeholder = L"{0}";
    const auto at = text.find(placeholder);
    if (at != std::wstring::npos) {
        text.replace(at, placeholder.size(), value.c_str());
    }
    return winrt::hstring{ text };
}

// "<adapter> (integrated GPU)" or "<adapter> (discrete GPU)".
winrt::hstring withKind(std::string_view description, std::string_view kindKey)
{
    return winrt::hstring{ std::wstring{ utf8(description).c_str() } + L" (" + loc(kindKey).c_str() + L")" };
}

// Picker text for one device: the labels are Str() keys, GPUs show their adapter name.
winrt::hstring optionLabel(const telaviva::DeviceOption& option)
{
    switch (option.kind) {
    case telaviva::DeviceKind::Auto:
        return loc("inference.auto");
    case telaviva::DeviceKind::Cpu:
        return loc("inference.cpu");
    case telaviva::DeviceKind::IntegratedGpu:
        return withKind(option.description, "inference.kind.integrated");
    case telaviva::DeviceKind::DiscreteGpu:
        return withKind(option.description, "inference.kind.discrete");
    case telaviva::DeviceKind::Gpu:
        break;
    }
    return utf8(option.description);
}

// Switch with empty On and Off text; the row header already names the setting.
xc::ToggleSwitch makeSwitch(winrt::hstring const& name)
{
    xc::ToggleSwitch toggle;
    toggle.OnContent(winrt::box_value(winrt::hstring{}));
    toggle.OffContent(winrt::box_value(winrt::hstring{}));
    xaml::Automation::AutomationProperties::SetName(toggle, name);
    return toggle;
}

}  // namespace

namespace winrt::TelaViva::implementation
{
    AdvancedPage::AdvancedPage()
    {
        InitializeComponent();
        // The host arrives after construction, so the device list is filled when the page is first shown.
        Loaded([this](auto const&, auto const&) {
            fillDeviceBox();
            updateReport();
            startPolling();
        });
        Unloaded([this](auto const&, auto const&) {
            if (poll_) {
                poll_.Stop();
            }
        });
        RefreshStrings();
    }

    void AdvancedPage::SetHost(telaviva::host::ISettingsHost* host)
    {
        host_ = host;
        fillDeviceBox();
        syncAlwaysOn();
        updateReport();
    }

    void AdvancedPage::RefreshStrings()
    {
        PageTitle().Text(loc("settings.tab.advanced"));

        // Rebuilt rather than edited in place, as on the General page. The list is refilled below.
        RowsPanel().Children().Clear();

        // Camera: the keep-alive switch and its warning. Built here so it follows the same rebuild as the rows below.
        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("camera")));
        alwaysOnSwitch_ = makeSwitch(loc("alwaysOnCamera"));
        alwaysOnSwitch_.Toggled([this](auto const& sender, auto const&) {
            if (suppress_ || !host_) {
                return;
            }
            const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
            edit([enabled](telaviva::SettingsDraft& draft) { draft.alwaysOnCamera = enabled; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(loc("alwaysOnCamera"), loc("alwaysOnCameraHelp"), alwaysOnSwitch_));
        RowsPanel().Children().Append(telaviva::host::MakeCaption(loc("alwaysOnCameraWarning")));

        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("inference.title")));

        const winrt::hstring deviceHeader = loc("inference.title");
        deviceBox_ = xc::ComboBox{};
        xaml::Automation::AutomationProperties::SetName(deviceBox_, deviceHeader);
        deviceBox_.SelectionChanged([this](auto const& sender, auto const&) {
            if (suppress_ || !host_) {
                return;
            }
            const int index = sender.as<xc::ComboBox>().SelectedIndex();
            if (index < 0 || index >= static_cast<int>(deviceIds_.size())) {
                return;
            }
            const std::string id = deviceIds_[static_cast<std::size_t>(index)];
            edit([id](telaviva::SettingsDraft& draft) { draft.inferenceDevice = id; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(deviceHeader, loc("inference.help"), deviceBox_));
        RowsPanel().Children().Append(telaviva::host::MakeCaption(loc("inference.discreteHelp")));

        // The device in use, and the notice when the chosen device was not used.
        inUseText_ = xc::TextBlock{};
        inUseText_.TextWrapping(xaml::TextWrapping::Wrap);
        inUseText_.Margin(xaml::Thickness{ 4, 4, 0, 0 });
        RowsPanel().Children().Append(inUseText_);

        noticeBar_ = xc::InfoBar{};
        noticeBar_.Severity(xc::InfoBarSeverity::Warning);
        noticeBar_.IsClosable(false);
        noticeBar_.IsOpen(false);
        RowsPanel().Children().Append(noticeBar_);

        fillDeviceBox();
        syncAlwaysOn();
        updateReport();
    }

    void AdvancedPage::edit(const std::function<void(telaviva::SettingsDraft&)>& change)
    {
        if (!host_) {
            return;
        }
        auto draft = telaviva::draftFromSnapshotSettings(host_->currentSettings());
        change(draft);
        host_->submit(telaviva::buildSettingsPayload(draft));
    }

    void AdvancedPage::fillDeviceBox()
    {
        if (!deviceBox_ || !host_) {
            return;
        }
        const std::string stored = host_->currentSettings().inference_device;

        // suppress_ keeps the programmatic population and selection from submitting a change.
        suppress_ = true;
        deviceBox_.Items().Clear();
        deviceIds_.clear();
        bool listed = false;
        for (const auto& option : host_->inferenceDevices()) {
            deviceIds_.push_back(option.id);
            deviceBox_.Items().Append(winrt::box_value(optionLabel(option)));
            listed = listed || option.id == stored;
        }
        // A stored id that is not in the list (a GPU that is gone) stands for itself as one extra item.
        if (!listed) {
            deviceIds_.push_back(stored);
            deviceBox_.Items().Append(winrt::box_value(loc("inference.missing")));
        }
        const auto found = std::find(deviceIds_.begin(), deviceIds_.end(), stored);
        deviceBox_.SelectedIndex(static_cast<int>(found - deviceIds_.begin()));
        suppress_ = false;
    }

    void AdvancedPage::syncAlwaysOn()
    {
        if (!alwaysOnSwitch_ || !host_) {
            return;
        }
        // suppress_ keeps the programmatic toggle from submitting a change.
        suppress_ = true;
        alwaysOnSwitch_.IsOn(host_->currentSettings().always_on_camera);
        suppress_ = false;
    }

    void AdvancedPage::startPolling()
    {
        if (!poll_) {
            poll_ = DispatcherQueue().CreateTimer();
            poll_.Interval(winrt::Windows::Foundation::TimeSpan{ kOneSecondTicks });
            poll_.Tick([this](auto const&, auto const&) { updateReport(); });
        }
        poll_.Start();
    }

    void AdvancedPage::updateReport()
    {
        if (!host_ || !inUseText_ || !noticeBar_) {
            return;
        }
        const telaviva::InferenceReport report = host_->inferenceReport();
        inUseText_.Text(report.decided
            ? fillPlaceholder(loc("inference.inUse"), utf8(report.activeLabel))
            : loc("inference.notDecided"));

        const auto key = telaviva::inferenceNoticeKey(report);
        if (key) {
            noticeBar_.Message(loc(*key));
            noticeBar_.IsOpen(true);
        }
        else {
            noticeBar_.IsOpen(false);
        }
    }
}
