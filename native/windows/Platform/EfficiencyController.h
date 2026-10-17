#pragma once
// Efficiency policy over IEfficiency. Process-level EcoQoS is requested once,
// at startup; after that only thread-level exemptions change. Not thread-safe:
// call from the UI thread, and call onStartup before any other method.
#include "IEfficiency.h"

#include <cstdint>
#include <string>

namespace telaviva {

enum class EfficiencyState {
    NotStarted,
    EcoQoSRequested,
    UserManaged,
    Unsupported,
};

class EfficiencyController {
public:
    // uiThread is the thread that shows the popup and settings windows.
    EfficiencyController(IEfficiency& efficiency, EfficiencyThreadId uiThread);

    EfficiencyController(const EfficiencyController&) = delete;
    EfficiencyController& operator=(const EfficiencyController&) = delete;

    // Runs once; later calls do nothing, so the process request is never repeated.
    void onStartup();

    // visibleCount is the number of visible popup or settings windows. The UI
    // thread is exempt while it is above zero.
    void onWindowVisibilityChanged(uint32_t visibleCount);

    // Gives a thread normal quality, e.g. the keyboard hook thread. Only applies
    // while EcoQoS was requested, so a user-managed process is never fought.
    void registerExemptThread(EfficiencyThreadId thread);

    EfficiencyState state() const { return state_; }

    // Diagnostics line fragment: "efficiency: <state>".
    std::string stateString() const;

private:
    void applyUiExemption(bool exempt);

    IEfficiency& efficiency_;
    EfficiencyThreadId uiThread_;
    EfficiencyState state_ = EfficiencyState::NotStarted;
    uint32_t visibleCount_ = 0;
    bool uiExempt_ = false;
};

}  // namespace telaviva
