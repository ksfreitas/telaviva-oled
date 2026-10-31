#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/LockCoordinator.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using telaviva::FakeScheduler;
using telaviva::LockCoordinator;

namespace {

// Answers from a field the test sets, runs an optional hook inside the call, and counts calls.
class FakeLocker final : public telaviva::ISessionLocker {
public:
    bool result = true;
    int calls = 0;
    std::function<void()> hook;

    bool lockNow() override
    {
        ++calls;
        if (hook) {
            hook();
        }
        return result;
    }
};

struct Submitted {
    uint64_t token;
    bool ok;
    std::string error;
};

// Declared in the order the coordinator uses them, so the coordinator dies first.
struct Harness {
    FakeLocker locker;
    FakeScheduler scheduler;
    std::vector<Submitted> submitted;
    std::vector<std::string> logs;
    LockCoordinator coordinator;

    explicit Harness(std::chrono::milliseconds window = std::chrono::milliseconds(3000))
        : coordinator(
              locker, scheduler,
              [this](uint64_t token, bool ok, const std::string& error) { submitted.push_back({token, ok, error}); },
              [this](const std::string& line) { logs.push_back(line); }, window)
    {
    }
};

std::string joinedLogs(const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

}  // namespace

TEST(session_lock_one_lock_per_token)
{
    Harness h;
    h.coordinator.observe(true, 7);
    CHECK_EQ(h.locker.calls, 1);
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{1});
    h.coordinator.observe(true, 7);  // repeated while pending
    CHECK_EQ(h.locker.calls, 1);

    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.submitted.size(), std::size_t{1});
    h.coordinator.observe(true, 7);  // repeated after the result
    CHECK_EQ(h.locker.calls, 1);
    CHECK_EQ(h.submitted.size(), std::size_t{1});
}

TEST(session_lock_not_requested_or_missing_token_keeps_remembered_token)
{
    Harness h;
    h.coordinator.observe(true, 7);
    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.locker.calls, 1);

    h.coordinator.observe(false, 7);
    h.coordinator.observe(true, std::nullopt);
    CHECK_EQ(h.locker.calls, 1);

    // Token 7 is still remembered, so it does not lock again.
    h.coordinator.observe(true, 7);
    CHECK_EQ(h.locker.calls, 1);
    CHECK_EQ(h.submitted.size(), std::size_t{1});
}

TEST(session_lock_new_token_acts_again)
{
    Harness h;
    h.coordinator.observe(true, 7);
    h.coordinator.sessionLockChanged(true);
    h.coordinator.observe(true, 8);
    CHECK_EQ(h.locker.calls, 2);
    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.submitted.size(), std::size_t{2});
    CHECK_EQ(h.submitted[1].token, uint64_t{8});
    CHECK(h.submitted[1].ok);
}

TEST(session_lock_event_within_window_confirms_once_and_cancels_timer)
{
    Harness h;
    h.coordinator.observe(true, 7);
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{1});

    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.submitted.size(), std::size_t{1});
    CHECK_EQ(h.submitted[0].token, uint64_t{7});
    CHECK(h.submitted[0].ok);
    CHECK_EQ(h.submitted[0].error, std::string(""));
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{0});

    h.scheduler.advance(std::chrono::milliseconds(5000));
    CHECK_EQ(h.submitted.size(), std::size_t{1});
}

TEST(session_lock_no_event_times_out_as_not_confirmed)
{
    Harness h;
    h.coordinator.observe(true, 7);

    h.scheduler.advance(std::chrono::milliseconds(2999));
    CHECK_EQ(h.submitted.size(), std::size_t{0});

    h.scheduler.advance(std::chrono::milliseconds(1));
    CHECK_EQ(h.submitted.size(), std::size_t{1});
    CHECK_EQ(h.submitted[0].token, uint64_t{7});
    CHECK(!h.submitted[0].ok);
    CHECK_EQ(h.submitted[0].error, std::string("not_confirmed"));

    h.scheduler.advance(std::chrono::milliseconds(10000));
    CHECK_EQ(h.submitted.size(), std::size_t{1});
}

TEST(session_lock_event_after_timeout_is_ignored)
{
    Harness h;
    h.coordinator.observe(true, 7);
    h.scheduler.advance(std::chrono::milliseconds(3000));
    CHECK_EQ(h.submitted.size(), std::size_t{1});

    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.submitted.size(), std::size_t{1});
}

