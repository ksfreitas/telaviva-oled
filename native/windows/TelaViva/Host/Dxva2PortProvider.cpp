#include "pch.h"

#include "Host/Dxva2PortProvider.h"
#include "Platform/Dxva2DdcPort.h"

#include <utility>

namespace telaviva::host {

// Owns one physical monitor and the port that borrows its handle. Both live in one
// object so the port can never outlive the handle it refers to.
struct OpenMonitor {
    explicit OpenMonitor(telaviva::PhysicalMonitor&& physical)
        : monitor(std::move(physical)), port(monitor)
    {
    }

    telaviva::PhysicalMonitor monitor;
    telaviva::Dxva2DdcPort port;
};

namespace {

// Forwards to a shared OpenMonitor, so the handle stays valid while the controller
// holds this port even after the provider has released its map entry.
class ForwardingPort final : public telaviva::IDdcPort {
public:
    explicit ForwardingPort(std::shared_ptr<OpenMonitor> target) : target_(std::move(target)) {}

    bool readVcp(uint8_t code, telaviva::DdcReading& out, telaviva::DdcError& error) override
    {
        return target_->port.readVcp(code, out, error);
    }

    bool writeVcp(uint8_t code, uint32_t value, telaviva::DdcError& error) override
    {
        return target_->port.writeVcp(code, value, error);
    }

private:
    std::shared_ptr<OpenMonitor> target_;
};

}  // namespace

std::unique_ptr<telaviva::IDdcPort> Dxva2PortProvider::open(const telaviva::CatalogEntry& entry)
{
    if (entry.identity.id.empty()) {
        return nullptr;
    }

    // Release the previous handle first; a port still held by the controller keeps its own reference.
    handles_.erase(entry.identity.id);

    if (entry.hmonitor == 0) {
        return nullptr;
    }

    // Only the first physical monitor is used; the rest close when the vector goes out of scope.
    auto monitors = telaviva::PhysicalMonitor::openForHMonitor(entry.hmonitor);
    if (monitors.empty()) {
        return nullptr;
    }

    auto monitor = std::make_shared<OpenMonitor>(std::move(monitors.front()));
    handles_[entry.identity.id] = monitor;
    return std::make_unique<ForwardingPort>(std::move(monitor));
}

}  // namespace telaviva::host
