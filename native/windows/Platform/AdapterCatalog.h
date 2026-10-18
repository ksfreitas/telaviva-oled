#pragma once
// DXGI adapter list for DirectML device selection and profile matching, with the
// integrated or discrete kind taken from DXCore. This header stays free of
// windows.h; the DXGI and DXCore calls live in AdapterCatalog.cpp.
#include <cstdint>
#include <string>
#include <vector>

namespace telaviva {

// Kind from DXCore IsIntegrated. Unknown when DXCore could not classify the adapter.
enum class AdapterKind { Unknown, Integrated, Discrete };

struct AdapterInfo {
    uint32_t vendorId = 0;      // PCI vendor id from DXGI_ADAPTER_DESC1
    uint32_t deviceId = 0;      // PCI device id from DXGI_ADAPTER_DESC1
    std::string description;    // UTF-8
    std::string driverVersion;  // "a.b.c.d" from the user-mode driver, empty when unknown
    bool software = false;      // Microsoft Basic Render Driver and other software adapters
    // Position in IDXGIFactory1::EnumAdapters1 order, software adapters included. ONNX Runtime
    // selects a DirectML device by this same order, so the index is taken from DXGI, not assumed.
    uint32_t index = 0;
    uint64_t luid = 0;          // (HighPart << 32) | LowPart of DXGI_ADAPTER_DESC1::AdapterLuid
    AdapterKind kind = AdapterKind::Unknown;
};

// One adapter as DXCore reports it. Pure data so classification is testable.
struct DxCoreAdapterInfo {
    uint64_t luid = 0;          // InstanceLuid
    bool integrated = false;    // IsIntegrated
    bool hardware = true;       // IsHardware
};

// Returns a copy of dxgi where each non-software adapter whose luid equals a DxCoreAdapterInfo::luid
// (and luid != 0, and hardware == true) gets kind Integrated or Discrete from `integrated`.
// Adapters without a match, software adapters, and every adapter when `dxcore` is empty stay Unknown.
std::vector<AdapterInfo> classifyKinds(std::vector<AdapterInfo> dxgi, const std::vector<DxCoreAdapterInfo>& dxcore);

struct IAdapterCatalog {
    virtual ~IAdapterCatalog() = default;
    virtual std::vector<AdapterInfo> adapters() = 0;
};

// Formats a user-mode driver version (the 64-bit value from CheckInterfaceSupport)
// as "a.b.c.d", each part a 16-bit decimal number.
std::string formatDriverVersion(uint64_t umdVersion);

class DxgiAdapterCatalog : public IAdapterCatalog {
public:
    // Lists hardware and software adapters, software ones with software = true, and fills
    // luid and kind. Never throws; returns an empty list when DXGI is unavailable.
    std::vector<AdapterInfo> adapters() override;
};

}  // namespace telaviva
