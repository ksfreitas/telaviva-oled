#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/KeepAwakeController.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::IKeepAwakeBackend;
using telaviva::KeepAwakeController;
using telaviva::KeepAwakeSafety;
using telaviva::KeepAwakeStatus;

namespace {

// Timeline steps are 125 ms, so every steady time is an exact binary fraction of a
// second. That keeps the deadline arithmetic exact and lets timers land on the deadline.
constexpr long long kStepMs = 125;

// Records the OS effects in order. Declared before the controller so it outlives it.
struct FakeBackend final : IKeepAwakeBackend {
    KeepAwakeSafety safety = KeepAwakeSafety::Eligible;
    bool posting = true;
    bool acquireOk = true;
    bool postOk = true;
    int acquires = 0;
    int releases = 0;
    int posts = 0;
    std::vector<std::string> log;

    KeepAwakeSafety safetyState() override { return safety; }
    bool postingAllowed() override { return posting; }

    bool acquireRequest() override
    {
        ++acquires;
        log.push_back("acquire");
        return acquireOk;
    }

    void releaseRequest() override
    {
        ++releases;
        log.push_back("release");
    }

    bool postActivity() override
    {
        ++posts;
        log.push_back("post");
        return postOk;
    }
};

// Owns the clock and scheduler together, so they always move in step.
struct Timeline {
    FakeClock clock;
    FakeScheduler scheduler;
    long long ms = 0;
    uint64_t epochBase = 0;

    // Moves both forward by spanMs, which must be a multiple of kStepMs.
    void advance(long long spanMs)
    {
        for (long long done = 0; done < spanMs; done += kStepMs) {
            ms += kStepMs;
            clock.setSteadySeconds(static_cast<double>(ms) / 1000.0);
            clock.setEpochMillis(epochBase + static_cast<uint64_t>(ms));
            scheduler.advance(std::chrono::milliseconds(kStepMs));
        }
    }
};

std::string joined(const std::vector<std::string>& entries)
{
    std::string text;
    for (const std::string& entry : entries) {
        if (!text.empty()) {
            text += ',';
        }
        text += entry;
    }
    return text;
}

// Prints an optional<bool> as true, false or nil.
std::string guardText(const std::optional<bool>& value)
{
    if (!value) {
        return "nil";
    }
    return *value ? "true" : "false";
}

// Accepts the first succeedCalls timers and fails every later one. Accepted timers never
// fire, and cancel does nothing, so the test controls only the scheduling outcome.
class FailingScheduler final : public telaviva::IScheduler {
public:
    explicit FailingScheduler(int succeedCalls) : remaining_(succeedCalls) {}

    telaviva::TimerId scheduleOnce(std::chrono::milliseconds, std::chrono::milliseconds,
                                   std::function<void()>) override
    {
        if (remaining_ <= 0) {
            return telaviva::kInvalidTimerId;
        }
        --remaining_;
        return nextId_++;
    }

    void cancel(telaviva::TimerId) override {}

private:
    int remaining_;
    telaviva::TimerId nextId_ = 1;
};

// Answers from a field the test sets, and counts how often it was asked.
struct CountingGuard {
    std::optional<bool> answer;
    int queries = 0;

    std::optional<bool> query()
    {
        ++queries;
        return answer;
    }
};

}  // namespace

TEST(keep_awake_disabled_or_idle_has_no_timers_or_effects)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(false, true, 1, 10, 3000);
    timeline.advance(5000);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(joined(backend.log), std::string(""));
    CHECK(controller.status() == KeepAwakeStatus::Disabled);

    // Enabled, but nothing is requested.
    controller.update(true, false, 1, 0, 0);
    timeline.advance(5000);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(joined(backend.log), std::string(""));
    CHECK(controller.status() == KeepAwakeStatus::AwaitingPresence);
}

TEST(keep_awake_valid_token_acquires_then_pulses_each_second)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(joined(backend.log), std::string("acquire,post"));
    CHECK(controller.status() == KeepAwakeStatus::Active);
    // One pulse timer and one expiry timer.
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{2});

    timeline.advance(1000);
    CHECK_EQ(backend.posts, 2);
    timeline.advance(1000);
    CHECK_EQ(backend.posts, 3);
    CHECK_EQ(backend.acquires, 1);
    CHECK_EQ(backend.releases, 0);
}

TEST(keep_awake_same_token_never_extends_deadline)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);  // deadline at 3 s
    timeline.advance(1000);
    controller.update(true, true, 1, 10, 5000);  // a larger TTL must not move the deadline
    timeline.advance(1875);                      // now 2875 ms
    CHECK_EQ(backend.releases, 0);
    timeline.advance(125);                       // now 3000 ms
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::AwaitingPresence);
}

