#include "TestHarness.h"
#include "Fakes.h"

#include "../Platform/AppPaths.h"
#include "../Platform/AutoBrightnessOwner.h"
#include "../Platform/BuiltInProtection.h"
#include "../Platform/ILaptopPanel.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using telaviva::AdaptiveBrightness;
using telaviva::AppPaths;
using telaviva::AutoBrightnessOwner;
using telaviva::BuiltInNotice;
using telaviva::BuiltInProtection;
using telaviva::BuiltinProtectionSnapshot;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::ILaptopPanel;
using telaviva::IPowerSettings;
using telaviva::PowerSource;
using telaviva::PowerStatus;
using telaviva::builtinCardVisible;

namespace {

const std::string kScheme = "381b4222-f694-41f0-9685-ff5bb260df2e";
const std::chrono::milliseconds kSecond{1000};

// In-memory power scheme with adaptive brightness on for both sources, as on the owner test.
class FakePower final : public IPowerSettings {
public:
    struct Values {
        uint32_t ac = 0;
        uint32_t dc = 0;
    };

    PowerStatus activeScheme(std::string& schemeGuid) override
    {
        schemeGuid = active;
        return PowerStatus::Ok;
    }

    PowerStatus readAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t& value) override
    {
        const auto it = schemes.find(schemeGuid);
        if (it == schemes.end()) {
            return PowerStatus::NotFound;
        }
        value = source == PowerSource::Ac ? it->second.ac : it->second.dc;
        return PowerStatus::Ok;
    }

    PowerStatus writeAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t value) override
    {
        const auto it = schemes.find(schemeGuid);
        if (it == schemes.end()) {
            return PowerStatus::NotFound;
        }
        if (failAdaptiveWrites) {
            return PowerStatus::Failed;
        }
        if (source == PowerSource::Ac) {
            it->second.ac = value;
        } else {
            it->second.dc = value;
        }
        return PowerStatus::Ok;
    }

    PowerStatus setActiveScheme(const std::string& schemeGuid) override
    {
        if (schemes.find(schemeGuid) == schemes.end()) {
            return PowerStatus::NotFound;
        }
        active = schemeGuid;
        return PowerStatus::Ok;
    }

    std::map<std::string, Values> schemes;
    std::string active;
    // makes every adaptive write fail, so turning automatic brightness off is refused.
    bool failAdaptiveWrites = false;
};

// Built-in panel with a supported-level list. A write snaps to the nearest level, as WmiPanel does, and
// failWrites makes a write return false without changing the level. Every write is logged.
class FakePanel final : public ILaptopPanel {
public:
    std::optional<double> level;
    std::vector<int> levels;
    bool readFails = false;
    bool failWrites = false;
    int reads = 0;
    std::vector<double> writes;

    std::optional<double> readPercent() override
    {
        ++reads;
        if (readFails) {
            return std::nullopt;
        }
        return level;
    }

    bool setPercent(double percent) override
    {
        writes.push_back(percent);
        if (failWrites) {
            return false;
        }
        level = snapped(percent);
        return true;
    }

    std::vector<int> supportedLevels() const override { return levels; }

private:
    double snapped(double percent) const
    {
        if (levels.empty()) {
            return percent;
        }
        int nearest = levels.front();
        for (const int candidate : levels) {
            if (std::fabs(candidate - percent) < std::fabs(nearest - percent)) {
                nearest = candidate;
            }
        }
        return static_cast<double>(nearest);
    }
};

std::vector<int> wholeLevels()
{
    std::vector<int> levels;
    for (int level = 0; level <= 100; ++level) {
        levels.push_back(level);
    }
    return levels;
}

BuiltinProtectionSnapshot decision(std::vector<std::string> reasons, std::optional<double> target,
                                   bool holdAuto = false, bool available = true)
{
    BuiltinProtectionSnapshot snapshot;
    snapshot.protectable = true;
    snapshot.protected_ = true;
    snapshot.available = available;
    snapshot.reasons = std::move(reasons);
    snapshot.targetPercent = target;
    snapshot.holdAutoBrightness = holdAuto;
    return snapshot;
}

