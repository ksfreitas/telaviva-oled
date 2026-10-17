#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/SessionEvents.h"

#include <dbt.h>
#include <wtsapi32.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using telaviva::FakeScheduler;
using telaviva::SessionEventCallbacks;
using telaviva::SessionEvents;

namespace {

// Test-side GUIDs, separate from the implementation's copies so a typo in either is caught.
const GUID kDisplayStatusGuid = {0x2B84C20E, 0xAD23, 0x4DDF, {0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5}};
const GUID kMonitorClassGuid = {0xE6F07B5F, 0xEE97, 0x4A90, {0xB0, 0x76, 0x33, 0xF5, 0x7B, 0xF4, 0xEA, 0xA7}};
// GUID_DEVINTERFACE_DISK, a device class that must be ignored.
const GUID kDiskClassGuid = {0x53F56307, 0xB6BF, 0x11D0, {0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};
// A power setting GUID that is not the display status.
const GUID kOtherPowerGuid = {0x00000000, 0x0000, 0x0000, {0, 0, 0, 0, 0, 0, 0, 1}};

const LRESULT kTrue = static_cast<LRESULT>(TRUE);

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

std::vector<unsigned char> deviceInterfaceBuffer(const GUID& classGuid)
{
    std::vector<unsigned char> buffer(sizeof(DEV_BROADCAST_DEVICEINTERFACE_W));
    auto* device = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(buffer.data());
    device->dbcc_size = static_cast<DWORD>(buffer.size());
    device->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    device->dbcc_classguid = classGuid;
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

// Records callback calls. Declare it before the SessionEvents under test so it outlives them.
struct Recorder {
    std::string display;  // one char per call: '0' off, '1' on
    int resumes = 0;
    int topology = 0;
    std::string locks;  // '1' locked, '0' unlocked

    SessionEventCallbacks callbacks()
    {
        SessionEventCallbacks callbacks;
        callbacks.onDisplayState = [this](bool on) { display += on ? '1' : '0'; };
        callbacks.onResume = [this] { ++resumes; };
        callbacks.onTopologyChanged = [this] { ++topology; };
        callbacks.onSessionLock = [this](bool locked) { locks += locked ? '1' : '0'; };
        return callbacks;
    }
};

}  // namespace

TEST(session_events_display_off_on_and_dim)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    const auto off = powerSettingBuffer(kDisplayStatusGuid, 0);
    const auto on = powerSettingBuffer(kDisplayStatusGuid, 1);
    const auto dim = powerSettingBuffer(kDisplayStatusGuid, 2);
    const auto unknown = powerSettingBuffer(kDisplayStatusGuid, 3);

    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(off))), kTrue);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(on))), kTrue);
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(dim))), kTrue);
    // Unknown values are neither consumed nor reported.
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(unknown)).has_value());
    CHECK_EQ(rec.display, std::string("011"));
}

TEST(session_events_other_power_setting_is_ignored)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    const auto other = powerSettingBuffer(kOtherPowerGuid, 0);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(other)).has_value());
    CHECK_EQ(rec.display, std::string(""));
}

TEST(session_events_display_setting_with_wrong_length_is_ignored)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    const auto shortValue = powerSettingBuffer(kDisplayStatusGuid, 0, 2);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(shortValue)).has_value());
    CHECK_EQ(rec.display, std::string(""));
}

TEST(session_events_resume_fires_and_passes_through)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    const auto result = events.handleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    CHECK(!result.has_value());
    CHECK_EQ(rec.resumes, 1);
}

TEST(session_events_other_power_broadcast_is_ignored)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0).has_value());
    CHECK_EQ(rec.resumes, 0);
    CHECK_EQ(rec.display, std::string(""));
}

