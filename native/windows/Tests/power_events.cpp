#include "TestHarness.h"

#include "Platform/PowerEvents.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using telaviva::IPowerStatusReader;
using telaviva::PowerEventCallbacks;
using telaviva::PowerEvents;
using telaviva::onBatteryFromStatus;

namespace {

// Test-side GUIDs, separate from the implementation's copy so a typo in either is caught.
const GUID kAcDcGuid = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
// GUID_SESSION_DISPLAY_STATUS, a setting that must be ignored here.
const GUID kDisplayStatusGuid = {0x2B84C20E, 0xAD23, 0x4DDF, {0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5}};
const GUID kLidGuid = {0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3}};

const LRESULT kTrue = static_cast<LRESULT>(TRUE);

// Reports the fields a test sets, as GetSystemPowerStatus would.
class FakePowerReader final : public IPowerStatusReader {
public:
    bool read(uint8_t& acLineStatus, uint8_t& batteryFlag) override
    {
        ++reads;
        if (!readable) {
            return false;
        }
        acLineStatus = ac;
        batteryFlag = battery;
        return true;
    }

    bool readable = true;
    uint8_t ac = 255;
    uint8_t battery = 255;
    int reads = 0;
};

// One char per callback: 'a' AC, 'b' battery, '?' unknown. The same encoding
// is used for the pure mapping so the results print readably.
char code(const std::optional<bool>& onBattery)
{
    if (!onBattery.has_value()) {
        return '?';
    }
    return *onBattery ? 'b' : 'a';
}

std::vector<unsigned char> powerSettingBuffer(const GUID& guid, DWORD value,
                                              DWORD dataLength = static_cast<DWORD>(sizeof(DWORD)))
{
    std::vector<unsigned char> buffer(sizeof(POWERBROADCAST_SETTING) + sizeof(DWORD));
    auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(buffer.data());
    setting->PowerSetting = guid;
    setting->DataLength = dataLength;
    std::memcpy(buffer.data() + offsetof(POWERBROADCAST_SETTING, Data), &value, sizeof value);
    return buffer;
}

LPARAM asLParam(const std::vector<unsigned char>& buffer)
{
    return reinterpret_cast<LPARAM>(buffer.data());
}

LRESULT valueOrNone(const std::optional<LRESULT>& result)
{
    return result.value_or(-1);
}

// Records callback calls. Declare it before the PowerEvents under test so it outlives them.
// Lid calls are recorded as 'o' open and 'c' closed.
struct Recorder {
    std::string events;
    std::string lid;

    PowerEventCallbacks callbacks()
    {
        PowerEventCallbacks callbacks;
        callbacks.onPower = [this](std::optional<bool> onBattery) { events += code(onBattery); };
        callbacks.onLid = [this](bool open) { lid += open ? 'o' : 'c'; };
        return callbacks;
    }
};

}  // namespace

TEST(power_status_mapping_ac_online_is_mains)
{
    CHECK_EQ(code(onBatteryFromStatus(1, 0)), 'a');
    CHECK_EQ(code(onBatteryFromStatus(1, 1)), 'a');
    CHECK_EQ(code(onBatteryFromStatus(1, 128)), 'a');
    CHECK_EQ(code(onBatteryFromStatus(1, 255)), 'a');
}

TEST(power_status_mapping_ac_offline_is_battery)
{
    CHECK_EQ(code(onBatteryFromStatus(0, 0)), 'b');
    CHECK_EQ(code(onBatteryFromStatus(0, 1)), 'b');
    CHECK_EQ(code(onBatteryFromStatus(0, 255)), 'b');
}

TEST(power_status_mapping_unknown_ac_without_battery_is_mains)
{
    // A desktop reports unknown AC with no battery; it is always on mains power.
    CHECK_EQ(code(onBatteryFromStatus(255, 128)), 'a');
}

TEST(power_status_mapping_unknown_ac_with_battery_is_unknown)
{
    CHECK_EQ(code(onBatteryFromStatus(255, 0)), '?');
    CHECK_EQ(code(onBatteryFromStatus(255, 1)), '?');
    CHECK_EQ(code(onBatteryFromStatus(255, 255)), '?');
}

TEST(power_status_mapping_other_ac_values_are_unknown)
{
    // Only 0, 1 and 255 are defined; anything else is not guessed.
    CHECK_EQ(code(onBatteryFromStatus(2, 128)), '?');
    CHECK_EQ(code(onBatteryFromStatus(254, 0)), '?');
}