// A presence dim to the target.
BuiltinProtectionSnapshot dimming(double target, bool holdAuto = false, bool available = true)
{
    return decision({"presence"}, target, holdAuto, available);
}

// The decision when nothing dims the built-in screen.
BuiltinProtectionSnapshot idle(bool available = true)
{
    return decision({}, std::nullopt, false, available);
}

fs::path makeTempDirectory()
{
    static int counter = 0;
    const fs::path directory =
        fs::temp_directory_path() / ("telaviva-test-builtin-" + std::to_string(counter++) + "-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    static_cast<void>(fs::create_directories(directory, ec));
    return directory;
}

void writeText(const fs::path& file, const std::string& text)
{
    std::error_code ec;
    static_cast<void>(fs::create_directories(file.parent_path(), ec));
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// One temp directory, a real owner over a fake power scheme, a fake panel, a fake scheduler and clock, and the
// holding reports in order.
class Fixture {
public:
    explicit Fixture(std::optional<double> level)
        : directory(makeTempDirectory()),
          adaptive(power),
          owner(adaptive, AppPaths(directory).autoBrightnessHoldFile(), nullptr)
    {
        power.schemes[kScheme] = FakePower::Values{1, 1};
        power.active = kScheme;
        panel.level = level;
        panel.levels = wholeLevels();
    }

    ~Fixture()
    {
        controller.reset();
        std::error_code ec;
        static_cast<void>(fs::remove_all(directory, ec));
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    // Creates the controller with the default record path, or with the given one.
    void createController(const std::optional<fs::path>& file = std::nullopt)
    {
        const fs::path record = file ? *file : recordFile();
        controller = std::make_unique<BuiltInProtection>(
            panel, owner, scheduler, clock, record, nullptr,
            [this](bool holding) {
                holdingReports.push_back(holding);
                recordExisted.push_back(fs::exists(recordFile()));
            },
            [this] { ++noticeReports; });
    }

    fs::path recordFile() const { return AppPaths(directory).builtinProtectionSessionFile(); }
    fs::path ownerFile() const { return AppPaths(directory).autoBrightnessHoldFile(); }

    fs::path directory;
    FakePower power;
    AdaptiveBrightness adaptive;
    AutoBrightnessOwner owner;
    FakePanel panel;
    FakeScheduler scheduler;
    FakeClock clock;
    std::vector<bool> holdingReports;
    // Whether the record existed at each holding report, in order.
    std::vector<bool> recordExisted;
    // How many times the notice changed.
    int noticeReports = 0;
    // Declared last, so it is destroyed first.
    std::unique_ptr<BuiltInProtection> controller;
};

int writeCount(const Fixture& fx)
{
    return static_cast<int>(fx.panel.writes.size());
}

int pendingCount(const Fixture& fx)
{
    return static_cast<int>(fx.scheduler.pendingCount());
}

}  // namespace

TEST(builtin_protection_dim_saves_the_record_then_reports_holding_then_writes)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    const std::vector<bool> holding = {true};
    const std::vector<bool> existed = {true};
    const std::vector<double> writes = {30.0};
    CHECK(fx.holdingReports == holding);
    CHECK(fx.recordExisted == existed);
    CHECK(fx.panel.writes == writes);
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.controller->isHolding());
    const std::string text = readText(fx.recordFile());
    CHECK(text.find("\"version\":1") != std::string::npos);
    CHECK(text.find("\"baseline_percent\":60") != std::string::npos);
}

TEST(builtin_protection_dim_then_restore_writes_the_baseline_and_reports_idle)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.controller->reconcile(idle());
    const std::vector<double> writes = {30.0, 60.0};
    const std::vector<bool> holding = {true, false};
    const std::vector<bool> existed = {true, false};
    CHECK(fx.panel.writes == writes);
    CHECK(fx.panel.level == 60.0);
    CHECK(fx.holdingReports == holding);
    CHECK(fx.recordExisted == existed);
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(!fx.controller->isHolding());
}

TEST(builtin_protection_target_above_the_baseline_writes_nothing_but_still_holds)
{
    Fixture fx(40.0);
    fx.createController();
    fx.controller->reconcile(dimming(70.0));
    const std::vector<bool> holding = {true};
    CHECK(fx.panel.writes.empty());
    CHECK(fx.panel.level == 40.0);
    CHECK(fx.holdingReports == holding);
    CHECK(fx.controller->isHolding());
}

