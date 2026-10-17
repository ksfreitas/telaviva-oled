#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/ScreensaverMonitor.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

using namespace std::chrono_literals;
using telaviva::FakeScheduler;
using telaviva::IScreensaverProbe;
using telaviva::ScreensaverMonitor;
using telaviva::ScreensaverState;

namespace {

// Answers from a field the test sets. std::nullopt simulates a failed query.
struct ScriptedProbe final : IScreensaverProbe {
    std::optional<bool> answer = false;
    int calls = 0;

    std::optional<bool> isRunning() override
    {
        ++calls;
        return answer;
    }
};

// One char per published state: 'u' unknown, 'i' inactive, 'a' active, 'x' unavailable.
char code(ScreensaverState state)
{
    switch (state) {
    case ScreensaverState::Unknown:
        return 'u';
    case ScreensaverState::Inactive:
        return 'i';
    case ScreensaverState::Active:
        return 'a';
    case ScreensaverState::Unavailable:
        return 'x';
    }
    return '?';
}

std::string stateName(ScreensaverState state)
{
    return telaviva::screensaverStateString(state);
}

// Declare before the monitor so it outlives it.
struct Recorder {
    std::string published;

    std::function<void(ScreensaverState)> callback()
    {
        return [this](ScreensaverState state) { published += code(state); };
    }
};

}  // namespace

TEST(screensaver_nothing_runs_until_enabled)
{
    ScriptedProbe probe;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    scheduler.advance(10000ms);
    CHECK_EQ(probe.calls, 0);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(rec.published, std::string(""));
    CHECK_EQ(stateName(monitor.state()), std::string("unknown"));
}

TEST(screensaver_enable_reads_at_once_and_sees_active_at_startup)
{
    ScriptedProbe probe;
    probe.answer = true;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    CHECK_EQ(probe.calls, 1);
    CHECK_EQ(rec.published, std::string("a"));
    CHECK_EQ(stateName(monitor.state()), std::string("active"));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
}

TEST(screensaver_steady_state_publishes_once)
{
    ScriptedProbe probe;
    probe.answer = false;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    scheduler.advance(2000ms);
    CHECK_EQ(probe.calls, 2);
    scheduler.advance(4000ms);
    CHECK_EQ(probe.calls, 4);
    CHECK_EQ(rec.published, std::string("i"));
}

TEST(screensaver_every_change_is_published)
{
    ScriptedProbe probe;
    probe.answer = false;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    probe.answer = true;
    scheduler.advance(2000ms);
    probe.answer = false;
    scheduler.advance(2000ms);
    CHECK_EQ(rec.published, std::string("iai"));
    CHECK_EQ(stateName(monitor.state()), std::string("inactive"));
}

TEST(screensaver_disable_cancels_timer_and_publishes_unknown_once)
{
    ScriptedProbe probe;
    probe.answer = true;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    monitor.setEnabled(false);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(stateName(monitor.state()), std::string("unknown"));

    scheduler.advance(10000ms);
    CHECK_EQ(probe.calls, 1);
    // Disabling again must not publish a second Unknown.
    monitor.setEnabled(false);
    scheduler.advance(10000ms);
    CHECK_EQ(probe.calls, 1);
    CHECK_EQ(rec.published, std::string("au"));
}

TEST(screensaver_reenable_reads_and_polls_again)
{
    ScriptedProbe probe;
    probe.answer = true;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    monitor.setEnabled(false);
    monitor.setEnabled(true);
    CHECK_EQ(probe.calls, 2);
    // The first read after re-enabling publishes even though the value is unchanged.
    CHECK_EQ(rec.published, std::string("aua"));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});

    probe.answer = false;
    scheduler.advance(2000ms);
    CHECK_EQ(rec.published, std::string("auai"));
}

TEST(screensaver_probe_failure_publishes_unavailable_and_recovers)
{
    ScriptedProbe probe;
    probe.answer = std::nullopt;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    CHECK_EQ(stateName(monitor.state()), std::string("unavailable"));
    scheduler.advance(2000ms);
    CHECK_EQ(rec.published, std::string("x"));

    probe.answer = true;
    scheduler.advance(2000ms);
    CHECK_EQ(stateName(monitor.state()), std::string("active"));
    CHECK_EQ(rec.published, std::string("xa"));

    probe.answer = std::nullopt;
    scheduler.advance(2000ms);
    CHECK_EQ(rec.published, std::string("xax"));
    // Polling continues through failures.
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
}

TEST(screensaver_tick_rearms_with_interval_and_tolerance)
{
    ScriptedProbe probe;
    Recorder rec;
    FakeScheduler scheduler;
    ScreensaverMonitor monitor(scheduler, probe, rec.callback());

    monitor.setEnabled(true);
    CHECK_EQ(scheduler.toleranceLog().size(), std::size_t{1});
    CHECK_EQ(scheduler.toleranceLog().back().count(), telaviva::toleranceFor(2000ms).count());

    scheduler.advance(1999ms);
    CHECK_EQ(probe.calls, 1);
    scheduler.advance(1ms);
    CHECK_EQ(probe.calls, 2);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    CHECK_EQ(scheduler.toleranceLog().size(), std::size_t{2});
    CHECK_EQ(scheduler.toleranceLog().back().count(), telaviva::toleranceFor(2000ms).count());
}

TEST(screensaver_destruction_cancels_timer)
{
    ScriptedProbe probe;
    Recorder rec;
    FakeScheduler scheduler;
    {
        ScreensaverMonitor monitor(scheduler, probe, rec.callback());
        monitor.setEnabled(true);
        CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    }
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    scheduler.advance(10000ms);
    CHECK_EQ(probe.calls, 1);
}
