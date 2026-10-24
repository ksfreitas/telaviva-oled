// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "MonitorCard.xaml.h"
#include "Host/Strings.h"
#include "Platform/ProtectionRules.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>

#include <cmath>
#include <functional>
#include <string>
#include <string_view>

#if __has_include("MonitorCard.g.cpp")
#include "MonitorCard.g.cpp"
#endif

namespace winrt::TelaViva::implementation
{
    namespace {
        namespace xaml = winrt::Microsoft::UI::Xaml;
        namespace xc = winrt::Microsoft::UI::Xaml::Controls;

        winrt::hstring valueText(double value)
        {
            return winrt::hstring{ std::to_wstring(std::lround(value)) };
        }

        // Also exposes the percent to UI Automation as the item status, since a Slider has no value text of its own.
        void updateValueTip(xc::Slider const& slider, double value)
        {
            xc::ToolTipService::SetToolTip(slider, winrt::box_value(valueText(value)));
            xaml::Automation::AutomationProperties::SetItemStatus(slider,
                winrt::hstring{ std::to_wstring(std::lround(value)) + L"%" });
        }

        // Shows the rounded value as N% beside the slider.
        void setValueLabel(xc::TextBlock const& label, double value)
        {
            label.Text(winrt::hstring{ std::to_wstring(std::lround(value)) + L"%" });
        }

        // A state built during a drag can hold a value older than the thumb, so values are held off this long after a release.
        constexpr ULONGLONG kReleaseHoldOffMs = 300;

        bool withinReleaseHoldOff(ULONGLONG releasedAt)
        {
            return releasedAt != 0 && GetTickCount64() - releasedAt < kReleaseHoldOffMs;
        }

        bool isSliderKey(winrt::Windows::System::VirtualKey key)
        {
            using winrt::Windows::System::VirtualKey;
            switch (key) {
            case VirtualKey::Left:
            case VirtualKey::Right:
            case VirtualKey::Up:
            case VirtualKey::Down:
            case VirtualKey::Home:
            case VirtualKey::End:
            case VirtualKey::PageUp:
            case VirtualKey::PageDown:
                return true;
            default:
                return false;
            }
        }

        // Marks the slider busy while the pointer is down or a navigation key is held, so
        // Update does not overwrite the value the user is setting. Handlers run even when
        // the slider has already handled the event. Each release stamps releasedAt for the hold-off.
        void hookBusy(xc::Slider const& slider, bool& pointer, bool& key, ULONGLONG& releasedAt)
        {
            slider.AddHandler(xaml::UIElement::PointerPressedEvent(),
                winrt::box_value(xaml::Input::PointerEventHandler([&pointer](auto const&, auto const&) { pointer = true; })), true);
            auto release = [&pointer, &releasedAt](auto const&, auto const&) { pointer = false; releasedAt = GetTickCount64(); };
            slider.AddHandler(xaml::UIElement::PointerReleasedEvent(), winrt::box_value(xaml::Input::PointerEventHandler(release)), true);
            slider.AddHandler(xaml::UIElement::PointerCaptureLostEvent(), winrt::box_value(xaml::Input::PointerEventHandler(release)), true);
            slider.AddHandler(xaml::UIElement::PointerCanceledEvent(), winrt::box_value(xaml::Input::PointerEventHandler(release)), true);

            slider.AddHandler(xaml::UIElement::KeyDownEvent(),
                winrt::box_value(xaml::Input::KeyEventHandler([&key](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
                    if (isSliderKey(args.Key())) { key = true; }
                })), true);
            slider.AddHandler(xaml::UIElement::KeyUpEvent(),
                winrt::box_value(xaml::Input::KeyEventHandler([&key, &releasedAt](auto const&, auto const&) {
                    key = false;
                    releasedAt = GetTickCount64();
                })), true);
            slider.LostFocus([&key, &releasedAt](auto const&, auto const&) { key = false; releasedAt = GetTickCount64(); });
        }

