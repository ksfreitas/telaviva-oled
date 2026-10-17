#include "TestHarness.h"

#include "../Platform/AdaptiveBrightness.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using telaviva::AdaptiveBrightness;
using telaviva::IPowerSettings;
using telaviva::isSchemeGuidText;
using telaviva::PowerSource;
using telaviva::PowerStatus;
using telaviva::SavedAdaptiveState;
using telaviva::SuspendResult;

namespace {

const std::string kScheme = "381b4222-f694-41f0-9685-ff5bb260df2e";
const std::string kOtherScheme = "a1841308-3541-4fab-bc81-f71556f20b4a";

// In-memory power schemes. Failure switches inject the errors the policy must survive.
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
        if (failWrites || (failWriteSource && *failWriteSource == source)) {
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
        applied.push_back(schemeGuid);
        return PowerStatus::Ok;
    }

    std::map<std::string, Scheme> schemes;
    std::string active;
    bool failActive = false;
    bool failWrites = false;
    bool failSetActive = false;
    std::optional<PowerSource> failWriteSource;
    int writes = 0;
    std::vector<std::string> applied;
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

}  // namespace

TEST(adaptive_brightness_suspend_saves_and_turns_off_both_values)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Suspended);
    CHECK_EQ(saved.schemeGuid, kScheme);
    CHECK_EQ(saved.acValue, static_cast<uint32_t>(1));
    CHECK_EQ(saved.dcValue, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(0));
    CHECK_EQ(power.schemes[kScheme].dc, static_cast<uint32_t>(0));
    CHECK_EQ(power.applied.size(), static_cast<size_t>(1));
}

TEST(adaptive_brightness_restore_puts_back_both_values_and_applies)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Suspended);
    CHECK(adaptive.restore(saved));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kScheme].dc, static_cast<uint32_t>(1));
    CHECK_EQ(power.applied.size(), static_cast<size_t>(2));
}

TEST(adaptive_brightness_desktop_without_setting_is_not_applicable)
{
    FakePower power = makeLaptop();
    power.schemes[kScheme].hasSetting = false;
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::NotApplicable);
    CHECK_EQ(power.writes, 0);
    CHECK(power.applied.empty());
}

TEST(adaptive_brightness_already_off_writes_nothing)
{
    FakePower power = makeLaptop();
    power.schemes[kScheme] = FakePower::Scheme{true, 0, 0};
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::WasOff);
    CHECK_EQ(power.writes, 0);
    CHECK(power.applied.empty());
}

TEST(adaptive_brightness_read_failure_writes_nothing)
{
    FakePower power = makeLaptop();
    power.failActive = true;
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Failed);
    CHECK_EQ(power.writes, 0);
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
}

TEST(adaptive_brightness_write_failure_rolls_back_first_value)
{
    FakePower power = makeLaptop();
    power.failWriteSource = PowerSource::Dc;
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Failed);
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kScheme].dc, static_cast<uint32_t>(1));
    CHECK(power.applied.empty());
}

TEST(adaptive_brightness_persist_first_sees_saved_state_before_writes)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    bool calledWithValues = false;
    const SuspendResult result = adaptive.suspend(saved, [&power, &calledWithValues](const SavedAdaptiveState& state) {
        calledWithValues = state.acValue == 1 && power.schemes[kScheme].ac == 1;
        return true;
    });
    CHECK(result == SuspendResult::Suspended);
    CHECK(calledWithValues);
}

TEST(adaptive_brightness_persist_first_false_aborts_without_writes)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    const SuspendResult result =
        adaptive.suspend(saved, [](const SavedAdaptiveState&) { return false; });
    CHECK(result == SuspendResult::Failed);
    CHECK_EQ(power.writes, 0);
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
}

TEST(adaptive_brightness_scheme_changed_mid_calibration_restores_saved_scheme_only)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Suspended);

    // The user switches to another scheme while the calibration runs.
    power.active = kOtherScheme;
    const size_t appliedBefore = power.applied.size();
    CHECK(adaptive.restore(saved));

    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kScheme].dc, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kOtherScheme].ac, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kOtherScheme].dc, static_cast<uint32_t>(0));
    CHECK_EQ(power.active, kOtherScheme);
    CHECK_EQ(power.applied.size(), appliedBefore);
}

TEST(adaptive_brightness_failed_restore_write_reports_false)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Suspended);
    power.failWrites = true;
    CHECK(!adaptive.restore(saved));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(0));
}

TEST(adaptive_brightness_failed_apply_after_restore_reports_false)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved) == SuspendResult::Suspended);
    power.failSetActive = true;
    CHECK(!adaptive.restore(saved));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
}

TEST(adaptive_brightness_restore_rejects_invalid_scheme_text)
{
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    SavedAdaptiveState saved;
    saved.schemeGuid = "not-a-scheme";
    saved.acValue = 1;
    saved.dcValue = 1;
    CHECK(!adaptive.restore(saved));
    CHECK_EQ(power.writes, 0);
}

TEST(adaptive_brightness_scheme_guid_text_is_validated)
{
    CHECK(isSchemeGuidText(kScheme));
    CHECK(!isSchemeGuidText("381b4222f694-41f0-9685-ff5bb260df2e"));
    CHECK(!isSchemeGuidText("{381b4222-f694-41f0-9685-ff5bb260df2e}"));
    CHECK(!isSchemeGuidText("381b4222-f694-41f0-9685-ff5bb260df2"));
}
