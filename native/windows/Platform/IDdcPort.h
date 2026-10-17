#pragma once
// Transport seam for DDC/CI VCP access. The dxva2 adapter implements it and
// tests use a fake. Results come back through out-parameters so no Win32 types leak in.
#include <cstdint>

namespace telaviva {

enum class DdcError { None, Unsupported, Io, Disconnected };

struct DdcReading {
    uint32_t current = 0;
    uint32_t maximum = 0;
};

struct IDdcPort {
    virtual ~IDdcPort() = default;
    virtual bool readVcp(uint8_t code, DdcReading& out, DdcError& error) = 0;
    virtual bool writeVcp(uint8_t code, uint32_t value, DdcError& error) = 0;
};

}  // namespace telaviva
