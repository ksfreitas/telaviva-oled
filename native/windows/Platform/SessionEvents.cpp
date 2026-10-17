#include "SessionEvents.h"

#include <dbt.h>
#include <wtsapi32.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>

#pragma comment(lib, "wtsapi32.lib")

namespace telaviva {

namespace {

// Local copies so the link does not depend on which SDK headers define the GUIDs.
constexpr GUID kSessionDisplayStatus = {
    0x2B84C20E, 0xAD23, 0x4DDF, {0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5}};
constexpr GUID kMonitorInterfaceClass = {
    0xE6F07B5F, 0xEE97, 0x4A90, {0xB0, 0x76, 0x33, 0xF5, 0x7B, 0xF4, 0xEA, 0xA7}};

// Display power values: 0 off, 1 on, 2 dimmed (treated as on). Any other value
// is ignored rather than guessed.
constexpr DWORD kDisplayDim = 2;

// Handles from the last successful registerFor.
HPOWERNOTIFY gPowerNotify = nullptr;
HDEVNOTIFY gDeviceNotify = nullptr;
HWND gSessionWindow = nullptr;

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

bool isMonitorInterfaceEvent(WPARAM wParam, LPARAM lParam)
{
    if (wParam != static_cast<WPARAM>(DBT_DEVICEARRIVAL) &&
        wParam != static_cast<WPARAM>(DBT_DEVICEREMOVECOMPLETE)) {
        return false;
    }
    if (lParam == 0) {
        return false;
    }
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
    if (header->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
        return false;
    }
    // Refuse a broadcast too short to contain the class GUID.
    if (header->dbch_size < offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_classguid) + sizeof(GUID)) {
        return false;
    }
    const auto* device = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lParam);
    return sameGuid(device->dbcc_classguid, kMonitorInterfaceClass);
}

}  // namespace

// Shared with scheduled timers so a timer that fires after the owner is gone
// finds a closed state instead of a dangling pointer.
struct SessionEvents::Topology {
    std::mutex mutex;
    std::function<void()> onChanged;
    TimerId pending = kInvalidTimerId;
    bool closed = false;

    void fire();
};

void SessionEvents::Topology::fire()
{
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = kInvalidTimerId;
        if (closed) {
            return;
        }
        callback = onChanged;
    }
    // Called outside the lock so the callback may re-enter the event source.
    callback();
}

SessionEvents::SessionEvents(IScheduler& scheduler, SessionEventCallbacks callbacks,
                             std::chrono::milliseconds topologyDebounce)
    : scheduler_(scheduler),
      topologyDebounce_(topologyDebounce),
      callbacks_(std::move(callbacks)),
      topology_(std::make_shared<Topology>())
{
    topology_->onChanged = callbacks_.onTopologyChanged;
}

SessionEvents::~SessionEvents()
{
    TimerId pending = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(topology_->mutex);
        topology_->closed = true;
        pending = topology_->pending;
        topology_->pending = kInvalidTimerId;
    }
    // Cancel outside the lock: a timer that is already running waits for the
    // same lock, and cancel may wait for that timer to finish.
    if (pending != kInvalidTimerId) {
        scheduler_.cancel(pending);
    }
}

void SessionEvents::requestTopology()
{
    std::lock_guard<std::mutex> lock(topology_->mutex);
    if (topology_->closed || !topology_->onChanged || topology_->pending != kInvalidTimerId) {
        return;
    }
    std::shared_ptr<Topology> self = topology_;
    topology_->pending = scheduler_.scheduleOnce(topologyDebounce_, toleranceFor(topologyDebounce_),
                                                 [self] { self->fire(); });
}

std::optional<LRESULT> SessionEvents::handlePower(WPARAM wParam, LPARAM lParam)
{
    if (wParam == static_cast<WPARAM>(PBT_APMRESUMEAUTOMATIC)) {
        if (callbacks_.onResume) {
            callbacks_.onResume();
        }
        // Resume is not ours alone; other handlers must see it too.
        return std::nullopt;
    }
    if (wParam != static_cast<WPARAM>(PBT_POWERSETTINGCHANGE) || lParam == 0) {
        return std::nullopt;
    }
    const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
    if (!sameGuid(setting->PowerSetting, kSessionDisplayStatus) ||
        setting->DataLength != static_cast<DWORD>(sizeof(DWORD))) {
        return std::nullopt;
    }
    const DWORD state = readPowerValue(setting);
    if (state > kDisplayDim) {
        return std::nullopt;
    }
    if (callbacks_.onDisplayState) {
        callbacks_.onDisplayState(state != 0);
    }
    return static_cast<LRESULT>(TRUE);
}

std::optional<LRESULT> SessionEvents::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    try {
        switch (message) {
        case WM_POWERBROADCAST:
            return handlePower(wParam, lParam);
        case WM_DISPLAYCHANGE:
            requestTopology();
            return std::nullopt;
        case WM_DEVICECHANGE:
            if (isMonitorInterfaceEvent(wParam, lParam)) {
                requestTopology();
            }
            return std::nullopt;
        case WM_WTSSESSION_CHANGE:
            if (wParam == static_cast<WPARAM>(WTS_SESSION_LOCK)) {
                if (callbacks_.onSessionLock) {
                    callbacks_.onSessionLock(true);
                }
            } else if (wParam == static_cast<WPARAM>(WTS_SESSION_UNLOCK)) {
                if (callbacks_.onSessionLock) {
                    callbacks_.onSessionLock(false);
                }
            }
            return std::nullopt;
        default:
            return std::nullopt;
        }
    } catch (...) {
        // A window procedure must not throw; the failed callback is dropped.
        return std::nullopt;
    }
}

bool SessionEvents::registerFor(HWND window)
{
    unregisterFor();
    if (window == nullptr) {
        return false;
    }

    gPowerNotify = RegisterPowerSettingNotification(window, &kSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (gPowerNotify == nullptr) {
        return false;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = static_cast<DWORD>(sizeof(filter));
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kMonitorInterfaceClass;
    gDeviceNotify = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (gDeviceNotify == nullptr) {
        unregisterFor();
        return false;
    }

    if (!WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION)) {
        unregisterFor();
        return false;
    }
    gSessionWindow = window;
    return true;
}

void SessionEvents::unregisterFor()
{
    if (gPowerNotify != nullptr) {
        UnregisterPowerSettingNotification(gPowerNotify);
        gPowerNotify = nullptr;
    }
    if (gDeviceNotify != nullptr) {
        UnregisterDeviceNotification(gDeviceNotify);
        gDeviceNotify = nullptr;
    }
    if (gSessionWindow != nullptr) {
        WTSUnRegisterSessionNotification(gSessionWindow);
        gSessionWindow = nullptr;
    }
}

}  // namespace telaviva
