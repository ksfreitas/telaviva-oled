// Tests for the manual monitor controls. They fail to link until
// Platform/ManualControls.cpp defines ManualControls; that is expected at this stage.
#include "TestHarness.h"
#include "Fakes.h"
#include "Platform/JsonCodec.h"
#include "Platform/ManualControls.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {
namespace {

// Longer than any drain delay the coordinator may choose.
constexpr std::chrono::milliseconds kSettle{1000};

const std::string kA = "monitor-a";

struct SinkCall {
    std::string monitorId;
    ManualFeature feature;
    double value;
    uint64_t generation;
};

class FakeSink final : public IManualSink {
public:
    int submit(std::string_view monitorId, ManualFeature feature, double value,
               uint64_t connectionGeneration) override
    {
        calls.push_back(SinkCall{std::string(monitorId), feature, value, connectionGeneration});
        return status;
    }

    std::vector<SinkCall> calls;
    int status = 0;  // non-zero makes the next submissions fail
};

MonitorSnapshot makeMonitor(const std::string& id, uint64_t generation = 1)
{
    MonitorSnapshot monitor;
    monitor.id = id;
    monitor.name = id;
    monitor.identityStatus = "stable";
    monitor.generation = generation;
    monitor.displayState = "awake";
    monitor.brightness.supported = true;
    monitor.brightness.observed = 41.0;
    monitor.brightness.raw = 41.0;
    monitor.brightness.min = 0.0;
    monitor.brightness.max = 100.0;
    monitor.brightness.status = "ready";
    monitor.volume.supported = true;
    monitor.volume.observed = 50.0;
    monitor.volume.raw = 50.0;
    monitor.volume.min = 0.0;
    monitor.volume.max = 100.0;
    monitor.volume.status = "ready";
    return monitor;
}

MonitorSnapshot withBrightness(MonitorSnapshot monitor, double observed)
{
    monitor.brightness.observed = observed;
    monitor.brightness.raw = observed;
    return monitor;
}

PendingAction makeAction(uint64_t id, const std::string& monitorId, uint64_t generation,
                         const std::string& kind, double value)
{
    PendingAction action;
    action.id = id;
    action.monitorId = monitorId;
    action.generation = generation;
    action.kind = kind;
    action.value = value;
    return action;
}

// Callers pass increasing revisions, as the core does for each new snapshot.
Snapshot makeSnapshot(uint64_t revision, std::vector<MonitorSnapshot> monitors,
                      std::vector<PendingAction> actions = {})
{
    Snapshot snapshot;
    snapshot.revision = revision;
    snapshot.monitors = std::move(monitors);
    snapshot.pendingActions = std::move(actions);
    return snapshot;
}

TEST(manual_idle_control_does_no_work)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 41.0);
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Ready);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{0});
}

TEST(manual_set_target_shows_draft_immediately)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 62.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 62.0);
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);
    CHECK_EQ(sink.calls.size(), std::size_t{0});
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 1 && sink.calls[0].monitorId == kA && sink.calls[0].value == 62.0);
    CHECK(sink.calls.size() == 1 && sink.calls[0].feature == ManualFeature::Brightness);
    CHECK(sink.calls.size() == 1 && sink.calls[0].generation == 1);
}

TEST(manual_rejected_submit_reports_failure_without_retry)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    sink.status = -1;
    controls.setTarget(kA, ManualFeature::Brightness, 62.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Failed);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 41.0);

    controls.apply(makeSnapshot(2, {makeMonitor(kA)}));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Failed);

    sink.status = 0;
    controls.setTarget(kA, ManualFeature::Brightness, 62.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{2});
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);
}

TEST(manual_rapid_targets_coalesce_behind_in_flight_submission)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 0.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.setTarget(kA, ManualFeature::Brightness, 100.0);
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    // Old behaviour: a newer snapshot released the slot. Now the completion of the first command does.
    controls.apply(makeSnapshot(2, {withBrightness(makeMonitor(kA), 0.0)}));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 0.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 37.0);
    CHECK_EQ(sink.calls.size(), std::size_t{2});

    controls.apply(makeSnapshot(3, {withBrightness(makeMonitor(kA), 37.0)}));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{2});
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 37.0);
}