        // Sets one slider from the state. A busy slider keeps the user's value.
        void applySlider(xc::Slider const& slider, xc::TextBlock const& valueLabel,
            xc::TextBlock const& failedText, xc::TextBlock const& reason,
            const telaviva::host::SliderState& state, bool controllable,
            const std::wstring& accessibleName, bool busy)
        {
            slider.IsEnabled(controllable && state.available);
            if (!busy) {
                slider.Value(state.value);
            }
            xaml::Automation::AutomationProperties::SetName(slider, winrt::hstring{ accessibleName });
            updateValueTip(slider, slider.Value());
            setValueLabel(valueLabel, slider.Value());

            failedText.Visibility(state.failed ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);

            const bool showReason = !state.available && !state.reason.empty();
            reason.Text(winrt::hstring{ state.reason });
            reason.Visibility(showReason ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        }

        // Clears the flag on every exit path, including exceptions from WinRT calls.
        class FlagScope
        {
        public:
            explicit FlagScope(bool& flag) : flag_(flag) { flag_ = true; }
            ~FlagScope() { flag_ = false; }
            FlagScope(const FlagScope&) = delete;
            FlagScope& operator=(const FlagScope&) = delete;

        private:
            bool& flag_;
        };
    }

    MonitorCard::MonitorCard()
    {
        InitializeComponent();

        // Static text is set in code, not in XAML (ADR 0015).
        const winrt::hstring failedText{ telaviva::host::Str("flyout.failed") };
        BrightnessFailed().Text(failedText);
        VolumeFailed().Text(failedText);

        const winrt::hstring protectionText{ telaviva::host::Str("protection.monitor.toggle") };
        ProtectionToggleText().Text(protectionText);
        xc::ToolTipService::SetToolTip(ProtectionToggle(), winrt::box_value(protectionText));
        ProtectionLabel().Text(winrt::hstring{ telaviva::host::Str("protectionBrightness") });
        FollowedText().Text(winrt::hstring{ telaviva::host::Str("ambient.toggle") });

        hookBusy(BrightnessSlider(), brightnessPointer_, brightnessKey_, lastReleaseTick_);
        hookBusy(VolumeSlider(), volumePointer_, volumeKey_, lastReleaseTick_);
        hookBusy(ProtectionSlider(), protectionPointer_, protectionKey_, lastReleaseTick_);

        // Hooked after InitializeComponent so the initial value does not reach the actions.
        BrightnessSlider().ValueChanged([this](auto const&, auto const& args) {
            const double value = args.NewValue();
            updateValueTip(BrightnessSlider(), value);
            setValueLabel(BrightnessValue(), value);
            if (!updating_ && actions_ != nullptr) {
                if (builtin_) {
                    actions_->setBuiltinBrightness(value);
                } else {
                    actions_->setBrightness(id_, value);
                }
            }
        });
        VolumeSlider().ValueChanged([this](auto const&, auto const& args) {
            const double value = args.NewValue();
            updateValueTip(VolumeSlider(), value);
            setValueLabel(VolumeValue(), value);
            if (!updating_ && actions_ != nullptr) {
                actions_->setVolume(id_, value);
            }
        });
        ProtectionToggle().Toggled([this](auto const& sender, auto const&) {
            if (!updating_ && actions_ != nullptr) {
                actions_->setMonitorProtected(id_, sender.as<xc::ToggleSwitch>().IsOn());
            }
        });
        ProtectionSlider().ValueChanged([this](auto const&, auto const& args) {
            const double value = args.NewValue();
            updateValueTip(ProtectionSlider(), value);
            setValueLabel(ProtectionValue(), value);
            if (!updating_ && actions_ != nullptr) {
                actions_->setProtectionBrightness(id_, value);
            }
        });
    }

    void MonitorCard::Bind(telaviva::host::IFlyoutActions* actions)
    {
        actions_ = actions;
    }

