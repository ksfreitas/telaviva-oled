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

    std::optional<double> readPercent() override
    {
        ++reads;
        return level;
    }

    bool setPercent(double) override { return true; }

    std::vector<int> supportedLevels() const override { return {}; }
};

struct FakeEnvironment final : ILaptopEnvironment {
    std::optional<DisplayPathInfo> path = DisplayPathInfo{true, "Built-in"};
    std::optional<bool> lid = true;
    bool display = true;
    std::optional<bool> battery = true;
    BrightnessReadability readability = BrightnessReadability::Ok;

    std::optional<DisplayPathInfo> internalDisplayPath() override { return path; }
    std::optional<bool> lidOpen() override { return lid; }
    bool displayOn() override { return display; }
    std::optional<bool> hasBattery() override { return battery; }
    BrightnessReadability panelBrightnessReadable() override { return readability; }
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
    return buildBuiltinDisplay(available, brightness, calibrating, true, false);
}

std::string reasonName(LaptopReason reason)
{
    return laptopReasonString(reason);
}

// Declared in dependency order: the follower is destroyed first.
struct Rig {
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    LaptopPanelFollower follower{panel, env, submitter, scheduler, clock};
};

}  // namespace

TEST(follow_pause_lid_close_pauses_and_submits_unavailable_once)
{
    Rig rig;
    rig.follower.setFollowEnabled(true);
    rig.scheduler.advance(0ms);
    CHECK_EQ(rig.submitter.messages.back(), report(true, 40.0, false));

    rig.env.lid = false;
    rig.follower.onLidChanged(false);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
    CHECK(!rig.follower.canCalibrate());
    CHECK_EQ(reasonName(rig.follower.reason()), std::string("displayOffline"));

    rig.scheduler.advance(10000ms);
    rig.follower.onLidChanged(false);
    CHECK_EQ(rig.panel.reads, 1);
    CHECK_EQ(rig.submitter.messages.size(), std::size_t{2});
    CHECK_EQ(rig.submitter.messages.back(), report(false, std::nullopt, false));
}

TEST(follow_pause_display_off_pauses_and_reports_offline)
{
    Rig rig;
    rig.follower.setFollowEnabled(true);
    rig.scheduler.advance(0ms);

    rig.env.display = false;
    rig.follower.onDisplayStatusChanged(false);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
    CHECK(!rig.follower.canCalibrate());
    CHECK_EQ(reasonName(rig.follower.reason()), std::string("displayOffline"));
    CHECK_EQ(rig.submitter.messages.size(), std::size_t{2});
    CHECK_EQ(rig.submitter.messages.back(), report(false, std::nullopt, false));
}

TEST(follow_pause_reopen_resumes_at_current_level)
{
    Rig rig;
    rig.follower.setFollowEnabled(true);
    rig.scheduler.advance(0ms);

    rig.env.lid = false;
    rig.follower.onLidChanged(false);
    rig.panel.level = 55.0;  // the panel was changed while the lid was closed

    rig.env.lid = true;
    rig.follower.onLidChanged(true);
    // Reopening queues an immediate read on the scheduler, not on this thread.
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});
    CHECK_EQ(rig.panel.reads, 1);
    CHECK_EQ(rig.submitter.messages.size(), std::size_t{2});

    rig.scheduler.advance(0ms);
    CHECK_EQ(rig.panel.reads, 2);
    CHECK_EQ(rig.submitter.messages.size(), std::size_t{3});
    CHECK_EQ(rig.submitter.messages.back(), report(true, 55.0, false));
    CHECK(rig.follower.canCalibrate());
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});
}

TEST(follow_pause_can_calibrate_is_false_while_paused)
{
    Rig rig;
    rig.follower.setFollowEnabled(true);
    rig.scheduler.advance(0ms);
    CHECK(rig.follower.canCalibrate());

    rig.env.lid = false;
    rig.follower.onLidChanged(false);
    CHECK(!rig.follower.canCalibrate());

    rig.env.lid = true;
    rig.follower.onLidChanged(true);
    CHECK(rig.follower.canCalibrate());

    rig.follower.setFollowEnabled(false);
    CHECK(!rig.follower.canCalibrate());
}

TEST(follow_pause_resume_while_follow_disabled_does_not_poll)
{
    Rig rig;
    rig.follower.setFollowEnabled(false);

    rig.env.lid = false;
    rig.follower.onLidChanged(false);
    rig.env.lid = true;
    rig.follower.onLidChanged(true);

    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
    rig.scheduler.advance(10000ms);
    CHECK_EQ(rig.panel.reads, 0);
    CHECK_EQ(rig.submitter.messages.size(), std::size_t{3});
    CHECK_EQ(rig.submitter.messages.back(), report(true, std::nullopt, false));
    CHECK(!rig.follower.canCalibrate());
}

TEST(follow_pause_calibration_stops_polling_and_reports_flag)
{
    Rig rig;
    rig.follower.setFollowEnabled(true);
    rig.scheduler.advance(0ms);

    rig.follower.onCalibrationChanged(true);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(rig.submitter.messages.back(), report(true, 40.0, true));

    rig.scheduler.advance(5000ms);
    CHECK_EQ(rig.panel.reads, 1);

    rig.follower.onCalibrationChanged(false);
    rig.scheduler.advance(0ms);
    CHECK_EQ(rig.panel.reads, 2);
    CHECK_EQ(rig.submitter.messages.back(), report(true, 40.0, false));
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});
}

TEST(follow_pause_destruction_cancels_poll)
{
    FakePanel panel;
    FakeEnvironment env;
    RecordingSubmitter submitter;
    FakeScheduler scheduler;
    FakeClock clock;
    {
        LaptopPanelFollower follower(panel, env, submitter, scheduler, clock);
        follower.setFollowEnabled(true);
        scheduler.advance(0ms);
        CHECK_EQ(scheduler.pendingCount(), std::size_t{1});
    }
    CHECK_EQ(scheduler.pendingCount(), std::size_t{0});
    scheduler.advance(10000ms);
    CHECK_EQ(panel.reads, 1);
}
