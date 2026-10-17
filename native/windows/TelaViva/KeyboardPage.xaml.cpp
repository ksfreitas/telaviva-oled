// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "KeyboardPage.xaml.h"
#include "Host/SettingRow.h"
#include "Host/Strings.h"
#include "Platform/SettingsModel.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#if __has_include("KeyboardPage.g.cpp")
#include "KeyboardPage.g.cpp"
#endif

namespace {

namespace xaml = winrt::Microsoft::UI::Xaml;
namespace xc = winrt::Microsoft::UI::Xaml::Controls;

// Mirrors the core limit in crates/telaviva-core/src/settings.rs validate().
constexpr double kVolumeStepMin = 1.0;
constexpr double kVolumeStepMax = 15.0;

// One second in 100 ns ticks, the unit of TimeSpan.
constexpr std::int64_t kOneSecondTicks = 10'000'000;

winrt::hstring loc(std::string_view key)
{
    return winrt::hstring{ telaviva::host::Str(key) };
}

// The core's stored step, held inside the control's range so the control never rejects it.
double clampedStep(int step)
{
    return std::clamp(static_cast<double>(step), kVolumeStepMin, kVolumeStepMax);
}

// Numeric entry for the volume step. Inline spin buttons: the row is short and the step is a single small number.
xc::NumberBox makeVolumeBox(double value)
{
    xc::NumberBox box;
    box.Minimum(kVolumeStepMin);
    box.Maximum(kVolumeStepMax);
    box.SmallChange(1.0);
    box.LargeChange(5.0);
    box.Value(value);
    box.SpinButtonPlacementMode(xc::NumberBoxSpinButtonPlacementMode::Inline);
    box.ValidationMode(xc::NumberBoxValidationMode::InvalidInputOverwritten);
    box.Width(120);
    return box;
}

// Places a unit caption after a control. The unit carries its own space (" pt"), so it reads "N pt".
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

}  // namespace

namespace winrt::TelaViva::implementation
{
    KeyboardPage::KeyboardPage()
    {
        InitializeComponent();
        Loaded([this](auto const&, auto const&) {
            startPolling();
            // Programmatic focus on the first control, so arriving from the navigation pane lands in the page.
            if (keysToggle_) {
                static_cast<void>(keysToggle_.Focus(xaml::FocusState::Programmatic));
            }
        });
        Unloaded([this](auto const&, auto const&) {
            if (poll_) {
                poll_.Stop();
            }
        });
        RefreshStrings();
    }

    void KeyboardPage::SetHost(telaviva::host::ISettingsHost* host)
    {
        host_ = host;
        RefreshStrings();
    }

    void KeyboardPage::RefreshStrings()
    {
        PageTitle().Text(loc("settings.tab.keyboard"));

        // Rebuilt rather than edited in place, so the header and help text follow the language.
        RowsPanel().Children().Clear();
        const telaviva::Snapshot snapshot = host_ ? host_->latestSnapshot() : telaviva::Snapshot{};
        const telaviva::SettingsSnapshot& settings = snapshot.settings;
        revision_ = snapshot.revision;

        RowsPanel().Children().Append(telaviva::host::MakeSectionHeader(loc("hardwareKeys.title")));

        // Volume and mute keys only. Brightness keys keep their Windows behavior, so no brightness row.
        const winrt::hstring toggleHeader = loc("hardwareKeys.toggle");
        keysToggle_ = makeSwitch(settings.hardware_keys_enabled, toggleHeader);
        keysToggle_.Toggled([this](auto const& sender, auto const&) {
            const bool enabled = sender.as<xc::ToggleSwitch>().IsOn();
            if (volumeBox_) {
                volumeBox_.IsEnabled(enabled);
            }
            if (suppress_) {
                return;
            }
            edit([enabled](telaviva::SettingsDraft& draft) { draft.hardwareKeysEnabled = enabled; });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(
            toggleHeader, loc("hardwareKeys.help"), keysToggle_));

        // Status under the toggle: off, active, or unavailable with what Windows still does with the keys.
        keysStatus_ = xc::TextBlock();
        keysStatus_.TextWrapping(xaml::TextWrapping::Wrap);
        keysStatus_.FontSize(12.0);
        RowsPanel().Children().Append(keysStatus_);
        updateKeysStatus();

        // Step per press. Disabled while the keys are not routed, as on macOS.
        volumeBox_ = makeVolumeBox(clampedStep(settings.volume_key_step));
        volumeBox_.IsEnabled(settings.hardware_keys_enabled);
        xaml::Automation::AutomationProperties::SetName(volumeBox_, loc("hardwareKeys.volumeStep"));
        volumeBox_.ValueChanged([this](xc::NumberBox const&, xc::NumberBoxValueChangedEventArgs const& args) {
            const double value = args.NewValue();
            if (suppress_ || std::isnan(value)) {
                return;
            }
            edit([value](telaviva::SettingsDraft& draft) { draft.volumeKeyStep = static_cast<int>(std::lround(value)); });
        });
        RowsPanel().Children().Append(telaviva::host::MakeSettingRow(
            loc("hardwareKeys.volumeStep"), winrt::hstring{}, withUnit(volumeBox_, L" pt")));
    }

    void KeyboardPage::edit(const std::function<void(telaviva::SettingsDraft&)>& change)
    {
        if (!host_) {
            return;
        }
        auto draft = telaviva::draftFromSnapshotSettings(host_->currentSettings());
        change(draft);
        host_->submit(telaviva::buildSettingsPayload(draft));
    }

    void KeyboardPage::startPolling()
    {
        if (!poll_) {
            poll_ = DispatcherQueue().CreateTimer();
            poll_.Interval(winrt::Windows::Foundation::TimeSpan{ kOneSecondTicks });
            poll_.Tick([this](auto const&, auto const&) { onPoll(); });
        }
        poll_.Start();
    }

    void KeyboardPage::onPoll()
    {
        if (!host_) {
            return;
        }
        updateKeysStatus();
        const telaviva::Snapshot snapshot = host_->latestSnapshot();
        if (revision_ == 0 && snapshot.revision != 0) {
            // Built before the first snapshot arrived, so the controls hold placeholder values.
            RefreshStrings();
            return;
        }
        // Syncs only when the revision moves, so a change the user just made is not reverted
        // by the poll before the core has accepted it.
        if (snapshot.revision == revision_) {
            return;
        }
        revision_ = snapshot.revision;
        syncControls(snapshot.settings);
    }

    void KeyboardPage::updateKeysStatus()
    {
        if (!host_ || !keysStatus_) {
            return;
        }
        std::string_view key = "hardwareKeys.status.disabled";
        switch (host_->keysState()) {
        case telaviva::host::KeysState::Active:
            key = "hardwareKeys.status.enabled";
            break;
        case telaviva::host::KeysState::Unavailable:
            key = "hardwareKeys.status.listenerUnavailable";
            break;
        case telaviva::host::KeysState::Off:
            break;
        }
        keysStatus_.Text(loc(key));
    }

    void KeyboardPage::syncControls(const telaviva::SettingsSnapshot& settings)
    {
        suppress_ = true;
        if (keysToggle_ && keysToggle_.IsOn() != settings.hardware_keys_enabled) {
            keysToggle_.IsOn(settings.hardware_keys_enabled);
        }
        if (volumeBox_) {
            const double step = clampedStep(settings.volume_key_step);
            if (volumeBox_.Value() != step) {
                volumeBox_.Value(step);
            }
            volumeBox_.IsEnabled(settings.hardware_keys_enabled);
        }
        suppress_ = false;
    }
}