TEST(builtin_protection_presence_and_screensaver_reasons_dim_to_the_target)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(decision({"screensaver"}, 25.0));
    CHECK(fx.panel.level == 25.0);
    // A second reason set at the same target only reads the level, which already matches.
    fx.controller->reconcile(decision({"presence", "screensaver"}, 25.0));
    const std::vector<double> writes = {25.0};
    CHECK(fx.panel.writes == writes);
}

TEST(builtin_protection_unavailable_display_gets_no_writes_and_keeps_holding)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(decision({"presence"}, 30.0, false, false));
    CHECK(fx.panel.writes.empty());
    CHECK(fx.holdingReports.empty());
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(!fx.controller->isHolding());

    fx.controller->reconcile(dimming(30.0));
    const std::vector<double> dimWrites = {30.0};
    CHECK(fx.panel.writes == dimWrites);

    fx.controller->reconcile(decision({"presence"}, 30.0, false, false));
    CHECK(fx.panel.writes == dimWrites);
    fx.controller->reconcile(idle(false));
    CHECK(fx.panel.writes == dimWrites);
    CHECK(fx.controller->isHolding());
    CHECK(fx.panel.level == 30.0);
}

TEST(builtin_protection_unreadable_baseline_skips_the_dim)
{
    Fixture fx(60.0);
    fx.panel.readFails = true;
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    CHECK(fx.panel.writes.empty());
    CHECK(fx.holdingReports.empty());
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(!fx.controller->isHolding());
}

TEST(builtin_protection_unwritable_record_skips_the_dim)
{
    Fixture fx(60.0);
    // A file where the record's directory should be, so the record cannot be created.
    const fs::path blocker = fx.directory / "blocker";
    writeText(blocker, "not a directory");
    fx.createController(blocker / "builtin-protection-session.json");
    fx.controller->reconcile(dimming(30.0));
    CHECK(fx.panel.writes.empty());
    CHECK(fx.holdingReports.empty());
    CHECK(!fx.controller->isHolding());
}

TEST(builtin_protection_failed_write_retries_then_parks_at_three_attempts)
{
    Fixture fx(80.0);
    fx.createController();
    fx.panel.failWrites = true;
    fx.controller->reconcile(dimming(30.0));
    CHECK(writeCount(fx) == 1);
    CHECK(pendingCount(fx) == 1);
    fx.scheduler.advance(kSecond);
    CHECK(writeCount(fx) == 2);
    CHECK(pendingCount(fx) == 1);
    fx.scheduler.advance(kSecond);
    CHECK(writeCount(fx) == 3);
    CHECK(pendingCount(fx) == 0);
    fx.scheduler.advance(kSecond);
    CHECK(writeCount(fx) == 3);

    // The parked dim stays quiet while the decision repeats.
    fx.controller->reconcile(dimming(30.0));
    CHECK(writeCount(fx) == 3);
    CHECK(fx.controller->isHolding());

    // A changed target starts a fresh episode with its own attempts.
    fx.controller->reconcile(dimming(20.0));
    CHECK(writeCount(fx) == 4);
    CHECK(pendingCount(fx) == 1);
    CHECK(fx.panel.writes.back() == 20.0);
}

TEST(builtin_protection_leftover_record_holds_the_stored_baseline_without_rereading_it)
{
    // The crash left the screen dimmed at 10; the stored baseline is 60.
    Fixture fx(10.0);
    writeText(fx.recordFile(), "{\"version\":1,\"started_at\":\"2026-10-23T11:00:00Z\",\"baseline_percent\":60}");
    fx.createController();
    fx.controller->recoverIfNeeded();
    const std::vector<bool> holding = {true};
    CHECK(fx.controller->isHolding());
    CHECK(fx.holdingReports == holding);
    CHECK(fx.panel.reads == 0);
    CHECK(fx.panel.writes.empty());

    // The first snapshot without reasons restores the stored baseline; the only read is the readback.
    fx.controller->reconcile(idle());
    const std::vector<double> writes = {60.0};
    const std::vector<bool> both = {true, false};
    CHECK(fx.panel.writes == writes);
    CHECK(fx.panel.level == 60.0);
    CHECK(fx.holdingReports == both);
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(fx.panel.reads == 1);
    CHECK(!fx.controller->isHolding());
}

