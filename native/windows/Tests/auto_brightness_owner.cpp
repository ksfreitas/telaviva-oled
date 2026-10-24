#include "TestHarness.h"

#include "../Platform/AppPaths.h"
#include "../Platform/AutoBrightnessOwner.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using telaviva::AdaptiveBrightness;
using telaviva::AutoBrightnessHolder;
using telaviva::AutoBrightnessOutcome;
using telaviva::AutoBrightnessOwner;
using telaviva::AppPaths;
using telaviva::IPowerSettings;
using telaviva::PowerSource;
using telaviva::PowerStatus;
using telaviva::SavedAdaptiveState;

namespace {

const std::string kScheme = "381b4222-f694-41f0-9685-ff5bb260df2e";
const std::string kOtherScheme = "a1841308-3541-4fab-bc81-f71556f20b4a";

// In-memory power schemes. Failure switches inject the errors the owner must survive.
class FakePower final : public IPowerSettings {
public:
    struct Scheme {
        bool hasSetting = true;
        uint32_t ac = 0;
        uint32_t dc = 0;
    };

    PowerStatus activeScheme(std::string& schemeGuid) override
    {
        if (failActive) {
            return PowerStatus::Failed;
        }
        schemeGuid = active;
        return PowerStatus::Ok;
    }

    PowerStatus readAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t& value) override
    {
        const auto it = schemes.find(schemeGuid);
        if (it == schemes.end() || !it->second.hasSetting) {
            return PowerStatus::NotFound;
        }
        value = source == PowerSource::Ac ? it->second.ac : it->second.dc;
        return PowerStatus::Ok;
    }

    PowerStatus writeAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t value) override
    {
        ++writes;
        const auto it = schemes.find(schemeGuid);
        if (it == schemes.end() || !it->second.hasSetting) {
            return PowerStatus::NotFound;
        }
        if (failWrites) {
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
        if (failSetActive) {
            return PowerStatus::Failed;
        }
        if (schemes.find(schemeGuid) == schemes.end()) {
            return PowerStatus::NotFound;
        }
        active = schemeGuid;
        return PowerStatus::Ok;
    }

    std::map<std::string, Scheme> schemes;
    std::string active;
    bool failActive = false;
    bool failWrites = false;
    bool failSetActive = false;
    int writes = 0;
};

// A laptop with adaptive brightness on for both sources, on the balanced scheme.
FakePower makeLaptop()
{
    FakePower power;
    power.schemes[kScheme] = FakePower::Scheme{true, 1, 1};
    power.schemes[kOtherScheme] = FakePower::Scheme{true, 1, 0};
    power.active = kScheme;
    return power;
}

// A temp folder for the hold record, removed when the test ends.
class TempDir {
public:
    TempDir()
        : path_(fs::temp_directory_path() /
                ("telaviva-test-owner-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
    }

    ~TempDir()
    {
        std::error_code ec;
        static_cast<void>(fs::remove_all(path_, ec));
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    fs::path recordFile() const { return AppPaths(path_).autoBrightnessHoldFile(); }

private:
    fs::path path_;
};

void writeText(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST(auto_brightness_two_holders_turn_off_once_and_restore_on_last_release)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::turnedOff);
    const int writesAfterFirst = power.writes;
    CHECK(power.schemes[kScheme].ac == 0);
    CHECK(power.schemes[kScheme].dc == 0);
    CHECK(fs::exists(dir.recordFile()));

    // The second holder joins the hold without reading or writing the setting.
    CHECK(owner.acquire(AutoBrightnessHolder::protection) == AutoBrightnessOutcome::turnedOff);
    CHECK(power.writes == writesAfterFirst);
    CHECK(owner.isHeld(AutoBrightnessHolder::calibration));
    CHECK(owner.isHeld(AutoBrightnessHolder::protection));

    CHECK(owner.release(AutoBrightnessHolder::calibration));
    CHECK(owner.anyHeld());
    CHECK(power.schemes[kScheme].ac == 0);
    CHECK(fs::exists(dir.recordFile()));

    CHECK(owner.release(AutoBrightnessHolder::protection));
    CHECK(!owner.anyHeld());
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_release_of_a_holder_that_never_acquired_changes_nothing)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.release(AutoBrightnessHolder::protection));
    CHECK(power.writes == 0);
    CHECK(!owner.anyHeld());
}