    void MonitorCard::Update(const telaviva::host::MonitorCardState& state)
    {
        FlagScope scope(updating_);

        id_ = state.id;
        name_ = state.name;
        builtin_ = state.isBuiltin;
        NameText().Text(winrt::hstring{ name_ });
        StatusText().Text(winrt::hstring{ state.status });

        // The built-in screen has no volume, so its volume row is left out.
        VolumeRow().Visibility(state.hideVolume ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
        NoticeText().Text(winrt::hstring{ state.notice });
        NoticeText().Visibility(state.notice.empty() ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);

        // A followed monitor is driven by the Laptop panel, so the brightness row gives way to a caption.
        BrightnessRow().Visibility(state.followed ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
        FollowedRow().Visibility(state.followed ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);

        // Each control names its monitor after its label, as "Brightness, <monitor>", so Narrator tells cards apart.
        const std::wstring brightnessLabel = telaviva::host::Str("flyout.brightness");
        const std::wstring volumeLabel = telaviva::host::Str("flyout.volume");

        // Values are held off for a moment after any release (see withinReleaseHoldOff).
        const bool holdOff = withinReleaseHoldOff(lastReleaseTick_);
        applySlider(BrightnessSlider(), BrightnessValue(), BrightnessFailed(), BrightnessReason(),
            state.brightness, state.controllable, brightnessLabel + L", " + name_,
            brightnessPointer_ || brightnessKey_ || holdOff);
        applySlider(VolumeSlider(), VolumeValue(), VolumeFailed(), VolumeReason(),
            state.volume, state.controllable, volumeLabel + L", " + name_,
            volumePointer_ || volumeKey_ || holdOff);

        ProtectionToggleRow().Visibility(state.protectionToggleVisible ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        // The built-in screen explains the toggle with its own help text; monitors keep the generic one.
        xc::ToolTipService::SetToolTip(ProtectionToggle(), winrt::box_value(winrt::hstring{
            telaviva::host::Str(state.isBuiltin ? "builtin.protection.toggle.help" : "protection.monitor.toggle") }));
        ProtectionToggle().IsOn(state.protectionOn);
        xaml::Automation::AutomationProperties::SetName(ProtectionToggle(),
            winrt::hstring{ telaviva::host::Str("protection.monitor.toggle") + L", " + name_ });
        ProtectionPanel().Visibility(state.protectionRowVisible ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        if (state.protectionRowVisible) {
            applyProtectionSlider(state, telaviva::host::Str("protectionBrightness") + L", " + name_);
        }
    }

    // Sets the protection slider from the stored target, clamped into 1..50. The clamped value is
    // saved only when the clamp moved it, the monitor has brightness, and the user is not already moving the slider.
    void MonitorCard::applyProtectionSlider(const telaviva::host::MonitorCardState& state, const std::wstring& accessibleName)
    {
        ProtectionSlider().Minimum(telaviva::protectionSliderMinimum());
        ProtectionSlider().Maximum(telaviva::protectionSliderMaximum());
        ProtectionSlider().IsEnabled(telaviva::protectionSliderEnabled(state.protectionBrightnessSupported));
        const telaviva::ProtectionClamp clamp = telaviva::clampProtectionBrightness(state.protectionValue);
        // A stale state must not save a clamped value right after the release either.
        const bool busy = protectionPointer_ || protectionKey_ || withinReleaseHoldOff(lastReleaseTick_);
        if (!busy) {
            ProtectionSlider().Value(clamp.value);
        }
        xaml::Automation::AutomationProperties::SetName(ProtectionSlider(), winrt::hstring{ accessibleName });
        updateValueTip(ProtectionSlider(), ProtectionSlider().Value());
        setValueLabel(ProtectionValue(), ProtectionSlider().Value());

        if (clamp.changed && state.protectionBrightnessSupported && !busy && actions_ != nullptr) {
            actions_->setProtectionBrightness(id_, clamp.value);
        }
    }

    // Compared by PopupWindow to skip Relayout when no row, line or text that affects height changed.
    size_t MonitorCard::LayoutSignature()
    {
        size_t bits = 0;
        auto bit = [&bits](bool on) { bits = (bits << 1) | (on ? 1u : 0u); };
        bit(BrightnessRow().Visibility() == xaml::Visibility::Visible);
        bit(FollowedRow().Visibility() == xaml::Visibility::Visible);
        bit(ProtectionToggleRow().Visibility() == xaml::Visibility::Visible);
        bit(ProtectionPanel().Visibility() == xaml::Visibility::Visible);
        bit(BrightnessFailed().Visibility() == xaml::Visibility::Visible);
        bit(VolumeFailed().Visibility() == xaml::Visibility::Visible);
        bit(BrightnessReason().Visibility() == xaml::Visibility::Visible);
        bit(VolumeReason().Visibility() == xaml::Visibility::Visible);
        bit(VolumeRow().Visibility() == xaml::Visibility::Visible);
        bit(NoticeText().Visibility() == xaml::Visibility::Visible);

        // Wrapping depends on the text, so the texts that can wrap are folded in as well.
        const winrt::hstring name = NameText().Text();
        const winrt::hstring status = StatusText().Text();
        const winrt::hstring brightnessReason = BrightnessReason().Text();
        const winrt::hstring volumeReason = VolumeReason().Text();
        const winrt::hstring notice = NoticeText().Text();
        const std::hash<std::wstring_view> hash;
        size_t key = bits;
        for (const std::wstring_view text : { std::wstring_view{ name }, std::wstring_view{ status },
                 std::wstring_view{ brightnessReason }, std::wstring_view{ volumeReason },
                 std::wstring_view{ notice } }) {
            key = key * 31 + hash(text);
        }
        return key;
    }
}