TEST(builtin_protection_corrupt_record_is_deleted_at_launch)
{
    Fixture fx(10.0);
    writeText(fx.recordFile(), "not a record");
    fx.createController();
    fx.controller->recoverIfNeeded();
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(!fx.controller->isHolding());
    CHECK(fx.holdingReports.empty());
    CHECK(fx.panel.reads == 0);
    CHECK(fx.panel.writes.empty());
}

TEST(builtin_protection_coarse_panel_accepts_the_nearest_supported_level)
{
    Fixture fx(50.0);
    fx.panel.levels = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    fx.createController();
    // 25 is between 20 and 30; the panel and the verify step both pick the lower one, 20.
    fx.controller->reconcile(dimming(25.0));
    CHECK(fx.panel.level == 20.0);
    CHECK(writeCount(fx) == 1);
    CHECK(pendingCount(fx) == 0);
}

TEST(builtin_protection_target_one_snaps_to_zero_and_is_accepted)
{
    Fixture fx(50.0);
    fx.panel.levels = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    fx.createController();
    fx.controller->reconcile(dimming(1.0));
    CHECK(fx.panel.level == 0.0);
    CHECK(writeCount(fx) == 1);
    CHECK(pendingCount(fx) == 0);
}

TEST(builtin_protection_zero_percent_means_the_lowest_supported_level)
{
    // The lowest level here is 10, so a write of 0 lands on 10 and must still verify.
    Fixture fx(50.0);
    fx.panel.levels = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    fx.createController();
    fx.controller->reconcile(dimming(0.0));
    CHECK(fx.panel.level == 10.0);
    CHECK(writeCount(fx) == 1);
    CHECK(pendingCount(fx) == 0);
    CHECK(fx.controller->isHolding());
}

TEST(builtin_protection_quit_restore_runs_once_and_later_snapshots_do_not_dim)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.panel.writes.clear();
    fx.holdingReports.clear();

    fx.controller->restoreNow();
    const std::vector<double> writes = {60.0};
    const std::vector<bool> holding = {false};
    CHECK(fx.panel.writes == writes);
    CHECK(fx.panel.level == 60.0);
    CHECK(fx.holdingReports == holding);
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(!fx.controller->isHolding());

    fx.controller->restoreNow();
    CHECK(writeCount(fx) == 1);

    fx.controller->reconcile(dimming(30.0));
    CHECK(writeCount(fx) == 1);
    CHECK(static_cast<int>(fx.holdingReports.size()) == 1);
    CHECK(fx.panel.level == 60.0);
    CHECK(!fx.controller->isHolding());
}

TEST(builtin_protection_quit_failed_write_keeps_the_record)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.panel.writes.clear();
    fx.panel.failWrites = true;

    fx.controller->restoreNow();
    CHECK(writeCount(fx) == 1);
    CHECK(fx.controller->isHolding());
    CHECK(fs::exists(fx.recordFile()));
    CHECK(pendingCount(fx) == 0);
}

TEST(builtin_protection_dim_is_trusted_for_five_seconds_while_the_decision_repeats)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    const int readsAfterDim = fx.panel.reads;

    fx.controller->reconcile(dimming(30.0));
    CHECK(fx.panel.reads == readsAfterDim);

    fx.clock.advance(4.0);
    fx.controller->reconcile(dimming(30.0));
    CHECK(fx.panel.reads == readsAfterDim);

    // The OS brightens the panel. Six seconds after the verification the window has expired.
    fx.panel.level = 80.0;
    fx.clock.advance(2.0);
    fx.controller->reconcile(dimming(30.0));
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.panel.reads == readsAfterDim + 2);
}

TEST(builtin_protection_restore_is_not_throttled_by_the_verification_window)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.panel.writes.clear();
    fx.controller->reconcile(idle());
    const std::vector<double> writes = {60.0};
    CHECK(fx.panel.writes == writes);
    CHECK(!fx.controller->isHolding());
}

