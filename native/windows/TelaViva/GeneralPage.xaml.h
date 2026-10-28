// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "GeneralPage.g.h"
#include "pch.h"
#include "Host/SettingsHost.h"
#include "Platform/MonitoredDisplayRules.h"
#include "Platform/SettingsModel.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace winrt::TelaViva::implementation
{
    struct GeneralPage : GeneralPageT<GeneralPage>
    {
        GeneralPage();

        // Implementation-only: not part of the IDL, reached through get_self.
        void SetHost(telaviva::host::ISettingsHost* host);
        // Rebuilds the rows from Str() and keeps the current selections.
        void RefreshStrings();
        // Called after the language changes; runs on the UI thread, after the change handler returns.
        void SetOnLanguageChanged(std::function<void()> callback);

    private:
        // One screensaver target: its slider and readout, updated in place by the poll.
        struct TargetRow {
            std::string monitorId;
            winrt::Microsoft::UI::Xaml::Controls::Slider slider{ nullptr };
            winrt::Microsoft::UI::Xaml::Controls::TextBlock readout{ nullptr };
        };

        // Loads the settings once, when the host is set and the page is shown.
        void tryLoad();
        void notifyLanguageChanged();
        // Applies one change to a fresh draft of the current settings and submits the full payload.
        void edit(const std::function<void(telaviva::SettingsDraft&)>& change);
        // Starts the one-second poll that keeps camera status, cameras and monitors current.
        void startPolling();
        void onPoll();
        // Enumerates cameras on the thread pool; the poll reads the cached list.
        void reloadCameras();
        void syncCameras(const std::optional<std::string>& selected);
        void fillCameraBox(const std::optional<std::string>& selected);
        void updateCameraStatus(const telaviva::Snapshot& snapshot);
        void buildMonitorRows(const telaviva::Snapshot& snapshot);
        // Adds one protection brightness row, named after its display; the caller looks up the stored value.
        void addTargetRow(const std::string& id, const winrt::hstring& name, double stored, bool brightnessSupported);
        void updateTargets(const telaviva::Snapshot& snapshot);
        // Rebuilds the monitored display picker when its choices change, and keeps its selection and the threshold in step.
        void fillMonitoredBox(const telaviva::Snapshot& snapshot);
        void syncMonitoredDisplay(const telaviva::Snapshot& snapshot);
        // Shows the presence threshold of the monitored display, or the global value when none resolves.
        void refreshThreshold(const telaviva::Snapshot& snapshot);
        // Sets the keep-awake status line: off shows the disabled text, on shows the host's state.
        void updateKeepAwakeStatus(bool on);
        // Runs the keep-awake status refresh only while the switch is on and the page is loaded.
        void setKeepAwakeTimerRunning(bool running);

        telaviva::host::ISettingsHost* host_ = nullptr;
        std::function<void()> onLanguageChanged_;
        // Index into the language picker: 0 auto, 1 en-US, 2 pt-BR.
        int localeIndex_ = 0;
        bool launchAtLogin_ = false;
        bool loaded_ = false;
        // Set while the toggle is reverted in code, so the revert does not re-run the handler.
        bool suppressToggle_ = false;
        // Set while a control is written in code (combo box refill, slider clamp), so the write is not submitted.
        bool suppress_ = false;
        // Revision the rows were built from; 0 until the first snapshot has arrived.
        std::uint64_t builtRevision_ = 0;
        // Connected monitors (id and name) the monitor rows were built for.
        std::string monitorSignature_;
        // Selectable cameras in the picker, in the same order as its items after the default.
        std::vector<telaviva::CameraInfo> cameras_;
        std::vector<TargetRow> targets_;
        // Choices of the monitored display picker, in the same order as its items.
        std::vector<telaviva::MonitoredDisplayItem> monitoredItems_;
        // Picker choices the picker was built for: built-in visibility and each monitor's id, name and usability.
        std::string monitoredPickerSignature_;
        // Display whose presence threshold the box shows; empty for the global value.
        std::string thresholdTarget_;

        winrt::Microsoft::UI::Xaml::Controls::ComboBox languageBox_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ComboBox cameraBox_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ComboBox monitoredBox_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::NumberBox thresholdBox_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel monitorsPanel_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel targetsPanel_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock cameraStatusText_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::InfoBar cameraInfoBar_{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::HyperlinkButton cameraLink_{ nullptr };
        // Keep-awake status under its switch, and the timer that refreshes it while the switch is on.
        winrt::Microsoft::UI::Xaml::Controls::TextBlock keepAwakeStatus_{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer keepAwakeTimer_{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer poll_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct GeneralPage : GeneralPageT<GeneralPage, implementation::GeneralPage>
    {
    };
}
