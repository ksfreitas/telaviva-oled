// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "MonitorCard.g.h"
#include "pch.h"
#include "Host/FlyoutState.h"

namespace winrt::TelaViva::implementation
{
    struct MonitorCard : MonitorCardT<MonitorCard>
    {
        MonitorCard();

        // Implementation-only: not part of the IDL, reached through get_self.
        void Bind(telaviva::host::IFlyoutActions* actions);
        void Update(const telaviva::host::MonitorCardState& state);
        // Implementation-only: changes only when a row, line or text that can change the card height changes.
        size_t LayoutSignature();

    private:
        void applyProtectionSlider(const telaviva::host::MonitorCardState& state, const std::wstring& accessibleName);

        telaviva::host::IFlyoutActions* actions_ = nullptr;
        std::string id_;
        std::wstring name_;
        // Set while Update writes values, so ValueChanged from Update is not a user change.
        bool updating_ = false;
        // Pointer and key use are tracked apart, so releasing one does not clear the other.
        bool brightnessPointer_ = false;
        bool brightnessKey_ = false;
        bool volumePointer_ = false;
        bool volumeKey_ = false;
        bool protectionPointer_ = false;
        bool protectionKey_ = false;
        // Set from the state in Update: the card is the built-in screen, so its brightness goes to the panel.
        bool builtin_ = false;
        // Tick of the last slider release (0 when none). Values from states built before the release are held off briefly.
        ULONGLONG lastReleaseTick_ = 0;
    };
}

namespace winrt::TelaViva::factory_implementation
{
    struct MonitorCard : MonitorCardT<MonitorCard, implementation::MonitorCard>
    {
    };
}