TEST(session_events_topology_burst_fires_once_per_window)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks(), 500ms);

    const auto monitor = deviceInterfaceBuffer(kMonitorClassGuid);
    const WPARAM arrival = static_cast<WPARAM>(DBT_DEVICEARRIVAL);
    const WPARAM removal = static_cast<WPARAM>(DBT_DEVICEREMOVECOMPLETE);

    // Five triggers, 50 ms apart, all inside the 500 ms window that the first one opens.
    events.handleMessage(WM_DISPLAYCHANGE, 0, 0);
    scheduler.advance(50ms);
    events.handleMessage(WM_DEVICECHANGE, arrival, asLParam(monitor));
    scheduler.advance(50ms);
    events.handleMessage(WM_DEVICECHANGE, removal, asLParam(monitor));
    scheduler.advance(50ms);
    events.handleMessage(WM_DISPLAYCHANGE, 0, 0);
    scheduler.advance(50ms);
    events.handleMessage(WM_DEVICECHANGE, arrival, asLParam(monitor));

    // The first trigger is due at 500 ms; nothing fires before that.
    scheduler.advance(299ms);
    CHECK_EQ(rec.topology, 0);
    scheduler.advance(1ms);
    CHECK_EQ(rec.topology, 1);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(scheduler.toleranceLog().size(), std::size_t{1});

    // A later trigger opens a new window and fires once more.
    events.handleMessage(WM_DISPLAYCHANGE, 0, 0);
    scheduler.advance(500ms);
    CHECK_EQ(rec.topology, 2);
}

TEST(session_events_device_change_other_class_is_ignored)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    const auto disk = deviceInterfaceBuffer(kDiskClassGuid);
    const auto monitorQuery = deviceInterfaceBuffer(kMonitorClassGuid);
    CHECK(!events.handleMessage(WM_DEVICECHANGE, static_cast<WPARAM>(DBT_DEVICEARRIVAL), asLParam(disk)).has_value());
    // A query is not an arrival or removal, so it is not a trigger either.
    CHECK(!events.handleMessage(WM_DEVICECHANGE, static_cast<WPARAM>(DBT_DEVICEQUERYREMOVE),
                                asLParam(monitorQuery))
               .has_value());
    scheduler.advance(1000ms);
    CHECK_EQ(rec.topology, 0);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
}

TEST(session_events_session_lock_and_unlock)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    CHECK(!events.handleMessage(WM_WTSSESSION_CHANGE, static_cast<WPARAM>(WTS_SESSION_LOCK), 1).has_value());
    CHECK(!events.handleMessage(WM_WTSSESSION_CHANGE, static_cast<WPARAM>(WTS_SESSION_UNLOCK), 1).has_value());
    // Other session changes do not touch the lock state.
    CHECK(!events.handleMessage(WM_WTSSESSION_CHANGE, static_cast<WPARAM>(WTS_SESSION_LOGON), 1).has_value());
    CHECK_EQ(rec.locks, std::string("10"));
}

TEST(session_events_unknown_messages_return_nullopt)
{
    Recorder rec;
    FakeScheduler scheduler;
    SessionEvents events(scheduler, rec.callbacks());

    CHECK(!events.handleMessage(WM_CLOSE, 0, 0).has_value());
    CHECK(!events.handleMessage(WM_TIMER, 0, 0).has_value());
    CHECK(!events.handleMessage(WM_ENDSESSION, 1, 0).has_value());
    CHECK_EQ(rec.display, std::string(""));
    CHECK_EQ(rec.locks, std::string(""));
    CHECK_EQ(rec.resumes, 0);
    CHECK_EQ(rec.topology, 0);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
}

TEST(session_events_null_callbacks_do_not_crash)
{
    FakeScheduler scheduler;
    SessionEvents events(scheduler, SessionEventCallbacks{});

    const auto on = powerSettingBuffer(kDisplayStatusGuid, 1);
    const auto monitor = deviceInterfaceBuffer(kMonitorClassGuid);
    // A consumed display message still returns TRUE even without a callback.
    CHECK_EQ(valueOrNone(events.handleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, asLParam(on))), kTrue);
    CHECK(!events.handleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0).has_value());
    CHECK(!events.handleMessage(WM_DISPLAYCHANGE, 0, 0).has_value());
    CHECK(!events.handleMessage(WM_DEVICECHANGE, static_cast<WPARAM>(DBT_DEVICEARRIVAL), asLParam(monitor)).has_value());
    CHECK(!events.handleMessage(WM_WTSSESSION_CHANGE, static_cast<WPARAM>(WTS_SESSION_LOCK), 1).has_value());
    scheduler.advance(1000ms);
    // With no topology callback, no timer is scheduled at all.
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
}

TEST(session_events_destruction_cancels_pending_topology)
{
    Recorder rec;
    FakeScheduler scheduler;
    {
        SessionEvents events(scheduler, rec.callbacks());
        events.handleMessage(WM_DISPLAYCHANGE, 0, 0);
        CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    }
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    scheduler.advance(1000ms);
    CHECK_EQ(rec.topology, 0);
}
