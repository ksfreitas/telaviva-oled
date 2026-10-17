#include "PowerEvents.h"

#include <cstddef>
#include <cstring>
#include <utility>

namespace telaviva {

namespace {

// Local copy so the link does not depend on which SDK headers define the GUID.
constexpr GUID kAcDcPowerSource = {
    0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};

// Local copy for the same reason as kAcDcPowerSource.
constexpr GUID kLidSwitchStateChange = {
    0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3}};

// Values of the GUID_ACDC_POWER_SOURCE setting: 0 AC, 1 DC, 2 hot (a UPS).
constexpr DWORD kPoAc = 0;
constexpr DWORD kPoDc = 1;
constexpr DWORD kPoHot = 2;

// Values of the GUID_LIDSWITCH_STATE_CHANGE setting.
constexpr DWORD kLidClosed = 0;
constexpr DWORD kLidOpen = 1;

constexpr uint8_t kAcOffline = 0;
constexpr uint8_t kAcOnline = 1;
constexpr uint8_t kAcUnknown = 255;
// BatteryFlag 128 means the system has no battery.
constexpr uint8_t kNoSystemBattery = 128;

// Handles from the last registerFor.
HPOWERNOTIFY gAcDcNotify = nullptr;
HPOWERNOTIFY gLidNotify = nullptr;

bool sameGuid(const GUID& a, const GUID& b)
{
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// The payload of POWERBROADCAST_SETTING is a variable-length array declared as
// Data[1], so it is read by offset rather than by index.
DWORD readPowerValue(const POWERBROADCAST_SETTING* setting)
{
    DWORD value = 0;
    std::memcpy(&value, reinterpret_cast<const unsigned char*>(setting) + offsetof(POWERBROADCAST_SETTING, Data),
                sizeof value);
    return value;
}

}  // namespace

bool SystemPowerStatusReader::read(uint8_t& acLineStatus, uint8_t& batteryFlag)
{
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status) == FALSE) {
        return false;
    }
    acLineStatus = status.ACLineStatus;
    batteryFlag = status.BatteryFlag;
    return true;
}

std::optional<bool> onBatteryFromStatus(uint8_t acLineStatus, uint8_t batteryFlag)
{
    switch (acLineStatus) {
    case kAcOnline:
        return false;
    case kAcOffline:
        return true;
    case kAcUnknown:
        // A desktop without a battery always runs on mains power, so this is
        // not unknown. A laptop that reports unknown AC stays unknown.
        if (batteryFlag == kNoSystemBattery) {
            return false;
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

PowerEvents::PowerEvents(IPowerStatusReader& reader, PowerEventCallbacks callbacks)
    : reader_(reader),
      callbacks_(std::move(callbacks))
{
}

void PowerEvents::publishCurrent()
{
    std::optional<bool> onBattery;
    uint8_t acLineStatus = kAcUnknown;
    uint8_t batteryFlag = kAcUnknown;
    if (reader_.read(acLineStatus, batteryFlag)) {
        onBattery = onBatteryFromStatus(acLineStatus, batteryFlag);
    }
    if (callbacks_.onPower) {
        callbacks_.onPower(onBattery);
    }
}

std::optional<LRESULT> PowerEvents::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    try {
        if (message != WM_POWERBROADCAST) {
            return std::nullopt;
        }
        if (wParam == static_cast<WPARAM>(PBT_APMRESUMEAUTOMATIC)) {
            // The source may have changed while asleep. Other handlers must see resume too.
            publishCurrent();
            return std::nullopt;
        }
        if (wParam != static_cast<WPARAM>(PBT_POWERSETTINGCHANGE) || lParam == 0) {
            return std::nullopt;
        }
        const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
        if (sameGuid(setting->PowerSetting, kLidSwitchStateChange)) {
            if (setting->DataLength != static_cast<DWORD>(sizeof(DWORD))) {
                return std::nullopt;
            }
            const DWORD lid = readPowerValue(setting);
            if (lid != kLidClosed && lid != kLidOpen) {
                return std::nullopt;
            }
            if (callbacks_.onLid) {
                callbacks_.onLid(lid == kLidOpen);
            }
            return static_cast<LRESULT>(TRUE);
        }
        if (!sameGuid(setting->PowerSetting, kAcDcPowerSource) ||
            setting->DataLength != static_cast<DWORD>(sizeof(DWORD))) {
            return std::nullopt;
        }

        std::optional<bool> onBattery;
        switch (readPowerValue(setting)) {
        case kPoAc:
            onBattery = false;
            break;
        case kPoDc:
            onBattery = true;
            break;
        case kPoHot:
            // A UPS can be on either source, so the state is unknown.
            break;
        default:
            return std::nullopt;
        }
        if (callbacks_.onPower) {
            callbacks_.onPower(onBattery);
        }
        return static_cast<LRESULT>(TRUE);
    } catch (...) {
        // A window procedure must not throw; the failed callback is dropped.
        return std::nullopt;
    }
}

bool PowerEvents::registerFor(HWND window)
{
    unregisterFor();
    if (window == nullptr) {
        return false;
    }
    gAcDcNotify = RegisterPowerSettingNotification(window, &kAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
    // Optional: a desktop may reject or never send the lid setting, which must not fail registerFor.
    gLidNotify = RegisterPowerSettingNotification(window, &kLidSwitchStateChange, DEVICE_NOTIFY_WINDOW_HANDLE);
    return gAcDcNotify != nullptr;
}

void PowerEvents::unregisterFor()
{
    if (gAcDcNotify != nullptr) {
        UnregisterPowerSettingNotification(gAcDcNotify);
        gAcDcNotify = nullptr;
    }
    if (gLidNotify != nullptr) {
        UnregisterPowerSettingNotification(gLidNotify);
        gLidNotify = nullptr;
    }
}

}  // namespace telaviva
