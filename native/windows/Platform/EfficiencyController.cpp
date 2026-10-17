#include "EfficiencyController.h"

namespace telaviva {

EfficiencyController::EfficiencyController(IEfficiency& efficiency, EfficiencyThreadId uiThread)
    : efficiency_(efficiency), uiThread_(uiThread)
{
}

void EfficiencyController::onStartup()
{
    // Once only: a repeated process-level request would fight the user's choice.
    if (state_ != EfficiencyState::NotStarted) {
        return;
    }

    const ProcessEfficiencyReading reading = efficiency_.currentProcessState();
    if (!reading.readable) {
        state_ = EfficiencyState::Unsupported;
        return;
    }
    // A forced Efficiency mode shows up as an idle class or as throttling the
    // app did not set. Either way the user owns the state, so leave it alone.
    if (reading.idlePriority || reading.throttleControlled) {
        state_ = EfficiencyState::UserManaged;
        return;
    }
    if (!efficiency_.setProcessEcoQoS(true)) {
        state_ = EfficiencyState::Unsupported;
        return;
    }
    state_ = EfficiencyState::EcoQoSRequested;

    // Popup may already be showing when startup finishes.
    if (visibleCount_ > 0) {
        applyUiExemption(true);
    }
}

void EfficiencyController::onWindowVisibilityChanged(uint32_t visibleCount)
{
    visibleCount_ = visibleCount;
    if (state_ != EfficiencyState::EcoQoSRequested) {
        return;
    }
    applyUiExemption(visibleCount > 0);
}

void EfficiencyController::registerExemptThread(EfficiencyThreadId thread)
{
    if (state_ != EfficiencyState::EcoQoSRequested) {
        return;
    }
    efficiency_.setThreadEcoQoS(thread, true);
}

std::string EfficiencyController::stateString() const
{
    switch (state_) {
    case EfficiencyState::NotStarted:
        return "efficiency: not-started";
    case EfficiencyState::EcoQoSRequested:
        return "efficiency: ecoqos-requested";
    case EfficiencyState::UserManaged:
        return "efficiency: user-managed";
    case EfficiencyState::Unsupported:
        return "efficiency: unsupported";
    }
    return "efficiency: unsupported";
}

void EfficiencyController::applyUiExemption(bool exempt)
{
    if (exempt == uiExempt_) {
        return;
    }
    // Record only success, so a failed call is retried on the next change.
    if (efficiency_.setThreadEcoQoS(uiThread_, exempt)) {
        uiExempt_ = exempt;
    }
}

}  // namespace telaviva
