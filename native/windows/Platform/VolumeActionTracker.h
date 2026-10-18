#pragma once
// Reports each set_volume action of the core's snapshots once, when a snapshot first lists it.
// The key router needs that acceptance to hold back readbacks while our own volume write is
// outstanding. The action generation is the core's own counter, so it is passed on unchanged,
// and the connection generation is the monitor's in the same snapshot.
// Not synchronized: call from the snapshot thread only.
#include "JsonCodec.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace telaviva {

struct AcceptedAction {
    std::string monitorId;
    uint64_t connectionGeneration = 0;  // the monitor's generation in the same snapshot
    uint64_t actionGeneration = 0;      // PendingAction.generation, as the core numbers it
};

class VolumeActionTracker {
public:
    // Most action ids remembered at once. Pending actions are one per monitor in practice,
    // so the cap is only a bound on memory.
    static constexpr std::size_t kMaxRemembered = 256;

    // Returns the set_volume actions of the snapshot that were not reported before, in snapshot
    // order. Ids that are no longer pending are forgotten. Actions of other kinds, and actions
    // for monitors missing from the snapshot, are skipped and not remembered.
    std::vector<AcceptedAction> onSnapshot(const Snapshot& snapshot);

private:
    std::set<uint64_t> reported_;  // action ids already returned and still pending
};

}  // namespace telaviva
