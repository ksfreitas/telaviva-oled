#include "Win32Efficiency.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace telaviva {

// These APIs exist from Windows 8 (the calls) and 10 1709 (the throttling
// classes). On an older build the call returns FALSE, which the policy treats
// as unsupported, so there is no load-time failure to guard against.

ProcessEfficiencyReading Win32Efficiency::currentProcessState()
{
    ProcessEfficiencyReading reading;
    const HANDLE process = GetCurrentProcess();

    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (GetProcessInformation(process, ProcessPowerThrottling, &throttling, sizeof throttling) == FALSE) {
        return reading;
    }

    const DWORD priority = GetPriorityClass(process);
    if (priority == 0) {
        return reading;
    }

    reading.readable = true;
    reading.idlePriority = priority == IDLE_PRIORITY_CLASS;
    reading.throttleControlled = (throttling.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
    reading.throttleOn = (throttling.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
    return reading;
}

bool Win32Efficiency::setProcessEcoQoS(bool enabled)
{
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = enabled ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling) !=
           FALSE;
}

bool Win32Efficiency::setThreadEcoQoS(EfficiencyThreadId thread, bool exempt)
{
    // Thread ids need their own handle, since the caller may not be that thread.
    const HANDLE handle = OpenThread(THREAD_SET_INFORMATION, FALSE, static_cast<DWORD>(thread));
    if (handle == nullptr) {
        return false;
    }

    // Exempt: ControlMask set and StateMask clear means "normal quality".
    // Not exempt: clear ControlMask so the thread follows the process again.
    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = exempt ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    throttling.StateMask = 0;
    const BOOL ok = SetThreadInformation(handle, ThreadPowerThrottling, &throttling, sizeof throttling);
    CloseHandle(handle);
    return ok != FALSE;
}

}  // namespace telaviva