TEST(manual_queued_target_waits_for_core_pending_action)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)},
                                {makeAction(7, kA, 1, "set_brightness", 41.0)}));
    controls.setTarget(kA, ManualFeature::Brightness, 50.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{0});
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 50.0);
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);

    controls.apply(makeSnapshot(2, {makeMonitor(kA)}));
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 1 && sink.calls[0].value == 50.0);
}

TEST(manual_accepted_command_stays_in_flight_until_completion)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    // A snapshot no newer than the submission cannot prove the action finished.
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.apply(makeSnapshot(2, {makeMonitor(kA)}, {makeAction(9, kA, 1, "set_brightness", 20.0)}));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    // A newer snapshot without the action does not release a young slot any more.
    controls.apply(makeSnapshot(3, {makeMonitor(kA)}));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 20.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 37.0);
}

TEST(manual_readback_within_tolerance_clears_draft)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.apply(makeSnapshot(2, {withBrightness(makeMonitor(kA), 37.2)},
                                {makeAction(3, kA, 1, "set_brightness", 37.0)}));
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Ready);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 37.2);
}

TEST(manual_completion_without_readback_falls_back_to_observed)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    controls.apply(makeSnapshot(2, {withBrightness(makeMonitor(kA), 30.0)}));
    // Still in flight: a snapshot alone does not settle the draft.
    CHECK(controls.hasDraft(kA, ManualFeature::Brightness));
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, std::nullopt);
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 30.0);
}

TEST(manual_stale_completion_cannot_replace_newer_draft)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    controls.apply(makeSnapshot(2, {makeMonitor(kA)}, {makeAction(5, kA, 1, "set_brightness", 20.0)}));
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    // The older action finishes at 41 while the newer 37 is still waiting.
    controls.apply(makeSnapshot(3, {makeMonitor(kA)}));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 41.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 37.0);
}

TEST(manual_return_to_observed_value_is_still_submitted)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.setTarget(kA, ManualFeature::Brightness, 41.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 41.0);
    controls.apply(makeSnapshot(2, {makeMonitor(kA)}));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 41.0);
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 20.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 41.0);
}

TEST(manual_duplicate_raw_target_is_not_resubmitted)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    // A 0..10 range maps 51 and 52 percent to the same raw value, 5.
    MonitorSnapshot narrow = makeMonitor(kA);
    narrow.brightness.max = 10.0;
    narrow.brightness.observed = 5.0;
    narrow.brightness.raw = 5.0;
    controls.apply(makeSnapshot(1, {narrow}));
    controls.setTarget(kA, ManualFeature::Brightness, 51.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.setTarget(kA, ManualFeature::Brightness, 52.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 52.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
}

TEST(manual_more_than_128_targets_are_capped)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    std::vector<MonitorSnapshot> monitors;
    for (int index = 0; index < 129; ++index) {
        monitors.push_back(makeMonitor("monitor-" + std::to_string(index)));
    }
    controls.apply(makeSnapshot(1, monitors));
    for (const MonitorSnapshot& monitor : monitors) {
        controls.setTarget(monitor.id, ManualFeature::Brightness, 60.0);
    }
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), kManualMaxDrafts);
    CHECK(controls.hasDraft("monitor-127", ManualFeature::Brightness));
    CHECK(!controls.hasDraft("monitor-128", ManualFeature::Brightness));
    CHECK(controls.draft("monitor-128", ManualFeature::Brightness) == 41.0);

    // An existing draft is still updated at the cap.
    controls.setTarget("monitor-0", ManualFeature::Brightness, 70.0);
    CHECK(controls.draft("monitor-0", ManualFeature::Brightness) == 70.0);
}

TEST(manual_set_target_clamps_and_ignores_non_finite_values)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 150.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 100.0);
    controls.setTarget(kA, ManualFeature::Brightness, std::nan(""));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 100.0);
    controls.setTarget(kA, ManualFeature::Brightness, -5.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 0.0);
}