TEST(session_lock_locker_refusal_fails_at_once_without_timer)
{
    Harness h;
    h.locker.result = false;
    h.coordinator.observe(true, 7);

    CHECK_EQ(h.submitted.size(), std::size_t{1});
    CHECK_EQ(h.submitted[0].token, uint64_t{7});
    CHECK(!h.submitted[0].ok);
    CHECK_EQ(h.submitted[0].error, std::string("unavailable"));
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(joinedLogs(h.logs), std::string("lock failed: unavailable\n"));
}

TEST(session_lock_event_with_nothing_pending_is_ignored)
{
    Harness h;
    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.submitted.size(), std::size_t{0});
    CHECK_EQ(h.logs.size(), std::size_t{0});
}

TEST(session_lock_unlock_event_is_ignored)
{
    Harness h;
    h.coordinator.observe(true, 7);
    h.coordinator.sessionLockChanged(false);
    CHECK_EQ(h.submitted.size(), std::size_t{0});
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{1});

    h.coordinator.sessionLockChanged(true);
    CHECK_EQ(h.submitted.size(), std::size_t{1});
    CHECK(h.submitted[0].ok);
}

TEST(session_lock_event_raised_inside_locker_confirms_once_without_timer)
{
    Harness h;
    h.locker.hook = [&h] { h.coordinator.sessionLockChanged(true); };
    h.coordinator.observe(true, 7);

    CHECK_EQ(h.submitted.size(), std::size_t{1});
    CHECK_EQ(h.submitted[0].token, uint64_t{7});
    CHECK(h.submitted[0].ok);
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{0});

    h.scheduler.advance(std::chrono::milliseconds(5000));
    CHECK_EQ(h.submitted.size(), std::size_t{1});
}

TEST(session_lock_log_lines_never_contain_the_token)
{
    const std::string token = "90817263";

    Harness refused;
    refused.locker.result = false;
    refused.coordinator.observe(true, 90817263);

    Harness confirmed;
    confirmed.coordinator.observe(true, 90817263);
    confirmed.coordinator.sessionLockChanged(true);

    Harness timedOut;
    timedOut.coordinator.observe(true, 90817263);
    timedOut.scheduler.advance(std::chrono::milliseconds(3000));

    CHECK(!refused.logs.empty());
    CHECK(!confirmed.logs.empty());
    CHECK(!timedOut.logs.empty());
    CHECK(joinedLogs(refused.logs).find(token) == std::string::npos);
    CHECK(joinedLogs(confirmed.logs).find(token) == std::string::npos);
    CHECK(joinedLogs(timedOut.logs).find(token) == std::string::npos);
}

TEST(session_lock_shutdown_cancels_timer_and_submits_nothing)
{
    Harness h;
    h.coordinator.observe(true, 7);
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{1});

    h.coordinator.shutdown();
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{0});

    // Shutdown is idempotent, and later calls do nothing.
    h.coordinator.shutdown();
    h.coordinator.observe(true, 8);
    h.coordinator.sessionLockChanged(true);
    h.scheduler.advance(std::chrono::milliseconds(5000));
    CHECK_EQ(h.locker.calls, 1);
    CHECK_EQ(h.submitted.size(), std::size_t{0});
    CHECK_EQ(h.scheduler.pendingCount(), std::size_t{0});
}

TEST(session_lock_destruction_cancels_timer)
{
    FakeLocker locker;
    FakeScheduler scheduler;
    int submits = 0;
    {
        LockCoordinator coordinator(
            locker, scheduler, [&submits](uint64_t, bool, const std::string&) { ++submits; },
            [](const std::string&) {}, std::chrono::milliseconds(3000));
        coordinator.observe(true, 7);
        CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    }
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    scheduler.advance(std::chrono::milliseconds(5000));
    CHECK_EQ(submits, 0);
}

TEST(session_lock_custom_confirm_window_is_honored)
{
    Harness h(std::chrono::milliseconds(500));
    h.coordinator.observe(true, 7);

    h.scheduler.advance(std::chrono::milliseconds(499));
    CHECK_EQ(h.submitted.size(), std::size_t{0});

    h.scheduler.advance(std::chrono::milliseconds(1));
    CHECK_EQ(h.submitted.size(), std::size_t{1});
    CHECK_EQ(h.submitted[0].error, std::string("not_confirmed"));
}
