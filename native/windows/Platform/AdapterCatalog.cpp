#include "AdapterCatalog.h"

#include <windows.h>
#include <dxgi.h>
#include <dxcore.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdio>
#include <utility>

#pragma comment(lib, "dxgi.lib")

namespace telaviva {
namespace {

std::string toUtf8(const wchar_t* text, size_t count)
{
    if (count == 0) {
        return std::string();
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(needed), '\0');
    static_cast<void>(WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), out.data(), needed, nullptr,
                                          nullptr));
    return out;
}

// DXGI_ADAPTER_DESC1.Description is a fixed 128-element array that may lack a terminator.
std::string describe(const wchar_t* text)
{
    size_t length = 0;
    while (length < 128 && text[length] != L'\0') {
        ++length;
    }
    return toUtf8(text, length);
}

// Same value for a DXGI AdapterLuid and a DXCore InstanceLuid: the high part is taken as unsigned.
uint64_t makeLuid(const LUID& luid)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) | static_cast<uint64_t>(luid.LowPart);
}

// DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU, defined locally so no dxguid or dxcore.lib link is needed.
const GUID kDxCoreGpu = {0xb69eb219, 0x3ded, 0x4464, {0x97, 0x9f, 0xa0, 0x0b, 0xd4, 0x68, 0x70, 0x06}};

using DxCoreCreateAdapterFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

// Frees dxcore.dll when it goes out of scope. Declared before the COM objects, so they are released first.
class LoadedModule {
public:
    explicit LoadedModule(HMODULE module) : module_(module) {}
    ~LoadedModule()
    {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    HMODULE get() const { return module_; }

private:
    HMODULE module_;
};

// Reads one DXCore property. False when the property is unsupported or the call fails.
template <typename T>
bool readProperty(IDXCoreAdapter* adapter, DXCoreAdapterProperty property, T& value)
{
    if (!adapter->IsPropertySupported(property)) {
        return false;
    }
    return SUCCEEDED(adapter->GetProperty(property, sizeof(T), &value));
}

// Lists the GPU adapters DXCore reports. Returns an empty list on any failure. Creates no D3D12 or
// DirectML device; it only reads adapter properties.
std::vector<DxCoreAdapterInfo> queryDxCoreGpus()
{
    std::vector<DxCoreAdapterInfo> out;
    LoadedModule module(LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (module.get() == nullptr) {
        return out;
    }
    // The cast goes through void* so the function pointer is not converted directly from FARPROC.
    const auto create = reinterpret_cast<DxCoreCreateAdapterFactoryFn>(
        reinterpret_cast<void*>(GetProcAddress(module.get(), "DXCoreCreateAdapterFactory")));
    if (create == nullptr) {
        return out;
    }

    Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
    if (FAILED(create(__uuidof(IDXCoreAdapterFactory), reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
        return out;
    }
    Microsoft::WRL::ComPtr<IDXCoreAdapterList> list;
    if (FAILED(factory->CreateAdapterList(1, &kDxCoreGpu, IID_PPV_ARGS(&list)))) {
        return out;
    }

    const uint32_t count = list->GetAdapterCount();
    for (uint32_t i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IDXCoreAdapter> adapter;
        if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&adapter)))) {
            continue;
        }
        bool integrated = false;
        bool hardware = false;
        LUID luid{};
        if (!readProperty(adapter.Get(), DXCoreAdapterProperty::IsIntegrated, integrated) ||
            !readProperty(adapter.Get(), DXCoreAdapterProperty::IsHardware, hardware) ||
            !readProperty(adapter.Get(), DXCoreAdapterProperty::InstanceLuid, luid)) {
            continue;
        }
        DxCoreAdapterInfo info;
        info.luid = makeLuid(luid);
        info.integrated = integrated;
        info.hardware = hardware;
        out.push_back(info);
    }
    return out;
}

}  // namespace

std::string formatDriverVersion(uint64_t umdVersion)
{
    const auto high = static_cast<uint32_t>(umdVersion >> 32);
    const auto low = static_cast<uint32_t>(umdVersion & 0xFFFFFFFFu);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", high >> 16, high & 0xFFFFu, low >> 16, low & 0xFFFFu);
    return buffer;
}

std::vector<AdapterInfo> classifyKinds(std::vector<AdapterInfo> dxgi, const std::vector<DxCoreAdapterInfo>& dxcore)
{
    for (AdapterInfo& adapter : dxgi) {
        if (adapter.software || adapter.luid == 0) {
            continue;
        }
        for (const DxCoreAdapterInfo& entry : dxcore) {
            if (entry.hardware && entry.luid == adapter.luid) {
                adapter.kind = entry.integrated ? AdapterKind::Integrated : AdapterKind::Discrete;
                break;
            }
        }
    }
    return dxgi;
}

std::vector<AdapterInfo> DxgiAdapterCatalog::adapters()
{
    std::vector<AdapterInfo> out;
    try {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return out;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index) {
            // DXGI_ERROR_NOT_FOUND marks the end of the list; any other failure ends it too.
            if (FAILED(factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()))) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue;
            }
            AdapterInfo info;
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.description = describe(desc.Description);
            info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.luid = makeLuid(desc.AdapterLuid);
            // Stored from the loop counter, so an adapter skipped by GetDesc1 cannot shift the others.
            info.index = static_cast<uint32_t>(index);
            // The user-mode driver version comes from IDXGIDevice support on the adapter.
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                info.driverVersion = formatDriverVersion(static_cast<uint64_t>(umd.QuadPart));
            }
            out.push_back(std::move(info));
        }
        // Any DXCore failure leaves the kinds Unknown; the list itself is kept.
        if (!out.empty()) {
            out = classifyKinds(std::move(out), queryDxCoreGpus());
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

}  // namespace telaviva
