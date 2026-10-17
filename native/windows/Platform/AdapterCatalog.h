#pragma once
// DXGI adapter list for DirectML profile matching. This header stays free of
// windows.h; the DXGI calls live in AdapterCatalog.cpp.
#include <cstdint>
#include <string>
#include <vector>

namespace telaviva {

struct AdapterInfo {
    uint32_t vendorId = 0;      // PCI vendor id from DXGI_ADAPTER_DESC1
    uint32_t deviceId = 0;      // PCI device id from DXGI_ADAPTER_DESC1
    std::string description;    // UTF-8
    std::string driverVersion;  // "a.b.c.d" from the user-mode driver, empty when unknown
    bool software = false;      // Microsoft Basic Render Driver and other software adapters
    // Position in IDXGIFactory1::EnumAdapters1 order, software adapters included. ONNX Runtime
    // selects a DirectML device by this same order, so the index is taken from DXGI, not assumed.
    uint32_t index = 0;
};

struct IAdapterCatalog {
    virtual ~IAdapterCatalog() = default;
    virtual std::vector<AdapterInfo> adapters() = 0;
};

// Formats a user-mode driver version (the 64-bit value from CheckInterfaceSupport)
// as "a.b.c.d", each part a 16-bit decimal number.
std::string formatDriverVersion(uint64_t umdVersion);

class DxgiAdapterCatalog : public IAdapterCatalog {
public:
    // Lists hardware and software adapters, software ones with software = true.
    // Never throws; returns an empty list when DXGI is unavailable.
    std::vector<AdapterInfo> adapters() override;
};

}  // namespace telaviva
