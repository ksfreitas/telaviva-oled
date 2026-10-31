#pragma once
// Locks the Windows session when the core asks for a lock on absence, and reports the
// outcome once per token. A lock is confirmed by a session lock event within a window.
// The OS call goes through ISessionLocker, and timers through IScheduler. This header
// does not depend on windows.h.
#include "Scheduler.h"
#include "SessionLocker.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace telaviva {

// Thread-safe. Results are submitted outside the internal mutex, never from a locked section.
class LockCoordinator {
public:
    using SubmitResult = std::function<void(uint64_t token, bool ok, const std::string& error)>;
    using LogLine = std::function<void(const std::string& line)>;

    LockCoordinator(ISessionLocker& locker, IScheduler& scheduler, SubmitResult submit, LogLine log,
                    std::chrono::milliseconds confirmWindow = std::chrono::milliseconds(3000));
    // Calls shutdown(), so the timer is cancelled and no result is submitted afterwards.
    ~LockCoordinator();

    LockCoordinator(const LockCoordinator&) = delete;
    LockCoordinator& operator=(const LockCoordinator&) = delete;

    // Called from the core snapshot sink thread. Only a requested snapshot with a token
    // that differs from the last handled one locks the session.
    void observe(bool requested, std::optional<uint64_t> token);

    // Called from the UI thread on WTS_SESSION_LOCK and WTS_SESSION_UNLOCK. Only a lock
    // event confirms a pending request.
    void sessionLockChanged(bool locked);

    // Idempotent. Cancels the timer and drops any pending request without submitting.
    void shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace telaviva
