#include "Dxva2DdcPort.h"

#include <windows.h>
#include <HighLevelMonitorConfigurationAPI.h>
#include <PhysicalMonitorEnumerationAPI.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <winerror.h>

#include <cstddef>
#include <iterator>
#include <utility>
#include <wchar.h>

#pragma comment(lib, "dxva2.lib")

namespace telaviva {

namespace {

// The winerror.h constants are signed HRESULTs, so both sides are cast to DWORD
// to keep /W4 quiet about signed/unsigned comparisons.
DdcError mapLastError(DWORD error)
{
    if (error == static_cast<DWORD>(ERROR_GRAPHICS_DDCCI_VCP_NOT_SUPPORTED)) {
        return DdcError::Unsupported;
    }
    if (error == static_cast<DWORD>(ERROR_GRAPHICS_MONITOR_NO_LONGER_EXISTS) ||
        error == static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED)) {
        return DdcError::Disconnected;
    }
    return DdcError::Io;
}

}  // namespace

PhysicalMonitor::PhysicalMonitor(void* handle, std::wstring description)
    : handle_(handle), description_(std::move(description))
{
}

PhysicalMonitor::PhysicalMonitor(PhysicalMonitor&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), description_(std::move(other.description_))
{
}

PhysicalMonitor& PhysicalMonitor::operator=(PhysicalMonitor&& other) noexcept
{
    if (this != &other) {
        if (handle_ != nullptr) {
            DestroyPhysicalMonitor(static_cast<HANDLE>(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
        description_ = std::move(other.description_);
    }
    return *this;
}

PhysicalMonitor::~PhysicalMonitor()
{
    if (handle_ != nullptr) {
        DestroyPhysicalMonitor(static_cast<HANDLE>(handle_));
    }
}

std::vector<PhysicalMonitor> PhysicalMonitor::openForHMonitor(uintptr_t hmonitor)
{
    std::vector<PhysicalMonitor> monitors;
    const HMONITOR monitor = reinterpret_cast<HMONITOR>(hmonitor);

    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || count == 0) {
        return monitors;
    }

    std::vector<PHYSICAL_MONITOR> raw(count);
    if (!GetPhysicalMonitorsFromHMONITOR(monitor, count, raw.data())) {
        return monitors;
    }

    // Wrap each handle as soon as it is returned so it is always released.
    monitors.reserve(raw.size());
    for (const PHYSICAL_MONITOR& entry : raw) {
        const std::size_t length = ::wcsnlen(entry.szPhysicalMonitorDescription,
                                             std::size(entry.szPhysicalMonitorDescription));
        monitors.push_back(PhysicalMonitor(entry.hPhysicalMonitor,
                                           std::wstring(entry.szPhysicalMonitorDescription, length)));
    }
    return monitors;
}

std::wstring PhysicalMonitor::description() const
{
    return description_;
}

Dxva2DdcPort::Dxva2DdcPort(const PhysicalMonitor& monitor) : handle_(monitor.handle()) {}

Dxva2DdcPort::Dxva2DdcPort(void* handle) : handle_(handle) {}

bool Dxva2DdcPort::readVcp(uint8_t code, DdcReading& out, DdcError& error)
{
    DWORD current = 0;
    DWORD maximum = 0;
    if (!GetVCPFeatureAndVCPFeatureReply(static_cast<HANDLE>(handle_), code, nullptr, &current, &maximum)) {
        error = mapLastError(GetLastError());
        return false;
    }
    // Same validity rule as the macOS parser. A bad reply is treated as io, so
    // the transaction retries it rather than accepting it.
    if (maximum == 0 || current > maximum) {
        error = DdcError::Io;
        return false;
    }
    out.current = current;
    out.maximum = maximum;
    error = DdcError::None;
    return true;
}

bool Dxva2DdcPort::writeVcp(uint8_t code, uint32_t value, DdcError& error)
{
    if (!SetVCPFeature(static_cast<HANDLE>(handle_), code, value)) {
        error = mapLastError(GetLastError());
        return false;
    }
    error = DdcError::None;
    return true;
}

}  // namespace telaviva
