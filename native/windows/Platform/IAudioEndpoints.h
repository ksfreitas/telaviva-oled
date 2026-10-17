#pragma once
// Seams for the default render endpoint and the monitor container ids, so the
// owner policy in AudioOwner can be tested without COM or SetupAPI. No windows.h.
#include <functional>
#include <optional>
#include <string>

namespace telaviva {

struct EndpointInfo {
    std::string endpointId;   // IMMDevice::GetId, UTF-8
    std::string containerId;  // PKEY_Device_ContainerId, empty when absent
};

struct IAudioEndpoints {
    virtual ~IAudioEndpoints() = default;
    // Default render device for the console role (eConsole). nullopt when there is none.
    virtual std::optional<EndpointInfo> defaultRenderEndpoint() = 0;
    // Called when the default output or the device set changes. An empty function stops calls.
    virtual void setChangeCallback(std::function<void()> onChange) = 0;
};

struct IMonitorContainers {
    virtual ~IMonitorContainers() = default;
    // Container id of the monitor. nullopt when it has none or it cannot be read.
    virtual std::optional<std::string> containerIdFor(const std::string& monitorId) = 0;
};

}  // namespace telaviva