TEST(keep_awake_same_token_shorter_ttl_shortens_deadline)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);  // deadline at 3 s
    timeline.advance(500);
    controller.update(true, true, 1, 10, 1000);  // remaining 1 s from 500 ms: deadline 1500 ms
    timeline.advance(875);                       // now 1375 ms
    CHECK_EQ(backend.releases, 0);
    timeline.advance(125);                       // now 1500 ms
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::AwaitingPresence);
}

TEST(keep_awake_expiry_releases_once_and_stops_pulses)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    timeline.advance(2875);
    CHECK_EQ(backend.posts, 3);  // at 0, 1000 and 2000 ms
    timeline.advance(125);       // expiry at exactly 3000 ms
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::AwaitingPresence);
    // The pulse due at the deadline must not post.
    CHECK_EQ(backend.posts, 3);

    timeline.advance(5000);
    CHECK_EQ(backend.posts, 3);
    CHECK_EQ(backend.releases, 1);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});

    // The revoked token cannot come back.
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(backend.acquires, 1);
}

TEST(keep_awake_newer_token_replaces_deadline_and_posts_at_once)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);  // deadline at 3 s
    timeline.advance(1000);
    controller.update(true, true, 1, 11, 3000);  // newer observation: deadline at 4 s
    CHECK_EQ(backend.acquires, 1);
    CHECK_EQ(backend.posts, 3);                  // timer pulse at 1 s, then the immediate pulse

    // An older observation is ignored and does not restore the old deadline.
    controller.update(true, true, 1, 10, 9000);
    CHECK_EQ(backend.posts, 3);
    CHECK_EQ(backend.acquires, 1);
    // An older generation is ignored too.
    controller.update(true, true, 0, 99, 9000);
    CHECK_EQ(backend.posts, 3);

    timeline.advance(2875);  // now 3875 ms
    CHECK_EQ(backend.releases, 0);
    timeline.advance(125);   // now 4000 ms
    CHECK_EQ(backend.releases, 1);
}

TEST(keep_awake_revoked_token_cannot_be_replayed)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    controller.update(true, false, 1, 0, 0);  // first absence
    CHECK_EQ(backend.releases, 1);

    controller.update(true, true, 1, 10, 3000);  // replay of the revoked token
    controller.update(true, true, 1, 9, 3000);   // older replay
    CHECK_EQ(backend.acquires, 1);

    controller.update(true, true, 1, 11, 3000);  // a newer observation is accepted
    CHECK_EQ(backend.acquires, 2);
}

TEST(keep_awake_first_absence_releases_immediately)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(backend.acquires, 1);

    controller.update(true, false, 1, 0, 0);
    CHECK_EQ(backend.releases, 1);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});
    CHECK(controller.status() == KeepAwakeStatus::AwaitingPresence);

    timeline.advance(5000);
    CHECK_EQ(backend.posts, 1);
}

TEST(keep_awake_disable_releases_and_reports_disabled)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    controller.update(false, true, 1, 10, 3000);
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::Disabled);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});

    // Re-enabling does not restore the disabled token.
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(backend.acquires, 1);
}

TEST(keep_awake_session_ineligible_revokes_and_requires_newer_generation)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(backend.acquires, 1);

    controller.setSessionEligible(false);
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::Suspended);

    controller.setSessionEligible(true);
    // The same generation, and an older one, are ignored after resume.
    controller.update(true, true, 1, 11, 3000);
    CHECK_EQ(backend.acquires, 1);
    controller.update(true, true, 2, 20, 3000);
    CHECK_EQ(backend.acquires, 1);
    // A newer generation with fresh evidence reactivates the effect.
    controller.update(true, true, 3, 15, 3000);
    CHECK_EQ(backend.acquires, 2);
    CHECK(controller.status() == KeepAwakeStatus::Active);
}

TEST(keep_awake_safety_veto_requires_evidence_after_transition)
{
    Timeline timeline;
    timeline.epochBase = 5000;  // wall clock reads 5000 ms at steady time 0
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 4000, 3000);
    CHECK_EQ(backend.acquires, 1);

    timeline.advance(500);  // wall clock 5500 ms
    backend.safety = KeepAwakeSafety::ScreensaverActive;
    controller.safetyDidChange();
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::Suspended);

    timeline.advance(500);  // wall clock 6000 ms, the transition back to eligible
    backend.safety = KeepAwakeSafety::Eligible;
    controller.safetyDidChange();

    // Evidence captured at or before the transition is ignored.
    controller.update(true, true, 1, 5900, 3000);
    CHECK_EQ(backend.acquires, 1);
    // Evidence captured after the transition is accepted.
    controller.update(true, true, 1, 6001, 3000);
    CHECK_EQ(backend.acquires, 2);
    CHECK(controller.status() == KeepAwakeStatus::Active);
}

