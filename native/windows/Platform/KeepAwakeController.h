#pragma once
// Keeps the computer awake while a face is detected. Port of the macOS
// KeepAwakeController: the same token, revocation and safety rules. The OS
// effects go through IKeepAwakeBackend, and timers through IScheduler.
// This header does not depend on windows.h.
#include "IClock.h"
#include "Scheduler.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace telaviva {

enum class KeepAwakeSafety {
    Eligible,
    SessionInactive,
    SessionStateUnknown,
    ScreensaverActive,
    ScreensaverStateUnknown,
    DisplayAsleep,
    SystemSleeping,
    // Diagnostic only. A closed lid with an awake external-monitor session is not a veto.
    LidClosed,
};

// Strict order: system sleep first, then session, display and screensaver.
// An unknown value is never treated as eligible.
KeepAwakeSafety resolveKeepAwakeSafety(bool sessionActive, std::optional<bool> sessionLocked,
                                       std::optional<bool> screensaverActive, bool displayAsleep,
                                       bool systemSleeping);

// Camera demand follows the unlocked, awake session. Inhibition has stricter gates.
bool keepAwakeCameraMonitoringAllowed(KeepAwakeSafety safety);

// A positive notification vetoes at once. Otherwise the live query decides, so a
// stale negative notification cannot clear a veto.
std::optional<bool> resolveKeepAwakeGuard(std::optional<bool> notified,
                                          const std::function<std::optional<bool>()>& live);

enum class KeepAwakeStatus {
    Disabled,
    AwaitingPresence,
    Active,
    PolicyBlocked,
    Unavailable,
    Suspended,
};

// Stable identifiers for logs and tests: "disabled", "awaiting", "active",
// "policy", "unavailable" or "suspended".
const char* keepAwakeStatusName(KeepAwakeStatus status);

// Effects the controller drives. The backend owns the OS handles.
struct IKeepAwakeBackend {
    virtual ~IKeepAwakeBackend() = default;
    virtual KeepAwakeSafety safetyState() = 0;
    // False when synthetic input is blocked by policy or cannot be determined.
    virtual bool postingAllowed() = 0;
    // Holds the energy request. False if the OS refused it.
    virtual bool acquireRequest() = 0;
    virtual void releaseRequest() = 0;
    // Zero-displacement activity. False if the OS refused it.
    virtual bool postActivity() = 0;
};

// Owns the temporary OS effects and the lease deadline. Timer callbacks run on
// scheduler threads, so every public call is safe from any thread. Backend calls
// are made under the controller's mutex. The controller owns no threads.
class KeepAwakeController {
public:
    KeepAwakeController(IKeepAwakeBackend& backend, IScheduler& scheduler, const IClock& clock);
    // Calls shutdown(), so the request is released and no timer remains.
    ~KeepAwakeController();

    KeepAwakeController(const KeepAwakeController&) = delete;
    KeepAwakeController& operator=(const KeepAwakeController&) = delete;

    // A true report permits camera monitoring. Only evidence from a newer generation
    // than the last suspended one can reactivate the effect.
    void setSessionEligible(bool eligible);

    // The snapshot TTL is remaining time. Repeating the same token can only shorten
    // its local deadline. A stale or replayed token is ignored.
    void update(bool enabled, bool requested, uint64_t generation, uint64_t observationId,
                uint64_t validForMilliseconds);

    // Applies a local veto synchronously, so a queued snapshot cannot override it.
    void safetyDidChange();

    // Idempotent. Releases the request, cancels timers, and publishes Disabled.
    void shutdown();

    KeepAwakeStatus status() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace telaviva