TEST(power_dc_setting_reports_battery_and_ac_reports_mains)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    const auto dc = powerSettingBuffer(kAcDcGuid, 1);
    const auto ac = powerSettingBuffer(kAcDcGuid, 0);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(dc))), kTrue);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(ac))), kTrue);
    CHECK_EQ(rec.events, std::string("ba"));
    // The setting itself carries the answer, so the status is not read.
    CHECK_EQ(reader.reads, 0);
}

TEST(power_hot_setting_reports_unknown)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    // PoHot is a UPS, which can run on either source.
    const auto hot = powerSettingBuffer(kAcDcGuid, 2);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(hot))), kTrue);
    CHECK_EQ(rec.events, std::string("?"));
}

TEST(power_unknown_setting_value_is_ignored)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    const auto unknown = powerSettingBuffer(kAcDcGuid, 3);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(unknown)).has_value());
    CHECK_EQ(rec.events, std::string(""));
}

TEST(power_other_setting_guid_and_wrong_length_are_ignored)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    const auto other = powerSettingBuffer(kDisplayStatusGuid, 1);
    const auto shortValue = powerSettingBuffer(kAcDcGuid, 1, 2);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(other)).has_value());
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(shortValue)).has_value());
    CHECK_EQ(rec.events, std::string(""));
}

TEST(power_resume_republishes_current_status)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    reader.ac = 0;
    reader.battery = 1;
    // Resume is not consumed, so other handlers still see it.
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0).has_value());
    CHECK_EQ(rec.events, std::string("b"));
    CHECK_EQ(reader.reads, 1);

    reader.ac = 1;
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0).has_value());
    CHECK_EQ(rec.events, std::string("ba"));
    CHECK_EQ(reader.reads, 2);
}

TEST(power_publish_current_reports_once_per_call)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    reader.ac = 1;
    reader.battery = 128;
    events.publishCurrent();
    CHECK_EQ(rec.events, std::string("a"));
    CHECK_EQ(reader.reads, 1);

    reader.ac = 0;
    events.publishCurrent();
    CHECK_EQ(rec.events, std::string("ab"));
    CHECK_EQ(reader.reads, 2);
}

TEST(power_unreadable_status_reports_unknown)
{
    Recorder rec;
    FakePowerReader reader;
    reader.readable = false;
    PowerEvents events(reader, rec.callbacks());

    events.publishCurrent();
    CHECK_EQ(rec.events, std::string("?"));
    CHECK_EQ(reader.reads, 1);
}

TEST(power_unrelated_messages_return_nullopt)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    CHECK(!events.handleMessage(WM_CLOSE, 0, 0).has_value());
    CHECK(!events.handleMessage(WM_DISPLAYCHANGE, 0, 0).has_value());
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0).has_value());
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, 0).has_value());
    CHECK_EQ(rec.events, std::string(""));
    CHECK_EQ(reader.reads, 0);
}

TEST(power_null_callbacks_do_not_crash)
{
    FakePowerReader reader;
    PowerEvents events(reader, PowerEventCallbacks{});

    const auto dc = powerSettingBuffer(kAcDcGuid, 1);
    const auto hot = powerSettingBuffer(kAcDcGuid, 2);
    // A consumed power setting still returns TRUE even without a callback.
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(dc))), kTrue);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(hot))), kTrue);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0).has_value());
    events.publishCurrent();
    CHECK_EQ(reader.reads, 2);
}

TEST(power_lid_closed_then_open_reaches_on_lid_in_order)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    const auto closed = powerSettingBuffer(kLidGuid, 0);
    const auto open = powerSettingBuffer(kLidGuid, 1);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(closed))), kTrue);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(open))), kTrue);
    CHECK_EQ(rec.lid, std::string("co"));
    CHECK_EQ(rec.events, std::string(""));
    CHECK_EQ(reader.reads, 0);
}

TEST(power_lid_wrong_size_and_unknown_value_are_ignored)
{
    Recorder rec;
    FakePowerReader reader;
    PowerEvents events(reader, rec.callbacks());

    const auto shortValue = powerSettingBuffer(kLidGuid, 1, 2);
    const auto unknown = powerSettingBuffer(kLidGuid, 2);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(shortValue)).has_value());
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(unknown)).has_value());
    CHECK_EQ(rec.lid, std::string(""));
}

TEST(power_lid_null_callback_does_not_crash)
{
    FakePowerReader reader;
    PowerEvents events(reader, PowerEventCallbacks{});

    const auto closed = powerSettingBuffer(kLidGuid, 0);
    const auto open = powerSettingBuffer(kLidGuid, 1);
    // A consumed lid setting still returns TRUE even without a callback.
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(closed))), kTrue);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(open))), kTrue);
    CHECK_EQ(reader.reads, 0);
}