TEST(builtin_protection_changed_target_verifies_at_once_inside_the_window)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    const int readsBefore = fx.panel.reads;
    fx.controller->reconcile(dimming(20.0));
    // A fresh read and the readback of the write.
    CHECK(fx.panel.reads == readsBefore + 2);
    CHECK(fx.panel.level == 20.0);
}

TEST(builtin_protection_restore_failure_parks_at_three_then_a_later_snapshot_tries_again)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.panel.writes.clear();
    fx.panel.failWrites = true;

    fx.controller->reconcile(idle());
    CHECK(writeCount(fx) == 1);
    CHECK(pendingCount(fx) == 1);
    fx.scheduler.advance(kSecond);
    CHECK(writeCount(fx) == 2);
    CHECK(pendingCount(fx) == 1);
    fx.scheduler.advance(kSecond);
    CHECK(writeCount(fx) == 3);
    CHECK(pendingCount(fx) == 0);
    CHECK(fx.controller->isHolding());
    CHECK(fs::exists(fx.recordFile()));

    // Restores are not throttled: the next snapshot writes again and succeeds.
    fx.panel.failWrites = false;
    fx.controller->reconcile(idle());
    CHECK(writeCount(fx) == 4);
    CHECK(!fx.controller->isHolding());
    CHECK(!fs::exists(fx.recordFile()));
    CHECK(pendingCount(fx) == 0);
}

TEST(builtin_protection_visibility_shows_only_protectable_and_available)
{
    BuiltinProtectionSnapshot snapshot;
    snapshot.protectable = false;
    snapshot.available = false;
    CHECK(!builtinCardVisible(snapshot));
    snapshot.available = true;
    CHECK(!builtinCardVisible(snapshot));
    snapshot.protectable = true;
    snapshot.available = false;
    CHECK(!builtinCardVisible(snapshot));
    snapshot.available = true;
    CHECK(builtinCardVisible(snapshot));
    // Disabling protection or holding does not hide a protectable, available screen.
    snapshot.protected_ = false;
    snapshot.holding = true;
    CHECK(builtinCardVisible(snapshot));
}

TEST(builtin_protection_automatic_brightness_is_held_while_dimmed_and_restored_after)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0, true));
    CHECK(fx.power.schemes[kScheme].ac == 0);
    CHECK(fx.power.schemes[kScheme].dc == 0);
    CHECK(fs::exists(fx.ownerFile()));
    CHECK(fx.owner.anyHeld());

    fx.controller->reconcile(idle());
    CHECK(fx.power.schemes[kScheme].ac == 1);
    CHECK(fx.power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(fx.ownerFile()));
    CHECK(!fx.owner.anyHeld());
}

TEST(builtin_protection_automatic_brightness_is_untouched_when_the_option_is_off)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0, false));
    CHECK(fx.controller->isHolding());
    CHECK(fx.power.schemes[kScheme].ac == 1);
    CHECK(fx.power.schemes[kScheme].dc == 1);
    CHECK(!fx.owner.anyHeld());
    CHECK(!fs::exists(fx.ownerFile()));
}

TEST(builtin_protection_notice_is_none_while_nothing_dims_the_screen)
{
    Fixture fx(60.0);
    fx.createController();
    CHECK(fx.controller->notice() == BuiltInNotice::none);
    fx.controller->reconcile(idle());
    CHECK(fx.controller->notice() == BuiltInNotice::none);
    CHECK(fx.noticeReports == 0);
}

TEST(builtin_protection_notice_auto_brightness_unknown_when_the_setting_cannot_be_read)
{
    Fixture fx(60.0);
    // An unreadable scheme makes the owner's first acquire unknown.
    fx.power.active = "not-a-guid";
    fx.createController();
    fx.controller->reconcile(dimming(30.0, true));
    // The dim still happens.
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.controller->notice() == BuiltInNotice::autoBrightnessUnknown);

    // The note clears when holding ends. reconcile does not report notices: its caller builds the flyout.
    fx.power.active = kScheme;
    fx.controller->reconcile(idle());
    CHECK(fx.panel.level == 60.0);
    CHECK(fx.controller->notice() == BuiltInNotice::none);
    CHECK(fx.noticeReports == 0);
}