TEST(auto_brightness_restore_is_skipped_when_the_user_turned_the_setting_on)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::protection) == AutoBrightnessOutcome::turnedOff);
    // The user turns the setting on with their own values while the hold is active.
    power.schemes[kScheme].ac = 5;
    power.schemes[kScheme].dc = 3;
    const int writesBeforeRelease = power.writes;

    CHECK(owner.release(AutoBrightnessHolder::protection));
    CHECK(power.writes == writesBeforeRelease);
    CHECK(power.schemes[kScheme].ac == 5);
    CHECK(power.schemes[kScheme].dc == 3);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_release_restores_when_a_fresh_read_finds_the_setting_off)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::turnedOff);
    // The setting still reads off (both values 0) at release, so the restore runs.
    CHECK(owner.release(AutoBrightnessHolder::calibration));
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_user_change_during_the_hold_produces_no_write)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::turnedOff);
    // The user switches to the other scheme, which has the setting on for AC only.
    power.active = kOtherScheme;
    const int writesBeforeRelease = power.writes;

    CHECK(owner.release(AutoBrightnessHolder::calibration));
    // The fresh read is on for the new active scheme, so nothing is written to either scheme.
    CHECK(power.writes == writesBeforeRelease);
    CHECK(power.schemes[kScheme].ac == 0);
    CHECK(power.schemes[kOtherScheme].ac == 1);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_setting_already_off_writes_no_record)
{
    TempDir dir;
    FakePower power = makeLaptop();
    power.schemes[kScheme] = FakePower::Scheme{true, 0, 0};
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::notApplicable);
    CHECK(power.writes == 0);
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(owner.release(AutoBrightnessHolder::calibration));
    CHECK(power.writes == 0);
}

TEST(auto_brightness_desktop_without_the_setting_writes_no_record)
{
    TempDir dir;
    FakePower power = makeLaptop();
    power.schemes[kScheme].hasSetting = false;
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::protection) == AutoBrightnessOutcome::notApplicable);
    CHECK(power.writes == 0);
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(owner.release(AutoBrightnessHolder::protection));
    CHECK(power.writes == 0);
}

TEST(auto_brightness_unknown_read_writes_nothing)
{
    TempDir dir;
    FakePower power = makeLaptop();
    power.failActive = true;
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::unknown);
    CHECK(power.writes == 0);
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(owner.isHeld(AutoBrightnessHolder::calibration));
    CHECK(owner.release(AutoBrightnessHolder::calibration));
    CHECK(power.writes == 0);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_persist_first_runs_after_the_record_and_before_the_change)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    int calls = 0;
    bool recordExisted = false;
    bool settingStillOn = false;
    std::string schemeSeen;
    const AdaptiveBrightness::PersistFirst persist = [&](const SavedAdaptiveState& state) {
        ++calls;
        recordExisted = fs::exists(dir.recordFile());
        settingStillOn = power.schemes[kScheme].ac == 1;
        schemeSeen = state.schemeGuid;
        return true;
    };

    CHECK(owner.acquire(AutoBrightnessHolder::calibration, persist) == AutoBrightnessOutcome::turnedOff);
    CHECK(calls == 1);
    CHECK(recordExisted);
    CHECK(settingStillOn);
    CHECK(schemeSeen == kScheme);
    CHECK(power.schemes[kScheme].ac == 0);
}

TEST(auto_brightness_persist_first_refusal_leaves_the_setting_on)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    const AdaptiveBrightness::PersistFirst refuse = [](const SavedAdaptiveState&) { return false; };

    CHECK(owner.acquire(AutoBrightnessHolder::calibration, refuse) == AutoBrightnessOutcome::notChanged);
    CHECK(power.writes == 0);
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(owner.release(AutoBrightnessHolder::calibration));
    CHECK(power.writes == 0);
}

