#include "TestHarness.h"

#include "../Platform/LaptopDetection.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>

namespace {

using telaviva::BrightnessReadability;
using telaviva::DisplayPathInfo;
using telaviva::LaptopAvailability;
using telaviva::LaptopReason;

// Defaults to a healthy, open, powered laptop with a readable panel.
struct FakeLaptopEnvironment final : telaviva::ILaptopEnvironment {
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

// Desktop: no internal target and no battery.
FakeLaptopEnvironment makeDesktop()
{
    FakeLaptopEnvironment env;
    env.path.reset();
    env.battery = false;
    return env;
}

}  // namespace

TEST(laptop_desktop_no_internal_target)
{
    FakeLaptopEnvironment env = makeDesktop();
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::NoBuiltInDisplay);
}

TEST(laptop_no_internal_target_with_battery)
{
    FakeLaptopEnvironment env;
    env.path.reset();
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::NoBuiltInDisplay);
}

TEST(laptop_internal_target_on_desktop_without_battery)
{
    // An embedded target on a device with no battery still counts as a desktop.
    FakeLaptopEnvironment env = makeDesktop();
    env.path = DisplayPathInfo{true, "Embedded"};
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::NoBuiltInDisplay);
}

TEST(laptop_unknown_battery_is_not_desktop)
{
    FakeLaptopEnvironment env;
    env.battery.reset();
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.available);
    CHECK(result.reason == LaptopReason::None);
}

TEST(laptop_internal_target_inactive_is_display_offline)
{
    FakeLaptopEnvironment env;
    env.path = DisplayPathInfo{false, "Built-in"};
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::DisplayOffline);
}

TEST(laptop_lid_closed_is_display_offline)
{
    FakeLaptopEnvironment env;
    env.lid = false;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::DisplayOffline);
}

TEST(laptop_display_off_is_display_offline)
{
    FakeLaptopEnvironment env;
    env.display = false;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::DisplayOffline);
}

TEST(laptop_unreported_lid_is_treated_as_open)
{
    FakeLaptopEnvironment env;
    env.lid.reset();
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.available);
    CHECK(result.reason == LaptopReason::None);
}

TEST(laptop_wmi_missing_is_api_unavailable)
{
    FakeLaptopEnvironment env;
    env.readability = BrightnessReadability::ApiMissing;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::ApiUnavailable);
}

TEST(laptop_not_readable_is_brightness_not_adjustable)
{
    FakeLaptopEnvironment env;
    env.readability = BrightnessReadability::NotReadable;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(!result.available);
    CHECK(result.reason == LaptopReason::BrightnessNotAdjustable);
}

TEST(laptop_healthy_panel_is_available)
{
    FakeLaptopEnvironment env;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.available);
    CHECK(result.reason == LaptopReason::None);
}

TEST(laptop_precedence_desktop_over_api_missing)
{
    FakeLaptopEnvironment env = makeDesktop();
    env.readability = BrightnessReadability::ApiMissing;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.reason == LaptopReason::NoBuiltInDisplay);
}

TEST(laptop_precedence_lid_closed_over_not_readable)
{
    FakeLaptopEnvironment env;
    env.lid = false;
    env.readability = BrightnessReadability::NotReadable;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.reason == LaptopReason::DisplayOffline);
}

TEST(laptop_precedence_inactive_over_api_missing)
{
    FakeLaptopEnvironment env;
    env.path = DisplayPathInfo{false, "Built-in"};
    env.readability = BrightnessReadability::ApiMissing;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.reason == LaptopReason::DisplayOffline);
}

TEST(laptop_precedence_api_missing_over_not_readable)
{
    FakeLaptopEnvironment env;
    env.readability = BrightnessReadability::ApiMissing;
    const LaptopAvailability result = telaviva::detectLaptop(env);
    CHECK(result.reason == LaptopReason::ApiUnavailable);
}

TEST(laptop_reason_strings)
{
    CHECK_EQ(std::string(telaviva::laptopReasonString(LaptopReason::None)), std::string("none"));
    CHECK_EQ(std::string(telaviva::laptopReasonString(LaptopReason::NoBuiltInDisplay)), std::string("noBuiltInDisplay"));
    CHECK_EQ(std::string(telaviva::laptopReasonString(LaptopReason::DisplayOffline)), std::string("displayOffline"));
    CHECK_EQ(std::string(telaviva::laptopReasonString(LaptopReason::BrightnessNotAdjustable)),
             std::string("brightnessNotAdjustable"));
    CHECK_EQ(std::string(telaviva::laptopReasonString(LaptopReason::ApiUnavailable)), std::string("apiUnavailable"));
    CHECK_EQ(std::string(telaviva::laptopReasonString(LaptopReason::Unknown)), std::string("unknown"));
}

TEST(laptop_win32_probe_defaults_to_api_missing)
{
    telaviva::Win32LaptopEnvironment env;
    CHECK(env.panelBrightnessReadable() == BrightnessReadability::ApiMissing);
}

TEST(laptop_win32_probe_is_forwarded)
{
    telaviva::Win32LaptopEnvironment env;
    env.panelBrightnessProbe = [] { return BrightnessReadability::NotReadable; };
    CHECK(env.panelBrightnessReadable() == BrightnessReadability::NotReadable);
}

TEST(laptop_win32_recorded_states)
{
    telaviva::Win32LaptopEnvironment env;
    CHECK(env.displayOn());
    CHECK(!env.lidOpen().has_value());
    env.recordLidState(false);
    CHECK(env.lidOpen().has_value());
    CHECK(env.lidOpen().has_value() && !*env.lidOpen());
    env.recordDisplayStatus(false);
    CHECK(!env.displayOn());
}

TEST(laptop_win32_concurrent_record_and_read)
{
    // Reads race with UI-thread writes; the test is mainly for TSAN. The writer
    // ends on closed/off, so the join must observe that final state.
    telaviva::Win32LaptopEnvironment env;
    std::atomic<bool> stop{false};
    std::thread writer([&env, &stop] {
        bool value = false;
        while (!stop.load()) {
            env.recordLidState(value);
            env.recordDisplayStatus(value);
            value = !value;
        }
        env.recordLidState(false);
        env.recordDisplayStatus(false);
    });

    std::size_t reads = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(env.lidOpen());
        static_cast<void>(env.displayOn());
        ++reads;
    }

    stop.store(true);
    writer.join();
    CHECK(reads > 0);
    CHECK(env.lidOpen().has_value() && !*env.lidOpen());
    CHECK(!env.displayOn());
}
