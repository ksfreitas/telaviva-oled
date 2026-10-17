#pragma once
// dxva2 adapter for DDC/CI VCP access. Win32 types stay out of this header so
// callers only see void* handles and standard types.
//
// Threading: dxva2 gives no thread-safety guarantee, so one port or handle must
// be used from one thread at a time. The controller serializes access.
#include "IDdcPort.h"

#include <cstdint>
#include <string>
#include <vector>

namespace telaviva {

// RAII owner of one PHYSICAL_MONITOR handle, released with DestroyPhysicalMonitor.
// Copies are disallowed so the handle is destroyed exactly once.
class PhysicalMonitor {
public:
    PhysicalMonitor(PhysicalMonitor&& other) noexcept;
    PhysicalMonitor& operator=(PhysicalMonitor&& other) noexcept;
    ~PhysicalMonitor();

    PhysicalMonitor(const PhysicalMonitor&) = delete;
    PhysicalMonitor& operator=(const PhysicalMonitor&) = delete;

    // The HMONITOR is passed as an integer so this header needs no windows.h.
    static std::vector<PhysicalMonitor> openForHMonitor(uintptr_t hmonitor);

    std::wstring description() const;

    // Borrowed; valid only while this PhysicalMonitor is alive.
    void* handle() const noexcept { return handle_; }

private:
    PhysicalMonitor(void* handle, std::wstring description);

    void* handle_ = nullptr;
    std::wstring description_;
};

// Borrows the handle, so the PhysicalMonitor (or raw handle owner) must outlive the port.
class Dxva2DdcPort final : public IDdcPort {
public:
    explicit Dxva2DdcPort(const PhysicalMonitor& monitor);
    explicit Dxva2DdcPort(void* handle);

    bool readVcp(uint8_t code, DdcReading& out, DdcError& error) override;
    bool writeVcp(uint8_t code, uint32_t value, DdcError& error) override;

private:
    void* handle_;
};

}  // namespace telaviva