TEST(auto_brightness_failed_write_is_not_changed_and_owes_no_restore)
{
    TempDir dir;
    FakePower power = makeLaptop();
    power.failWrites = true;
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    // The setting was on, but the write that turns it off failed: the outcome is notChanged, not unknown.
    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::notChanged);
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(owner.isHeld(AutoBrightnessHolder::calibration));
    // Nothing is owed, so the release writes nothing.
    const int writesBeforeRelease = power.writes;
    CHECK(owner.release(AutoBrightnessHolder::calibration));
    CHECK(power.writes == writesBeforeRelease);
    CHECK(!owner.anyHeld());
}

TEST(auto_brightness_failed_restore_keeps_the_record_and_the_next_acquire_joins_it)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);

    CHECK(owner.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::turnedOff);
    power.failWrites = true;
    CHECK(!owner.release(AutoBrightnessHolder::calibration));
    CHECK(!owner.anyHeld());
    CHECK(fs::exists(dir.recordFile()));
    CHECK(power.schemes[kScheme].ac == 0);

    // The setting reads off because the restore failed, so the acquire joins the owed restore without
    // reading it as the user's choice and without writing anything.
    power.failWrites = false;
    const int writesBeforeJoin = power.writes;
    CHECK(owner.acquire(AutoBrightnessHolder::protection) == AutoBrightnessOutcome::turnedOff);
    CHECK(power.writes == writesBeforeJoin);

    CHECK(owner.release(AutoBrightnessHolder::protection));
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_launch_recovery_restores_and_deletes_the_record)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    {
        // The first run turns the setting off and never releases it, as after a crash.
        AutoBrightnessOwner first(adaptive, dir.recordFile(), nullptr);
        CHECK(first.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::turnedOff);
    }
    CHECK(power.schemes[kScheme].ac == 0);
    CHECK(fs::exists(dir.recordFile()));

    AutoBrightnessOwner second(adaptive, dir.recordFile(), nullptr);
    second.recoverIfNeeded();
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(power.schemes[kScheme].dc == 1);
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(!second.anyHeld());
}

TEST(auto_brightness_launch_recovery_skips_the_restore_when_the_setting_is_on)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    {
        AutoBrightnessOwner first(adaptive, dir.recordFile(), nullptr);
        CHECK(first.acquire(AutoBrightnessHolder::protection) == AutoBrightnessOutcome::turnedOff);
    }
    power.schemes[kScheme].ac = 7;
    power.schemes[kScheme].dc = 2;
    const int writesBeforeRecovery = power.writes;

    AutoBrightnessOwner second(adaptive, dir.recordFile(), nullptr);
    second.recoverIfNeeded();
    CHECK(power.writes == writesBeforeRecovery);
    CHECK(power.schemes[kScheme].ac == 7);
    CHECK(power.schemes[kScheme].dc == 2);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_launch_restore_failure_keeps_the_record_and_acquire_joins_it)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    {
        AutoBrightnessOwner first(adaptive, dir.recordFile(), nullptr);
        CHECK(first.acquire(AutoBrightnessHolder::calibration) == AutoBrightnessOutcome::turnedOff);
    }

    AutoBrightnessOwner second(adaptive, dir.recordFile(), nullptr);
    power.failWrites = true;
    second.recoverIfNeeded();
    CHECK(fs::exists(dir.recordFile()));

    power.failWrites = false;
    CHECK(second.acquire(AutoBrightnessHolder::protection) == AutoBrightnessOutcome::turnedOff);
    CHECK(second.release(AutoBrightnessHolder::protection));
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(!fs::exists(dir.recordFile()));
}

TEST(auto_brightness_unparsable_record_is_deleted_at_launch)
{
    TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    writeText(dir.recordFile(), "{ not a hold record");

    AutoBrightnessOwner owner(adaptive, dir.recordFile(), nullptr);
    owner.recoverIfNeeded();
    CHECK(!fs::exists(dir.recordFile()));
    CHECK(power.writes == 0);
    CHECK(power.schemes[kScheme].ac == 1);
    CHECK(!owner.anyHeld());
}
