#pragma once
// Decides which monitor owns the default audio output: the monitor whose container
// id equals the default render endpoint's container id. There is no name fallback;
// anything ambiguous means no owner, so volume keys go to Windows untouched.
// The Win32 adapters here keep windows.h and COM out of this header.
#include "IAudioEndpoints.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

// Lowercase, braces removed. Empty for the placeholder ids Windows reports for
// devices that have no container (all zeros, or all zeros with ffff tail).
std::string normalizeContainerId(const std::string& raw);

class AudioOwner {
public:
    using OwnerCallback = std::function<void(std::optional<std::string> owner)>;

    // Registers for default-output changes on endpoints and clears them on destruction,
    // so endpoints must outlive this object. Call refresh() or onMonitorsChanged()
    // once after construction to publish the first owner.
    AudioOwner(IAudioEndpoints& endpoints, IMonitorContainers& containers, OwnerCallback onOwnerChanged);
    ~AudioOwner();

    AudioOwner(const AudioOwner&) = delete;
    AudioOwner& operator=(const AudioOwner&) = delete;

    // Live owner from the current endpoint and the last monitor list.
    std::optional<std::string> owner() const;

    // Recomputes and publishes only when the owner differs from the last published one.
    void refresh();
    void onMonitorsChanged(const std::vector<std::string>& monitorIds);

private:
    void republish();
    std::optional<std::string> computeOwnerLocked() const;

    IAudioEndpoints& endpoints_;
    IMonitorContainers& containers_;
    OwnerCallback onOwnerChanged_;
    mutable std::mutex mutex_;
    std::vector<std::string> monitorIds_;
    std::optional<std::string> published_;
};

// Win32 adapter. The caller must initialise COM on the constructing thread
// (CoInitializeEx; MTA recommended). Change callbacks arrive on a system thread.
class Win32AudioEndpoints final : public IAudioEndpoints {
public:
    Win32AudioEndpoints();
    ~Win32AudioEndpoints() override;

    Win32AudioEndpoints(const Win32AudioEndpoints&) = delete;
    Win32AudioEndpoints& operator=(const Win32AudioEndpoints&) = delete;

    std::optional<EndpointInfo> defaultRenderEndpoint() override;
    void setChangeCallback(std::function<void()> onChange) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads DEVPKEY_Device_ContainerId from a device interface path. The app wiring
// supplies devicePathFor: it maps a monitor id to the DisplayConfig monitorDevicePath
// (a device interface path), or nullopt when the monitor is not attached.
class PnpMonitorContainers final : public IMonitorContainers {
public:
    using DevicePathLookup = std::function<std::optional<std::wstring>(const std::string& monitorId)>;

    explicit PnpMonitorContainers(DevicePathLookup devicePathFor);

    std::optional<std::string> containerIdFor(const std::string& monitorId) override;

private:
    DevicePathLookup devicePathFor_;
};

}  // namespace telaviva
