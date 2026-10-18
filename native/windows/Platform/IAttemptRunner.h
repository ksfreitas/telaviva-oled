#pragma once
// The seam between the core's DDC attempts and the platform port (architecture Decision 6).
// run() performs exactly one DDC primitive: no retries, no sleeps and no readback comparison,
// because the core owns those. DdcController implements it; the ControlsDriver worker
// depends on this header.
#include <cstdint>
#include <optional>
#include <string>

namespace telaviva {

struct AttemptRequest {
    std::string monitorId;
    bool write = false;
    uint8_t code = 0;    // VCP code: 0x10 brightness, 0x62 volume
    uint32_t value = 0;  // raw target, used when write is true
};

struct AttemptOutcome {
    bool ok = false;
    std::string error;            // "", "unsupported", "io" or "disconnected"
    std::optional<double> value;  // raw readback for reads
};

struct IAttemptRunner {
    virtual ~IAttemptRunner() = default;
    // Callable from the DDC executor thread.
    virtual AttemptOutcome run(const AttemptRequest& request) = 0;
};

// Told about discovery and removal on the executor thread, never under DdcController's mutex.
// The listener must stay valid until DdcController::shutdown has returned and queued work has drained.
struct IDdcListener {
    virtual ~IDdcListener() = default;
    virtual void onMonitorProfile(const std::string& monitorId, const std::string& manufacturer) = 0;
    virtual void onMonitorRemoved(const std::string& monitorId) = 0;
};

}  // namespace telaviva