TEST(builtin_protection_notice_auto_brightness_not_changed_when_the_setting_cannot_be_turned_off)
{
    Fixture fx(60.0);
    fx.power.failAdaptiveWrites = true;
    fx.createController();
    fx.controller->reconcile(dimming(30.0, true));
    // The dim still happens; only the automatic brightness change failed.
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.controller->notice() == BuiltInNotice::autoBrightnessNotChanged);
    CHECK(fx.power.schemes[kScheme].ac == 1);

    // The note clears when holding ends. reconcile does not report notices: its caller builds the flyout.
    fx.power.failAdaptiveWrites = false;
    fx.controller->reconcile(idle());
    CHECK(fx.panel.level == 60.0);
    CHECK(fx.controller->notice() == BuiltInNotice::none);
    CHECK(fx.noticeReports == 0);
}

TEST(builtin_protection_notice_auto_brightness_not_changed_is_not_shown_when_the_option_is_off)
{
    Fixture fx(60.0);
    fx.power.failAdaptiveWrites = true;
    fx.createController();
    fx.controller->reconcile(dimming(30.0, false));
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.controller->notice() == BuiltInNotice::none);
}

TEST(builtin_protection_notice_is_not_shown_when_the_setting_is_already_off)
{
    Fixture fx(60.0);
    fx.power.schemes[kScheme] = FakePower::Values{0, 0};
    fx.createController();
    fx.controller->reconcile(dimming(30.0, true));
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.controller->notice() == BuiltInNotice::none);
}

TEST(builtin_protection_notice_is_not_shown_on_a_desktop_without_adaptive_brightness)
{
    Fixture fx(60.0);
    fx.power.schemes.clear();
    fx.createController();
    fx.controller->reconcile(dimming(30.0, true));
    CHECK(fx.panel.level == 30.0);
    CHECK(fx.controller->notice() == BuiltInNotice::none);
}

TEST(builtin_protection_notice_is_not_shown_when_the_option_is_off)
{
    Fixture fx(60.0);
    fx.power.active = "not-a-guid";
    fx.createController();
    fx.controller->reconcile(dimming(30.0, false));
    CHECK(fx.controller->notice() == BuiltInNotice::none);
}

TEST(builtin_protection_notice_restore_failed_after_three_attempts_then_clears_on_success)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.panel.writes.clear();
    fx.panel.failWrites = true;
    fx.controller->reconcile(idle());
    CHECK(fx.controller->notice() == BuiltInNotice::none);
    fx.scheduler.advance(kSecond);
    fx.scheduler.advance(kSecond);
    CHECK(writeCount(fx) == 3);
    // The retry timer parked the restore, so the change is reported once.
    CHECK(fx.controller->notice() == BuiltInNotice::restoreFailed);
    CHECK(fx.noticeReports == 1);

    fx.panel.failWrites = false;
    fx.controller->reconcile(idle());
    CHECK(fx.controller->notice() == BuiltInNotice::none);
    CHECK(!fx.controller->isHolding());
    CHECK(fx.noticeReports == 1);
}

TEST(builtin_protection_notice_restore_failed_while_a_quit_restore_is_pending)
{
    Fixture fx(60.0);
    fx.createController();
    fx.controller->reconcile(dimming(30.0));
    fx.panel.failWrites = true;
    fx.controller->restoreNow();
    CHECK(fx.controller->isHolding());
    CHECK(fs::exists(fx.recordFile()));
    CHECK(fx.controller->notice() == BuiltInNotice::restoreFailed);
}

TEST(builtin_protection_notice_restore_failed_takes_priority_over_auto_brightness_unknown)
{
    Fixture fx(60.0);
    fx.power.active = "not-a-guid";
    fx.createController();
    fx.controller->reconcile(dimming(30.0, true));
    CHECK(fx.controller->notice() == BuiltInNotice::autoBrightnessUnknown);

    fx.panel.failWrites = true;
    fx.controller->reconcile(idle());
    fx.scheduler.advance(kSecond);
    fx.scheduler.advance(kSecond);
    CHECK(fx.controller->notice() == BuiltInNotice::restoreFailed);
    CHECK(fx.controller->isHolding());
}
