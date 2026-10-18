// Tests for the set_volume acceptance tracker that feeds the key router.
#include "TestHarness.h"
#include "Platform/JsonCodec.h"
#include "Platform/VolumeActionTracker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {
namespace {

const std::string kA = "monitor-a";
const std::string kB = "monitor-b";

MonitorSnapshot makeMonitor(const std::string& id, uint64_t generation)
{
    MonitorSnapshot monitor;
    monitor.id = id;
    monitor.generation = generation;
    return monitor;
}

PendingAction makeAction(uint64_t id, const std::string& monitorId, uint64_t generation, const std::string& kind)
{
    PendingAction action;
    action.id = id;
    action.monitorId = monitorId;
    action.generation = generation;
    action.kind = kind;
    action.value = 20.0;
    return action;
}

Snapshot makeSnapshot(uint64_t revision, std::vector<MonitorSnapshot> monitors, std::vector<PendingAction> actions = {})
{
    Snapshot snapshot;
    snapshot.revision = revision;
    snapshot.monitors = std::move(monitors);
    snapshot.pendingActions = std::move(actions);
    return snapshot;
}

TEST(volume_tracker_reports_new_action_once_with_both_generations)
{
    VolumeActionTracker tracker;
    const Snapshot first = makeSnapshot(1, {makeMonitor(kA, 3)}, {makeAction(5, kA, 9, "set_volume")});
    const std::vector<AcceptedAction> reported = tracker.onSnapshot(first);
    CHECK_EQ(reported.size(), std::size_t{1});
    CHECK(reported.size() == 1 && reported[0].monitorId == kA && reported[0].connectionGeneration == 3 &&
          reported[0].actionGeneration == 9);
    CHECK_EQ(tracker.onSnapshot(first).size(), std::size_t{0});
    CHECK_EQ(tracker.onSnapshot(makeSnapshot(2, {makeMonitor(kA, 3)}, {makeAction(5, kA, 9, "set_volume")})).size(),
             std::size_t{0});
}

TEST(volume_tracker_reports_each_monitor)
{
    VolumeActionTracker tracker;
    const Snapshot snapshot = makeSnapshot(1, {makeMonitor(kA, 1), makeMonitor(kB, 2)},
                                           {makeAction(5, kA, 4, "set_volume"), makeAction(6, kB, 4, "set_volume")});
    const std::vector<AcceptedAction> reported = tracker.onSnapshot(snapshot);
    CHECK_EQ(reported.size(), std::size_t{2});
    CHECK(reported.size() == 2 && reported[0].monitorId == kA && reported[0].connectionGeneration == 1 &&
          reported[1].monitorId == kB && reported[1].connectionGeneration == 2);
}

TEST(volume_tracker_ignores_other_kinds_and_missing_monitors)
{
    VolumeActionTracker tracker;
    const Snapshot brightness = makeSnapshot(1, {makeMonitor(kA, 1)}, {makeAction(5, kA, 1, "set_brightness")});
    CHECK_EQ(tracker.onSnapshot(brightness).size(), std::size_t{0});

    // A monitor missing from the snapshot is skipped, and its action is reported once it appears.
    const Snapshot missing = makeSnapshot(2, {}, {makeAction(6, kB, 1, "set_volume")});
    CHECK_EQ(tracker.onSnapshot(missing).size(), std::size_t{0});
    const std::vector<AcceptedAction> later =
        tracker.onSnapshot(makeSnapshot(3, {makeMonitor(kB, 2)}, {makeAction(6, kB, 1, "set_volume")}));
    CHECK_EQ(later.size(), std::size_t{1});
    CHECK(later.size() == 1 && later[0].connectionGeneration == 2 && later[0].actionGeneration == 1);
}

TEST(volume_tracker_forgets_ids_that_are_no_longer_pending)
{
    VolumeActionTracker tracker;
    CHECK_EQ(tracker.onSnapshot(makeSnapshot(1, {makeMonitor(kA, 1)}, {makeAction(5, kA, 1, "set_volume")})).size(),
             std::size_t{1});
    CHECK_EQ(tracker.onSnapshot(makeSnapshot(2, {makeMonitor(kA, 1)})).size(), std::size_t{0});
    // Once the core stops listing the id, it no longer occupies the tracker.
    CHECK_EQ(tracker.onSnapshot(makeSnapshot(3, {makeMonitor(kA, 1)}, {makeAction(5, kA, 1, "set_volume")})).size(),
             std::size_t{1});
}

TEST(volume_tracker_is_capped)
{
    VolumeActionTracker tracker;
    std::vector<MonitorSnapshot> monitors;
    std::vector<PendingAction> actions;
    for (uint64_t index = 0; index < 300; ++index) {
        const std::string id = "monitor-" + std::to_string(index);
        monitors.push_back(makeMonitor(id, 1));
        actions.push_back(makeAction(index + 1, id, 1, "set_volume"));
    }
    const Snapshot crowded = makeSnapshot(1, monitors, actions);
    CHECK_EQ(tracker.onSnapshot(crowded).size(), VolumeActionTracker::kMaxRemembered);
    // The cap holds while the same ids stay pending, so nothing is reported twice.
    CHECK_EQ(tracker.onSnapshot(makeSnapshot(2, monitors, actions)).size(), std::size_t{0});
}

}  // namespace
}  // namespace telaviva
