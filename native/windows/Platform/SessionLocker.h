#pragma once
// Locks the Windows session on request. The Win32 call lives only in SessionLocker.cpp,
// so this header does not depend on windows.h. The interface lets tests replace the OS call.

namespace telaviva {

struct ISessionLocker {
    virtual ~ISessionLocker() = default;
    // True when the OS accepted the request. The lock itself is confirmed by a session event.
    virtual bool lockNow() = 0;
};

// Calls LockWorkStation. Safe from any thread.
class Win32SessionLocker final : public ISessionLocker {
public:
    bool lockNow() override;
};

}  // namespace telaviva
