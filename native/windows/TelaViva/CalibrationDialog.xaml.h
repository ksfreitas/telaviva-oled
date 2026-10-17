// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "CalibrationDialog.g.h"
#include "pch.h"
#include "Host/SettingsHost.h"

namespace winrt::TelaViva::implementation
{
    struct CalibrationDialog : CalibrationDialogT<CalibrationDialog>
    {
        CalibrationDialog();

        // Implementation-only: not part of the IDL, reached through get_self. Titles the dialog for the
        // point and starts the refresh. Call only while a calibration is active.
        void Start(telaviva::host::ISettingsHost* host, int point);

    private:
        // Copies the controller state into the controls. Closes the dialog when the calibration ended elsewhere.
        void refresh();

        telaviva::host::ISettingsHost* host_ = nullptr;
        // Set while the slider is written in code, so the write is not sent back to the controller.
        bool suppress_ = false;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct CalibrationDialog : CalibrationDialogT<CalibrationDialog, implementation::CalibrationDialog>
    {
    };
}