TEST(manual_external_intent_shows_draft_without_submitting)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    CHECK(controls.adoptExternalIntent(kA, 1, ManualFeature::Brightness, 56.0));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 56.0);
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{0});

    // The intent stays until the readback matches it, even across snapshots.
    controls.apply(makeSnapshot(2, {makeMonitor(kA)}));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 56.0);
    controls.apply(makeSnapshot(3, {withBrightness(makeMonitor(kA), 56.0)}));
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));

    CHECK(!controls.adoptExternalIntent(kA, 2, ManualFeature::Brightness, 56.0));
    CHECK(!controls.adoptExternalIntent(kA, 1, ManualFeature::Brightness, 150.0));
    CHECK(!controls.adoptExternalIntent("monitor-missing", 1, ManualFeature::Brightness, 10.0));
}

TEST(manual_external_intent_supersedes_unsent_slider_target)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 73.0);
    CHECK(controls.adoptExternalIntent(kA, 1, ManualFeature::Brightness, 56.0));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{0});
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 56.0);
}

TEST(manual_volume_intent_does_not_cancel_brightness_in_flight)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 70.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    controls.apply(makeSnapshot(2, {makeMonitor(kA)}, {makeAction(3, kA, 1, "set_brightness", 70.0)}));

    CHECK(controls.adoptExternalIntent(kA, 1, ManualFeature::Volume, 60.0));
    CHECK(controls.draft(kA, ManualFeature::Volume) == 60.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 70.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 70.0);
    controls.apply(makeSnapshot(3, {makeMonitor(kA)}));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 41.0);
    CHECK(controls.draft(kA, ManualFeature::Volume) == 60.0);
}

TEST(manual_monitors_and_features_submit_independently)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    const std::string second = "monitor-b";
    controls.apply(makeSnapshot(1, {makeMonitor(kA), makeMonitor(second)}));
    controls.setTarget(kA, ManualFeature::Brightness, 70.0);
    controls.setTarget(kA, ManualFeature::Volume, 30.0);
    controls.setTarget(second, ManualFeature::Brightness, 80.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{3});
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 70.0);
    CHECK(controls.draft(kA, ManualFeature::Volume) == 30.0);
    CHECK(controls.draft(second, ManualFeature::Brightness) == 80.0);

    bool sawFirst = false;
    bool sawSecond = false;
    bool sawThird = false;
    for (const SinkCall& call : sink.calls) {
        sawFirst = sawFirst || (call.monitorId == kA && call.feature == ManualFeature::Brightness && call.value == 70.0);
        sawSecond = sawSecond || (call.monitorId == kA && call.feature == ManualFeature::Volume && call.value == 30.0);
        sawThird = sawThird || (call.monitorId == second && call.feature == ManualFeature::Brightness && call.value == 80.0);
    }
    CHECK(sawFirst && sawSecond && sawThird);
}

TEST(manual_sleep_drops_queued_draft_and_wake_does_not_replay)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 25.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    controls.setTarget(kA, ManualFeature::Brightness, 60.0);
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 60.0);

    MonitorSnapshot asleep = makeMonitor(kA);
    asleep.displayState = "asleep";
    controls.apply(makeSnapshot(2, {asleep}));
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.apply(makeSnapshot(3, {makeMonitor(kA)}));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.setTarget(kA, ManualFeature::Brightness, 33.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 33.0);
}

TEST(manual_asleep_monitor_ignores_targets)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    MonitorSnapshot asleep = makeMonitor(kA);
    asleep.displayState = "asleep";
    controls.apply(makeSnapshot(1, {asleep}));
    controls.setTarget(kA, ManualFeature::Brightness, 50.0);
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{0});
}

TEST(manual_disappearing_monitor_drops_its_drafts)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 25.0);
    scheduler.advance(kSettle);
    controls.setTarget(kA, ManualFeature::Brightness, 60.0);

    controls.apply(makeSnapshot(2, {}));
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(!controls.draft(kA, ManualFeature::Brightness));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
}

