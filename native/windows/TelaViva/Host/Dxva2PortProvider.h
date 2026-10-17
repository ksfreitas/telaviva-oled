#pragma once
// Opens dxva2 ports for catalog entries. Threading: open() and every port it returns
// must be used from the DDC executor thread only (dxva2 is not thread-safe), so the
// provider's map needs no lock.
#include "Platform/DdcController.h"

#include <map>
#include <memory>
#include <string>

namespace telaviva::host {

struct OpenMonitor;

class Dxva2PortProvider final : public telaviva::IDdcPortProvider {
public:
    Dxva2PortProvider() = default;
    ~Dxva2PortProvider() override = default;

    Dxva2PortProvider(const Dxva2PortProvider&) = delete;
    Dxva2PortProvider& operator=(const Dxva2PortProvider&) = delete;

    // Returns nullptr when the entry has no HMONITOR or no physical monitor opens.
    // Opening an id again releases the map's handle for it; a port already returned keeps
    // its own handle alive until the port is destroyed, so it never dangles.
    std::unique_ptr<telaviva::IDdcPort> open(const telaviva::CatalogEntry& entry) override;

private:
    std::map<std::string, std::shared_ptr<OpenMonitor>> handles_;
};

}  // namespace telaviva::host
