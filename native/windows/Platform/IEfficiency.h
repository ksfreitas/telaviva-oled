#pragma once
// Seam between the efficiency policy and the OS. Win32Efficiency is the real
// implementation; tests use a fake. Keeps windows.h out of this header.
#include <cstdint>

namespace telaviva {

// Thread id as returned by GetThreadId or GetCurrentThreadId.
using EfficiencyThreadId = uint32_t;

// What the OS reports about the process before the app changes anything.
struct ProcessEfficiencyReading {
    // False when the query failed or the API is missing; the policy then does nothing.
    bool readable = false;
    // Priority class is IDLE_PRIORITY_CLASS.
    bool idlePriority = false;
    // Someone else set the execution-speed throttling control (ControlMask bit).
    bool throttleControlled = false;
    // Execution-speed throttling is on (StateMask bit).
    bool throttleOn = false;
};

struct IEfficiency {
    virtual ~IEfficiency() = default;

    virtual ProcessEfficiencyReading currentProcessState() = 0;

    // enabled = true requests EcoQoS for the whole process. Returns false on failure.
    virtual bool setProcessEcoQoS(bool enabled) = 0;

    // exempt = true asks the thread for normal quality; false returns it to the
    // process state. Returns false on failure.
    virtual bool setThreadEcoQoS(EfficiencyThreadId thread, bool exempt) = 0;
};

}  // namespace telaviva
