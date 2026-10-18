#include "VolumeActionTracker.h"

#include <iterator>

namespace telaviva {

std::vector<AcceptedAction> VolumeActionTracker::onSnapshot(const Snapshot& snapshot)
{
    std::set<uint64_t> listed;
    for (const PendingAction& action : snapshot.pendingActions) {
        listed.insert(action.id);
    }
    // An id the core no longer lists is finished; forgetting it keeps the set bounded by what is pending.
    for (auto it = reported_.begin(); it != reported_.end();) {
        if (listed.count(*it) != 0) {
            ++it;
        } else {
            it = reported_.erase(it);
        }
    }

    std::vector<AcceptedAction> accepted;
    for (const PendingAction& action : snapshot.pendingActions) {
        if (action.kind != "set_volume" || reported_.count(action.id) != 0) {
            continue;
        }
        const MonitorSnapshot* monitor = nullptr;
        for (const MonitorSnapshot& candidate : snapshot.monitors) {
            if (candidate.id == action.monitorId) {
                monitor = &candidate;
                break;
            }
        }
        if (monitor == nullptr) {
            continue;
        }
        if (reported_.size() >= kMaxRemembered) {
            break;
        }
        reported_.insert(action.id);
        accepted.push_back(AcceptedAction{action.monitorId, monitor->generation, action.generation});
    }
    return accepted;
}

}  // namespace telaviva
