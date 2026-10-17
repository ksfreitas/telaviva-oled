// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "AutomaticBrightnessPage.g.h"
#include "pch.h"
#include "Host/SettingsHost.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace winrt::TelaViva::implementation
{
    struct AutomaticBrightnessPage : AutomaticBrightnessPageT<AutomaticBrightnessPage>
    {
        AutomaticBrightnessPage();

        // Implementation-only: not part of the IDL, reached through get_self.
        void SetHost(telaviva::host::ISettingsHost* host);
        // Rebuilds the rows from Str() and keeps the last notice.
        void RefreshStrings();
        // Ends an active calibration with the given reason. Does nothing when none is active, so each end runs once.
        void EndCalibration(telaviva::CalibrationEndReason reason);

    private:
        // Starts a calibration at a monitor percent and shows its dialog when the controller accepted it.
        void startCalibration(const std::string& monitorId, int point);
        void showCalibrationDialog(int point);
        void startPolling();
        void onPoll();
        void setFollow(bool enabled);
        void updateAvailability(const telaviva::host::LaptopStatus& laptop, bool followOn);
        void updateNotice();
        // Everything that changes the monitor rows. The poll rebuilds them only when this changes, so a
        // half-typed add value is not lost on every tick.
        std::string layoutSignature(const telaviva::Snapshot& snapshot, const telaviva::host::LaptopStatus& laptop,
                                    const telaviva::host::CalibrationView& view);
        void buildMonitors(const telaviva::Snapshot& snapshot, const telaviva::host::LaptopStatus& laptop,
                           const telaviva::host::CalibrationView& view, std::string signature);
        winrt::Microsoft::UI::Xaml::Controls::StackPanel buildCalibrationPanel(const std::string& monitorId,
                                                                               const telaviva::host::CalibrationView& view);
        winrt::Microsoft::UI::Xaml::FrameworkElement buildPointRow(const std::string& monitorId,
                                                                   const telaviva::host::CurvePointView& point,
                                                                   const telaviva::host::CalibrationView& view);
        winrt::Microsoft::UI::Xaml::FrameworkElement buildAddRow(const std::string& monitorId,
                                                                 const std::vector<telaviva::host::CurvePointView>& points);

        telaviva::host::ISettingsHost* host_ = nullptr;
        // Revision the rows were built from; 0 until the first snapshot has arrived.
        std::uint64_t builtRevision_ = 0;
        // Layout the monitor rows were built for; see layoutSignature.
        std::string layoutSignature_;
        // The notice shown under the monitor list. Kept until the next calibration starts.
        std::optional<std::string> lastNotice_;

        winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch followSwitch_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::InfoBar availabilityBar_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel monitorsPanel_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::InfoBar noticeBar_{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer poll_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct AutomaticBrightnessPage : AutomaticBrightnessPageT<AutomaticBrightnessPage, implementation::AutomaticBrightnessPage>
    {
    };
}
