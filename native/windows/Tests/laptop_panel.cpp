#include "TestHarness.h"

#include "Fakes.h"
#include "../Platform/LaptopPanel.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using telaviva::BrightnessReadability;
using telaviva::DisplayPathInfo;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::ICoreSubmitter;
using telaviva::ILaptopEnvironment;
using telaviva::ILaptopPanel;
using telaviva::LaptopPanelFollower;
using telaviva::LaptopReason;
using telaviva::buildBuiltinDisplay;
using telaviva::laptopReasonString;

namespace {

struct FakePanel final : ILaptopPanel {
    std::optional<double> level = 40.0;
    int reads = 0;
    int writes = 0;

    std::optional<double> readPercent() override
    {
        ++reads;
        return level;
    }

    bool setPercent(double) override
    {
        ++writes;
        return true;
    }
};

struct FakeEnvironment final : ILaptopEnvironment {
    std::optional<DisplayPathInfo> path = DisplayPathInfo{true, "Built-in"};
    std::optional<bool> lid = true;
    bool display = true;
    std::optional<bool> battery = true;
    BrightnessReadability readability = BrightnessReadability::Ok;
    int probes = 0;

    std::optional<DisplayPathInfo> internalDisplayPath() override { return path; }
    std::optional<bool> lidOpen() override { return lid; }
    bool displayOn() override { return display; }
    std::optional<bool> hasBattery() override { return battery; }
    BrightnessReadability panelBrightnessReadable() override
    {
        ++probes;
        return readability;
    }
};

struct RecordingSubmitter final : ICoreSubmitter {
    std::vector<std::string> messages;

    int submit(const std::string& json) override
    {
        messages.push_back(json);
        return 0;
    }
};

std::string report(bool available, std::optional<double> brightness, bool calibrating)
{
    return buildBuiltinDisplay(available, brightness, calibrating);
}

std::string reasonName(LaptopReason reason)
{
    return laptopReasonString(reason);
}

}  // namespace

TEST(laptop_panel_desktop_never_polls_or_reads_wmi)
{
    FakePanel panel;
    FakeEnvironment env;
    env.path = std::nullopt;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    scheduler.advance(10000ms);
    CHECK_EQ(panel.reads, 0);
    CHECK_EQ(env.probes, 0);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(submitter.messages.size(), std::size_t{1});
    CHECK_EQ(submitter.messages.back(), report(false, std::nullopt, false));
    CHECK_EQ(reasonName(follower.reason()), std::string("noBuiltInDisplay"));
    CHECK(!follower.canCalibrate());
}

TEST(laptop_panel_disabled_follow_never_polls)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(false);
    scheduler.advance(10000ms);
    CHECK_EQ(panel.reads, 0);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(submitter.messages.size(), std::size_t{1});
    CHECK_EQ(submitter.messages.back(), report(true, std::nullopt, false));
    CHECK(!follower.canCalibrate());
}

TEST(laptop_panel_polls_once_a_second_with_half_second_leeway)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    CHECK_EQ(panel.reads, 0);
    scheduler.advance(0ms);
    CHECK_EQ(panel.reads, 1);
    CHECK_EQ(submitter.messages.back(), report(true, 40.0, false));
    CHECK_EQ(scheduler.toleranceLog().back().count(), 500);

    scheduler.advance(999ms);
    CHECK_EQ(panel.reads, 1);
    scheduler.advance(1ms);
    CHECK_EQ(panel.reads, 2);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    CHECK_EQ(scheduler.toleranceLog().back().count(), 500);
}

TEST(laptop_panel_submits_only_changed_whole_percent)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    scheduler.advance(0ms);
    scheduler.advance(1000ms);
    CHECK_EQ(submitter.messages.size(), std::size_t{1});

    // 40.4 rounds to 40, which is the value already sent.
    panel.level = 40.4;
    scheduler.advance(1000ms);
    CHECK_EQ(submitter.messages.size(), std::size_t{1});

    // 40.6 rounds to 41.
    panel.level = 40.6;
    scheduler.advance(1000ms);
    CHECK_EQ(submitter.messages.size(), std::size_t{2});
    CHECK_EQ(submitter.messages.back(), report(true, 41.0, false));
    CHECK_EQ(panel.reads, 4);
}

TEST(laptop_panel_failed_read_keeps_last_value_and_keeps_polling)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    scheduler.advance(0ms);
    panel.level = std::nullopt;
    scheduler.advance(1000ms);
    // The failure is not reported as zero, and nothing new is sent.
    CHECK_EQ(submitter.messages.size(), std::size_t{1});
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    CHECK(follower.canCalibrate());

    panel.level = 42.0;
    scheduler.advance(1000ms);
    CHECK_EQ(submitter.messages.size(), std::size_t{2});
    CHECK_EQ(submitter.messages.back(), report(true, 42.0, false));
}

TEST(laptop_panel_probe_missing_reports_api_unavailable_without_polling)
{
    FakePanel panel;
    FakeEnvironment env;
    env.readability = BrightnessReadability::ApiMissing;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    CHECK_EQ(submitter.messages.size(), std::size_t{1});
    CHECK_EQ(submitter.messages.back(), report(false, std::nullopt, false));
    CHECK_EQ(reasonName(follower.reason()), std::string("apiUnavailable"));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(panel.reads, 0);
}

TEST(laptop_panel_not_readable_reports_brightness_not_adjustable)
{
    FakePanel panel;
    FakeEnvironment env;
    env.readability = BrightnessReadability::NotReadable;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    CHECK_EQ(submitter.messages.back(), report(false, std::nullopt, false));
    CHECK_EQ(reasonName(follower.reason()), std::string("brightnessNotAdjustable"));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
}

TEST(laptop_panel_read_failure_after_probe_loss_reports_unavailable_once)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    panel.level = std::nullopt;
    follower.setFollowEnabled(true);
    scheduler.advance(0ms);
    // The first failed read with a readable probe keeps the panel available with no level.
    CHECK_EQ(submitter.messages.back(), report(true, std::nullopt, false));

    env.readability = BrightnessReadability::NotReadable;
    scheduler.advance(1000ms);
    CHECK_EQ(submitter.messages.size(), std::size_t{2});
    CHECK_EQ(submitter.messages.back(), report(false, std::nullopt, false));
    CHECK_EQ(reasonName(follower.reason()), std::string("brightnessNotAdjustable"));
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});

    scheduler.advance(5000ms);
    CHECK_EQ(submitter.messages.size(), std::size_t{2});
    CHECK_EQ(panel.reads, 2);
}

TEST(laptop_panel_disable_stops_polling_and_keeps_availability)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    scheduler.advance(0ms);
    follower.setFollowEnabled(false);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(submitter.messages.back(), report(true, std::nullopt, false));

    scheduler.advance(5000ms);
    CHECK_EQ(panel.reads, 1);
    CHECK_EQ(submitter.messages.size(), std::size_t{2});
}

TEST(laptop_panel_same_follow_value_does_not_reevaluate)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);

    follower.setFollowEnabled(true);
    const int probesBefore = env.probes;
    follower.setFollowEnabled(true);
    CHECK_EQ(env.probes, probesBefore);
    CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
}