TEST(manual_generation_rollover_drops_old_generation_draft)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA, 1)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    controls.apply(makeSnapshot(2, {makeMonitor(kA, 1)}, {makeAction(4, kA, 1, "set_brightness", 20.0)}));

    controls.apply(makeSnapshot(3, {makeMonitor(kA, 2)}));
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    controls.setTarget(kA, ManualFeature::Brightness, 70.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].generation == 2 && sink.calls[1].value == 70.0);
}

TEST(manual_shutdown_stops_submissions)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 25.0);
    controls.shutdown();
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});

    controls.setTarget(kA, ManualFeature::Brightness, 30.0);
    controls.apply(makeSnapshot(2, {makeMonitor(kA)}));
    CHECK(!controls.adoptExternalIntent(kA, 1, ManualFeature::Brightness, 56.0));
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{0});
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
}

// Applies a newer snapshot from inside submit, as a core snapshot can land while a command is handed over.
class SnapshotDuringSubmitSink final : public IManualSink {
public:
    ManualControls* controls = nullptr;
    uint64_t nextRevision = 2;

    int submit(std::string_view monitorId, ManualFeature feature, double value,
               uint64_t connectionGeneration) override
    {
        calls.push_back(SinkCall{std::string(monitorId), feature, value, connectionGeneration});
        if (controls != nullptr) {
            controls->apply(makeSnapshot(nextRevision++, {makeMonitor(kA)}));
        }
        return 0;
    }

    std::vector<SinkCall> calls;
};

TEST(manual_snapshot_during_submission_does_not_release_slot)
{
    SnapshotDuringSubmitSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    sink.controls = &controls;
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    // The snapshot inside submit is newer than the submission, yet the command is still young.
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 20.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 37.0);
}

TEST(manual_snapshot_releases_slot_only_after_safety_window)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    clock.advance(kManualInFlightSafetySeconds - 0.5);
    controls.apply(makeSnapshot(2, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    // Past the window with no completion, the snapshot releases the lost command.
    clock.advance(1.0);
    controls.apply(makeSnapshot(3, {makeMonitor(kA)}));
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 37.0);
}

TEST(manual_failed_completion_marks_draft_failed_at_observed_value)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 62.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, false, std::nullopt);
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Failed);
    CHECK(controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(controls.draft(kA, ManualFeature::Brightness) == 41.0);

    // A failed draft never retries on its own.
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.setTarget(kA, ManualFeature::Brightness, 62.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 62.0);
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);
}

TEST(manual_failed_completion_with_queued_target_sends_queued_target)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    controls.setTarget(kA, ManualFeature::Brightness, 30.0);

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, false, std::nullopt);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 30.0);
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);
}

TEST(manual_completion_sends_queued_target_without_debounce)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA)}));
    controls.setTarget(kA, ManualFeature::Brightness, 0.0);
    scheduler.advance(kSettle);
    controls.setTarget(kA, ManualFeature::Brightness, 37.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 0.0);
    scheduler.advance(std::chrono::milliseconds{0});
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 37.0);
}

TEST(manual_completion_ignored_for_other_generation_or_idle_slot)
{
    FakeSink sink;
    FakeScheduler scheduler;
    FakeClock clock;
    ManualControls controls(sink, scheduler, clock);
    controls.apply(makeSnapshot(1, {makeMonitor(kA, 1)}));
    controls.setTarget(kA, ManualFeature::Brightness, 20.0);
    scheduler.advance(kSettle);
    controls.setTarget(kA, ManualFeature::Brightness, 30.0);

    // Completion for generation 2 does not match the current generation 1.
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 2, true, 20.0);
    scheduler.advance(kSettle);
    CHECK_EQ(sink.calls.size(), std::size_t{1});
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Pending);

    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 20.0);
    scheduler.advance(kSettle);
    CHECK(sink.calls.size() == 2 && sink.calls[1].value == 30.0);
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, true, 30.0);
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));

    // A completion with nothing in flight and nothing queued changes nothing.
    controls.onHardwareCompleted(kA, ManualFeature::Brightness, 1, false, std::nullopt);
    CHECK(!controls.hasDraft(kA, ManualFeature::Brightness));
    CHECK(controls.status(kA, ManualFeature::Brightness) == ManualStatus::Ready);
}

}  // namespace
}  // namespace telaviva
