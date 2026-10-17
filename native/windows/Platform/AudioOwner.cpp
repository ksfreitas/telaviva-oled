#include "AudioOwner.h"

// The Win32 adapters at the bottom are the only code here that needs windows.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <initguid.h>
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <propkeydef.h>
#include <mmdeviceapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace telaviva {

namespace {

constexpr const char* kZeroContainerId = "00000000-0000-0000-0000-000000000000";
constexpr const char* kUnknownContainerId = "00000000-0000-0000-ffff-ffffffffffff";

}  // namespace

std::string normalizeContainerId(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        if (c == '{' || c == '}') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // Windows reports these for devices without a container. Treating them as ids
    // would match unrelated devices, so they count as absent.
    if (out == kZeroContainerId || out == kUnknownContainerId) {
        return {};
    }
    return out;
}

AudioOwner::AudioOwner(IAudioEndpoints& endpoints, IMonitorContainers& containers, OwnerCallback onOwnerChanged)
    : endpoints_(endpoints), containers_(containers), onOwnerChanged_(std::move(onOwnerChanged))
{
    endpoints_.setChangeCallback([this] { refresh(); });
}

AudioOwner::~AudioOwner()
{
    endpoints_.setChangeCallback(nullptr);
}

std::optional<std::string> AudioOwner::owner() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return computeOwnerLocked();
}

void AudioOwner::refresh()
{
    republish();
}

void AudioOwner::onMonitorsChanged(const std::vector<std::string>& monitorIds)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitorIds_ = monitorIds;
    }
    republish();
}

void AudioOwner::republish()
{
    std::optional<std::string> next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = computeOwnerLocked();
        if (next == published_) {
            return;
        }
        published_ = next;
    }
    // Outside the lock so the callback may query the owner. Two racing changes can
    // reach the callback out of order; the next change publishes the current value.
    if (onOwnerChanged_) {
        onOwnerChanged_(next);
    }
}

std::optional<std::string> AudioOwner::computeOwnerLocked() const
{
    const std::optional<EndpointInfo> endpoint = endpoints_.defaultRenderEndpoint();
    if (!endpoint) {
        return std::nullopt;
    }
    const std::string endpointContainer = normalizeContainerId(endpoint->containerId);
    if (endpointContainer.empty()) {
        return std::nullopt;
    }

    std::optional<std::string> match;
    int matches = 0;
    for (const std::string& monitorId : monitorIds_) {
        // A monitor without a container id cannot match; it does not veto a match elsewhere.
        const std::optional<std::string> raw = containers_.containerIdFor(monitorId);
        if (!raw) {
            continue;
        }
        const std::string container = normalizeContainerId(*raw);
        if (container.empty()) {
            continue;
        }
        if (container == endpointContainer) {
            ++matches;
            match = monitorId;
        }
    }
    // Two monitors on one container cannot be told apart, so neither owns the output.
    if (matches != 1) {
        return std::nullopt;
    }
    return match;
}

// ---- Win32 adapters ----

namespace {

std::string narrow(const wchar_t* text)
{
    if (text == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// Receives IMMNotificationClient callbacks and forwards every device or default-device
// change. The owner recomputes and publishes only real changes, so no filtering here.
class Notifier final : public IMMNotificationClient {
public:
    explicit Notifier(std::function<void()> onChange) : onChange_(std::move(onChange)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --refs_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        onChange_();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
    {
        onChange_();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        onChange_();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        // Only the render default for the console role decides the owner.
        if (flow == eRender && role == eConsole) {
            onChange_();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    std::function<void()> onChange_;
    std::atomic<ULONG> refs_{1};
};

// Reads the container id of an endpoint device, normalized. Empty when absent.
std::string readEndpointContainerId(IMMDevice* device)
{
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || store == nullptr) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string result;
    if (SUCCEEDED(store->GetValue(PKEY_Device_ContainerId, &value)) && value.vt == VT_CLSID &&
        value.puuid != nullptr) {
        wchar_t text[64] = {};
        if (StringFromGUID2(*value.puuid, text, 64) > 0) {
            result = normalizeContainerId(narrow(text));
        }
    }
    PropVariantClear(&value);
    store->Release();
    return result;
}

}  // namespace

struct Win32AudioEndpoints::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    Notifier* notifier = nullptr;
    std::mutex mutex;
    std::function<void()> callback;
    bool registered = false;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl()
    {
        if (enumerator != nullptr) {
            if (registered) {
                enumerator->UnregisterEndpointNotificationCallback(notifier);
            }
            enumerator->Release();
        }
        if (notifier != nullptr) {
            notifier->Release();
        }
    }

    // Copies the callback under the lock and calls it outside, so the callback may
    // query the endpoint or clear itself.
    void dispatch()
    {
        std::function<void()> target;
        {
            std::lock_guard<std::mutex> lock(mutex);
            target = callback;
        }
        if (target) {
            target();
        }
    }
};

Win32AudioEndpoints::Win32AudioEndpoints() : impl_(std::make_unique<Impl>())
{
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&impl_->enumerator));
    Impl* raw = impl_.get();
    impl_->notifier = new Notifier([raw] { raw->dispatch(); });
}

Win32AudioEndpoints::~Win32AudioEndpoints() = default;

std::optional<EndpointInfo> Win32AudioEndpoints::defaultRenderEndpoint()
{
    if (impl_->enumerator == nullptr) {
        return std::nullopt;
    }
    IMMDevice* device = nullptr;
    if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || device == nullptr) {
        return std::nullopt;
    }
    std::optional<EndpointInfo> result;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
        EndpointInfo info;
        info.endpointId = narrow(id);
        info.containerId = readEndpointContainerId(device);
        result = std::move(info);
    }
    if (id != nullptr) {
        CoTaskMemFree(id);
    }
    device->Release();
    return result;
}

void Win32AudioEndpoints::setChangeCallback(std::function<void()> onChange)
{
    bool registerNow = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->callback = std::move(onChange);
        if (impl_->callback && !impl_->registered && impl_->enumerator != nullptr) {
            impl_->registered = true;
            registerNow = true;
        }
    }
    if (registerNow &&
        FAILED(impl_->enumerator->RegisterEndpointNotificationCallback(impl_->notifier))) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->registered = false;
    }
}

PnpMonitorContainers::PnpMonitorContainers(DevicePathLookup devicePathFor) : devicePathFor_(std::move(devicePathFor))
{
}

std::optional<std::string> PnpMonitorContainers::containerIdFor(const std::string& monitorId)
{
    if (!devicePathFor_) {
        return std::nullopt;
    }
    const std::optional<std::wstring> path = devicePathFor_(monitorId);
    if (!path || path->empty()) {
        return std::nullopt;
    }

    GUID container{};
    DEVPROPTYPE type = 0;
    ULONG size = static_cast<ULONG>(sizeof(container));
    const CONFIGRET status = CM_Get_Device_Interface_PropertyW(path->c_str(), &DEVPKEY_Device_ContainerId, &type,
                                                               reinterpret_cast<BYTE*>(&container), &size, 0);
    if (status != CR_SUCCESS || type != DEVPROP_TYPE_GUID || size != sizeof(container)) {
        return std::nullopt;
    }

    wchar_t text[64] = {};
    if (StringFromGUID2(container, text, 64) <= 0) {
        return std::nullopt;
    }
    const std::string id = normalizeContainerId(narrow(text));
    if (id.empty()) {
        return std::nullopt;
    }
    return id;
}

}  // namespace telaviva
