#include "AdapterCatalog.h"

#include <windows.h>
#include <dxgi.h>
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

}  // namespace

std::string formatDriverVersion(uint64_t umdVersion)
{
    const auto high = static_cast<uint32_t>(umdVersion >> 32);
    const auto low = static_cast<uint32_t>(umdVersion & 0xFFFFFFFFu);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", high >> 16, high & 0xFFFFu, low >> 16, low & 0xFFFFu);
    return buffer;
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
            // Stored from the loop counter, so an adapter skipped by GetDesc1 cannot shift the others.
            info.index = static_cast<uint32_t>(index);
            // The user-mode driver version comes from IDXGIDevice support on the adapter.
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                info.driverVersion = formatDriverVersion(static_cast<uint64_t>(umd.QuadPart));
            }
            out.push_back(std::move(info));
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

}  // namespace telaviva
