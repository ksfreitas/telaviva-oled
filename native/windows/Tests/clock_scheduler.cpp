#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/Scheduler.h"
#include "Platform/SystemClock.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::SystemClock;
using telaviva::ThreadpoolScheduler;
using telaviva::TimerId;
using telaviva::toleranceFor;

namespace {

bool waitUntil(const std::atomic<int>& value, int target, std::chrono::milliseconds bound)
{
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (value.load() < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

}  // namespace

TEST(clock_scheduler_tolerance_for_known_values)
{
    CHECK_EQ(toleranceFor(0ms).count(), 0);
    CHECK_EQ(toleranceFor(100ms).count(), 10);
    CHECK_EQ(toleranceFor(5000ms).count(), 500);
    CHECK_EQ(toleranceFor(60000ms).count(), 1000);
    CHECK_EQ(toleranceFor(1ms).count(), 0);
}

TEST(clock_scheduler_tolerance_for_never_exceeds_rule)
{
    for (long long ms = 0; ms <= 20000; ms += 7) {
        const auto delay = std::chrono::milliseconds(ms);
        const auto tolerance = toleranceFor(delay);
        CHECK(tolerance.count() >= 0);
        CHECK(tolerance.count() * 10 <= delay.count());
        CHECK(tolerance.count() <= 1000);
    }
}

TEST(clock_scheduler_fake_scheduler_fires_in_due_order)
{
    FakeScheduler scheduler;
    std::string trace;
    scheduler.scheduleOnce(30ms, 3ms, [&trace] { trace += 'c'; });
    scheduler.scheduleOnce(10ms, 1ms, [&trace] { trace += 'a'; });
    scheduler.scheduleOnce(20ms, 2ms, [&trace] { trace += 'b'; });

    scheduler.advance(9ms);
    CHECK_EQ(trace, std::string(""));
    scheduler.advance(1ms);
    CHECK_EQ(trace, std::string("a"));
    scheduler.advance(100ms);
    CHECK_EQ(trace, std::string("abc"));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
}

TEST(clock_scheduler_fake_scheduler_honors_cancel)
{
    FakeScheduler scheduler;
    std::string trace;
    const TimerId first = scheduler.scheduleOnce(10ms, 0ms, [&trace] { trace += 'a'; });
    scheduler.scheduleOnce(20ms, 0ms, [&trace] { trace += 'b'; });
    scheduler.cancel(first);
    scheduler.cancel(first);
    scheduler.advance(50ms);
    CHECK_EQ(trace, std::string("b"));
}

TEST(clock_scheduler_fake_scheduler_records_tolerance)
{
    FakeScheduler scheduler;
    scheduler.scheduleOnce(100ms, 10ms, [] {});
    scheduler.scheduleOnce(5000ms, 500ms, [] {});
    CHECK_EQ(scheduler.toleranceLog().size(), std::size_t{2});
    if (scheduler.toleranceLog().size() == 2) {
        CHECK_EQ(scheduler.toleranceLog()[0].count(), 10);
        CHECK_EQ(scheduler.toleranceLog()[1].count(), 500);
    }
}

TEST(clock_scheduler_fake_clock_advances_both_readings)
{
    FakeClock clock;
    clock.setSteadySeconds(10.0);
    clock.setEpochMillis(1000);
    clock.advance(2.5);
    CHECK_EQ(clock.steadySeconds(), 12.5);
    CHECK_EQ(clock.epochMillis(), uint64_t{3500});
}

TEST(clock_scheduler_system_clock_reports_plausible_values)
{
    SystemClock clock;
    const double first = clock.steadySeconds();
    const uint64_t epoch = clock.epochMillis();
    CHECK(first >= 0.0);
    // Any date after 2020-01-01 in Unix milliseconds.
    CHECK(epoch > 1577836800000ULL);
    CHECK(clock.steadySeconds() >= first);
}

TEST(clock_scheduler_threadpool_fires_within_bound)
{
    // Declared after the state it captures, so it is destroyed first.
    std::atomic<int> fired{0};
    ThreadpoolScheduler scheduler;
    scheduler.scheduleOnce(30ms, toleranceFor(30ms), [&fired] { fired.fetch_add(1); });
    CHECK(waitUntil(fired, 1, 2000ms));
    CHECK_EQ(fired.load(), 1);
}

TEST(clock_scheduler_threadpool_cancelled_timer_never_fires)
{
    std::atomic<int> fired{0};
    ThreadpoolScheduler scheduler;
    const TimerId id = scheduler.scheduleOnce(200ms, toleranceFor(200ms), [&fired] { fired.fetch_add(1); });
    scheduler.cancel(id);
    std::this_thread::sleep_for(400ms);
    CHECK_EQ(fired.load(), 0);
}

TEST(clock_scheduler_threadpool_destructor_cancels_pending)
{
    std::atomic<int> fired{0};
    {
        ThreadpoolScheduler scheduler;
        scheduler.scheduleOnce(100ms, toleranceFor(100ms), [&fired] { fired.fetch_add(1); });
    }
    std::this_thread::sleep_for(250ms);
    CHECK_EQ(fired.load(), 0);
}

TEST(clock_scheduler_threadpool_cancel_after_fire_is_harmless)
{
    std::atomic<int> fired{0};
    ThreadpoolScheduler scheduler;
    const TimerId id = scheduler.scheduleOnce(10ms, toleranceFor(10ms), [&fired] { fired.fetch_add(1); });
    CHECK(waitUntil(fired, 1, 2000ms));
    scheduler.cancel(id);
    scheduler.cancel(id);
    scheduler.cancel(telaviva::kInvalidTimerId);
    CHECK_EQ(fired.load(), 1);
}