TEST(keep_awake_acquire_failure_is_unavailable_without_post)
{
    Timeline timeline;
    FakeBackend backend;
    backend.acquireOk = false;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(joined(backend.log), std::string("acquire"));
    CHECK_EQ(backend.releases, 0);
    CHECK(controller.status() == KeepAwakeStatus::Unavailable);

    timeline.advance(1000);
    CHECK_EQ(backend.posts, 0);
}

TEST(keep_awake_post_failure_is_unavailable_and_releases)
{
    Timeline timeline;
    FakeBackend backend;
    backend.postOk = false;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(joined(backend.log), std::string("acquire,post,release"));
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::Unavailable);
}

TEST(keep_awake_posting_blocked_by_policy_never_acquires)
{
    Timeline timeline;
    FakeBackend backend;
    backend.posting = false;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(backend.acquires, 0);
    CHECK_EQ(backend.posts, 0);
    CHECK(controller.status() == KeepAwakeStatus::PolicyBlocked);
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(controller.status())), std::string("policy"));
}

TEST(keep_awake_shutdown_releases_cancels_and_becomes_inert)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    timeline.advance(1000);
    CHECK_EQ(backend.posts, 2);

    controller.shutdown();
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::Disabled);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});

    // Shutdown is idempotent, and later calls do nothing.
    controller.shutdown();
    CHECK_EQ(backend.releases, 1);
    controller.update(true, true, 1, 11, 3000);
    controller.setSessionEligible(false);
    controller.safetyDidChange();
    timeline.advance(5000);
    CHECK_EQ(backend.acquires, 1);
    CHECK_EQ(backend.posts, 2);
    CHECK_EQ(backend.releases, 1);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});
    CHECK(controller.status() == KeepAwakeStatus::Disabled);
}

TEST(keep_awake_destruction_releases_request_and_cancels_timers)
{
    Timeline timeline;
    FakeBackend backend;
    {
        KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);
        controller.setSessionEligible(true);
        controller.update(true, true, 1, 10, 3000);
        CHECK_EQ(backend.acquires, 1);
    }
    CHECK_EQ(backend.releases, 1);
    CHECK_EQ(timeline.scheduler.pendingCount(), std::size_t{0});
}

TEST(keep_awake_zero_ttl_or_disabled_never_acquires)
{
    Timeline timeline;
    FakeBackend backend;
    KeepAwakeController controller(backend, timeline.scheduler, timeline.clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 0);
    controller.update(false, true, 1, 11, 3000);
    controller.update(true, true, 1, 12, 0);
    CHECK_EQ(backend.acquires, 0);
    CHECK_EQ(backend.posts, 0);
    CHECK(controller.status() == KeepAwakeStatus::AwaitingPresence);

    timeline.advance(3000);
    CHECK_EQ(backend.acquires, 0);
    CHECK_EQ(backend.posts, 0);
}

TEST(keep_awake_safety_resolution_table)
{
    const std::optional<bool> unknown;
    const std::optional<bool> on = true;
    const std::optional<bool> off = false;

    CHECK(telaviva::resolveKeepAwakeSafety(true, off, off, false, false) == KeepAwakeSafety::Eligible);
    CHECK(telaviva::resolveKeepAwakeSafety(true, off, on, false, false) == KeepAwakeSafety::ScreensaverActive);
    CHECK(telaviva::resolveKeepAwakeSafety(true, off, unknown, false, false) == KeepAwakeSafety::ScreensaverStateUnknown);
    CHECK(telaviva::resolveKeepAwakeSafety(true, off, off, true, false) == KeepAwakeSafety::DisplayAsleep);
    CHECK(telaviva::resolveKeepAwakeSafety(true, unknown, off, false, false) == KeepAwakeSafety::SessionStateUnknown);
    CHECK(telaviva::resolveKeepAwakeSafety(true, on, off, false, false) == KeepAwakeSafety::SessionInactive);
    CHECK(telaviva::resolveKeepAwakeSafety(false, off, off, false, false) == KeepAwakeSafety::SessionInactive);
    CHECK(telaviva::resolveKeepAwakeSafety(true, on, off, false, true) == KeepAwakeSafety::SystemSleeping);
    // Display sleep is checked before the screensaver.
    CHECK(telaviva::resolveKeepAwakeSafety(true, off, on, true, false) == KeepAwakeSafety::DisplayAsleep);
    CHECK(telaviva::resolveKeepAwakeSafety(true, off, unknown, true, false) == KeepAwakeSafety::DisplayAsleep);
    CHECK(telaviva::resolveKeepAwakeSafety(true, unknown, unknown, true, false) == KeepAwakeSafety::SessionStateUnknown);
    CHECK(telaviva::resolveKeepAwakeSafety(true, unknown, on, false, false) == KeepAwakeSafety::SessionStateUnknown);
    CHECK(telaviva::resolveKeepAwakeSafety(true, off, unknown, false, true) == KeepAwakeSafety::SystemSleeping);
    CHECK(telaviva::resolveKeepAwakeSafety(false, unknown, unknown, true, true) == KeepAwakeSafety::SystemSleeping);
}

