// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "CalibrationDialog.xaml.h"
#include "Host/Strings.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#if __has_include("CalibrationDialog.g.cpp")
#include "CalibrationDialog.g.cpp"
#endif

namespace {

namespace xc = winrt::Microsoft::UI::Xaml::Controls;

// 250 ms in 100 ns ticks. The controller state is read this often, which keeps the anchor line current.
constexpr std::int64_t kRefreshTicks = 2'500'000;

winrt::hstring loc(std::string_view key)
{
    return winrt::hstring{ telaviva::host::Str(key) };
}

// Fills the printf-style %d placeholders of a resource string in order. %% is a literal percent sign.
std::wstring fillInts(std::wstring_view text, std::initializer_list<int> values)
{
    std::wstring out;
    auto next = values.begin();
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'%' && i + 1 < text.size()) {
            if (text[i + 1] == L'd' && next != values.end()) {
                out += std::to_wstring(*next);
                ++next;
                ++i;
                continue;
            }
            if (text[i + 1] == L'%') {
                out += L'%';
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::wstring percentText(double value)
{
    return std::to_wstring(static_cast<int>(std::lround(value))) + L"%";
}

}  // namespace

namespace winrt::TelaViva::implementation
{
    CalibrationDialog::CalibrationDialog()
    {
        InitializeComponent();
    }

    void CalibrationDialog::Start(telaviva::host::ISettingsHost* host, int point)
    {
        host_ = host;
        Title(winrt::box_value(winrt::hstring{ fillInts(telaviva::host::Str("calibration.dialog.title"), { point }) }));
        HelpText().Text(loc("calibration.help"));
        WaitingText().Text(loc("calibration.waiting"));
        PrimaryButtonText(loc("calibration.done"));
        CloseButtonText(loc("calibration.cancel"));
        IsPrimaryButtonEnabled(false);

        // Done saves the point and ends the session. The check covers a session the controller ended already.
        PrimaryButtonClick([this](auto const&, auto const&) {
            if (host_ && host_->calibrationView().active) {
                host_->confirmCalibration();
                host_->endCalibration(telaviva::CalibrationEndReason::Done);
            }
        });

        // Cancel, Escape and a system close all land here. Done has already ended the session by now.
        Closed([this](auto const&, auto const&) {
            if (timer_) {
                timer_.Stop();
            }
            if (host_ && host_->calibrationView().active) {
                host_->endCalibration(telaviva::CalibrationEndReason::Cancelled);
            }
        });

        // The slider is the only control in the dialog, so it needs its own name for Narrator.
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(LaptopSlider(), loc("calibration.slider"));
        LaptopSlider().ValueChanged([this](auto const& sender, auto const&) {
            if (suppress_ || !host_) {
                return;
            }
            const double value = sender.as<xc::Slider>().Value();
            ValueText().Text(winrt::hstring{ percentText(value) });
            host_->setCalibrationSlider(value);
        });

        timer_ = DispatcherQueue().CreateTimer();
        timer_.Interval(winrt::Windows::Foundation::TimeSpan{ kRefreshTicks });
        timer_.Tick([this](auto const&, auto const&) { refresh(); });
        refresh();
        timer_.Start();
    }

    void CalibrationDialog::refresh()
    {
        if (!host_) {
            return;
        }
        const telaviva::host::CalibrationView view = host_->calibrationView();
        if (!view.active) {
            // The controller ended the session (a write failed, or the Laptop went away). Its notice shows on the page.
            if (timer_) {
                timer_.Stop();
            }
            Hide();
            return;
        }

        // The range moves before the value, so the slider never holds a minimum above its maximum.
        suppress_ = true;
        const double lower = view.lower;
        const double upper = view.upper;
        if (lower > LaptopSlider().Maximum()) {
            LaptopSlider().Maximum(upper);
            LaptopSlider().Minimum(lower);
        } else {
            LaptopSlider().Minimum(lower);
            LaptopSlider().Maximum(upper);
        }
        // A zero-width range has nothing to adjust.
        LaptopSlider().IsEnabled(upper > lower);
        if (LaptopSlider().Value() != view.sliderValue) {
            LaptopSlider().Value(view.sliderValue);
        }
        suppress_ = false;

        ValueText().Text(winrt::hstring{ percentText(view.sliderValue) });
        // Until the monitor is within 2 points of this brightness, the point cannot be saved.
        WaitingText().Visibility(view.anchorReached ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                                                    : winrt::Microsoft::UI::Xaml::Visibility::Visible);
        IsPrimaryButtonEnabled(view.anchorReached);
    }
}