TEST(keep_awake_camera_monitoring_table)
{
    CHECK(telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::Eligible));
    CHECK(telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::ScreensaverActive));
    CHECK(telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::ScreensaverStateUnknown));
    CHECK(telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::DisplayAsleep));
    CHECK(telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::LidClosed));
    CHECK(!telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::SessionInactive));
    CHECK(!telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::SessionStateUnknown));
    CHECK(!telaviva::keepAwakeCameraMonitoringAllowed(KeepAwakeSafety::SystemSleeping));
}

TEST(keep_awake_guard_resolution)
{
    CountingGuard live;

    // A positive notification wins without asking the live query.
    live.answer = false;
    CHECK_EQ(guardText(telaviva::resolveKeepAwakeGuard(true, [&live] { return live.query(); })), std::string("true"));
    CHECK_EQ(live.queries, 0);

    // A negative or missing notification defers to the live query.
    live.answer = false;
    CHECK_EQ(guardText(telaviva::resolveKeepAwakeGuard(false, [&live] { return live.query(); })), std::string("false"));
    CHECK_EQ(live.queries, 1);
    CHECK_EQ(guardText(telaviva::resolveKeepAwakeGuard(std::nullopt, [&live] { return live.query(); })),
             std::string("false"));
    CHECK_EQ(live.queries, 2);

    // A stale negative notification cannot clear a live lock or screensaver.
    live.answer = true;
    CHECK_EQ(guardText(telaviva::resolveKeepAwakeGuard(false, [&live] { return live.query(); })), std::string("true"));

    // An unknown live state stays unknown, even with a negative notification.
    live.answer = std::nullopt;
    CHECK_EQ(guardText(telaviva::resolveKeepAwakeGuard(false, [&live] { return live.query(); })), std::string("nil"));
    CHECK_EQ(guardText(telaviva::resolveKeepAwakeGuard(std::nullopt, [&live] { return live.query(); })),
             std::string("nil"));
}

TEST(keep_awake_expiry_timer_failure_never_acquires)
{
    telaviva::FakeClock clock;
    FailingScheduler scheduler(0);
    FakeBackend backend;
    KeepAwakeController controller(backend, scheduler, clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    // Without an expiry watchdog the lease must never be held.
    CHECK_EQ(backend.acquires, 0);
    CHECK_EQ(backend.posts, 0);
    CHECK_EQ(backend.releases, 0);
    CHECK(controller.status() == KeepAwakeStatus::Unavailable);
}

TEST(keep_awake_pulse_timer_failure_releases_and_is_unavailable)
{
    telaviva::FakeClock clock;
    // The expiry timer (first call) succeeds; the pulse timer (second call) fails.
    FailingScheduler scheduler(1);
    FakeBackend backend;
    KeepAwakeController controller(backend, scheduler, clock);

    controller.setSessionEligible(true);
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(joined(backend.log), std::string("acquire,post,release"));
    CHECK_EQ(backend.releases, 1);
    CHECK(controller.status() == KeepAwakeStatus::Unavailable);

    // The revoked token cannot restart the pulse chain.
    controller.update(true, true, 1, 10, 3000);
    CHECK_EQ(backend.acquires, 1);
}

TEST(keep_awake_status_names)
{
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(KeepAwakeStatus::Disabled)), std::string("disabled"));
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(KeepAwakeStatus::AwaitingPresence)), std::string("awaiting"));
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(KeepAwakeStatus::Active)), std::string("active"));
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(KeepAwakeStatus::PolicyBlocked)), std::string("policy"));
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(KeepAwakeStatus::Unavailable)), std::string("unavailable"));
    CHECK_EQ(std::string(telaviva::keepAwakeStatusName(KeepAwakeStatus::Suspended)), std::string("suspended"));
}
