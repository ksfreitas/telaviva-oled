// Tests for the flyout protection rules in native/windows/Platform/ProtectionRules.h.
#include "TestHarness.h"

#include "../Platform/ProtectionRules.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using telaviva::clampProtectionBrightness;
using telaviva::isMonitorProtected;
using telaviva::protectionBrightnessCap;
using telaviva::protectionSliderEnabled;
using telaviva::protectionSliderMaximum;
using telaviva::showProtectionBrightnessRow;
using telaviva::showProtectionToggle;
using telaviva::withMonitorProtection;

std::vector<std::string> toVector(const std::set<std::string>& ids)
{
    return std::vector<std::string>(ids.begin(), ids.end());
}

}  // namespace

TEST(protectionRules_toggleVisibilityMatrix)
{
    CHECK(!showProtectionToggle(false, false));
    CHECK(showProtectionToggle(true, false));
    CHECK(showProtectionToggle(false, true));
    CHECK(showProtectionToggle(true, true));
}

TEST(protectionRules_brightnessRowVisibilityMatrix)
{
    // Protected and screensaver on is the only combination that shows the row.
    CHECK(!showProtectionBrightnessRow(false, false));
    CHECK(!showProtectionBrightnessRow(false, true));
    CHECK(!showProtectionBrightnessRow(true, false));
    CHECK(showProtectionBrightnessRow(true, true));
}

TEST(protectionRules_untouchedMonitorIsProtected)
{
    const std::set<std::string> none;
    CHECK(isMonitorProtected(none, "win-a"));

    const std::set<std::string> disabled{"win-a"};
    CHECK(!isMonitorProtected(disabled, "win-a"));
    CHECK(isMonitorProtected(disabled, "win-b"));
}

TEST(protectionRules_capIsConfirmedWithoutDraft)
{
    CHECK(protectionBrightnessCap(60.0, std::nullopt) == 60.0);
    CHECK(protectionBrightnessCap(120.0, std::nullopt) == 100.0);
    CHECK(protectionBrightnessCap(-5.0, std::nullopt) == 0.0);
}

TEST(protectionRules_capIsLowerOfConfirmedAndDraft)
{
    CHECK(protectionBrightnessCap(60.0, 40.0) == 40.0);
    CHECK(protectionBrightnessCap(40.0, 60.0) == 40.0);
    CHECK(protectionBrightnessCap(60.0, -10.0) == 0.0);
}

TEST(protectionRules_capIsZeroWithoutConfirmedValue)
{
    CHECK(protectionBrightnessCap(std::nullopt, std::nullopt) == 0.0);
    CHECK(protectionBrightnessCap(std::nullopt, 50.0) == 0.0);
}

TEST(protectionRules_zeroCapDisablesSlider)
{
    CHECK(!protectionSliderEnabled(0.0));
    CHECK(protectionSliderEnabled(50.0));
    CHECK(protectionSliderMaximum(0.0) == 1.0);
    CHECK(protectionSliderMaximum(30.0) == 30.0);
}

TEST(protectionRules_clampOnShowLowersStoredValue)
{
    const auto clamped = clampProtectionBrightness(80.0, 40.0);
    CHECK(clamped.value == 40.0);
    CHECK(clamped.changed);
}

TEST(protectionRules_clampOnShowRaisesNothing)
{
    const auto kept = clampProtectionBrightness(30.0, 40.0);
    CHECK(kept.value == 30.0);
    CHECK(!kept.changed);
}

TEST(protectionRules_clampToZeroCapReportsChange)
{
    const auto clamped = clampProtectionBrightness(20.0, 0.0);
    CHECK(clamped.value == 0.0);
    CHECK(clamped.changed);

    const auto alreadyZero = clampProtectionBrightness(0.0, 0.0);
    CHECK(alreadyZero.value == 0.0);
    CHECK(!alreadyZero.changed);
}

TEST(protectionRules_brightnessDecreaseClampsStoredValue)
{
    // Confirmed brightness drops from 60 to 40 while the stored protection brightness is 50.
    const double cap = protectionBrightnessCap(40.0, std::nullopt);
    const auto clamped = clampProtectionBrightness(50.0, cap);
    CHECK(clamped.value == 40.0);
    CHECK(clamped.changed);
}

TEST(protectionRules_brightnessChangeWithinCapLeavesValue)
{
    const double cap = protectionBrightnessCap(60.0, std::nullopt);
    const auto kept = clampProtectionBrightness(50.0, cap);
    CHECK(kept.value == 50.0);
    CHECK(!kept.changed);
}

TEST(protectionRules_disabledSetAddsAndRemovesMonitor)
{
    const std::set<std::string> empty;
    const std::set<std::string> added = withMonitorProtection(empty, "win-a", false);
    CHECK(added == std::set<std::string>{"win-a"});
    CHECK(!isMonitorProtected(added, "win-a"));

    const std::set<std::string> removed = withMonitorProtection(added, "win-a", true);
    CHECK(removed.empty());
    CHECK(isMonitorProtected(removed, "win-a"));
}

TEST(protectionRules_disabledSetKeepsOtherIds)
{
    const std::set<std::string> base{"win-b"};
    const std::set<std::string> result = withMonitorProtection(base, "win-a", false);
    CHECK(result == (std::set<std::string>{"win-a", "win-b"}));

    const std::set<std::string> cleared = withMonitorProtection(result, "win-b", true);
    CHECK(cleared == std::set<std::string>{"win-a"});
}

TEST(protectionRules_disabledSetIsSortedAndUnique)
{
    const std::set<std::string> base{"win-c"};
    const std::set<std::string> afterB = withMonitorProtection(base, "win-b", false);
    const std::set<std::string> afterA = withMonitorProtection(afterB, "win-a", false);
    const std::set<std::string> dedup = withMonitorProtection(afterA, "win-a", false);

    const std::vector<std::string> expected{"win-a", "win-b", "win-c"};
    CHECK(toVector(dedup) == expected);
    CHECK(dedup.size() == 3);
}

TEST(protectionRules_disabledSetRemoveUnlistedIdIsNoOp)
{
    const std::set<std::string> base{"win-a"};
    const std::set<std::string> result = withMonitorProtection(base, "win-z", true);
    CHECK(result == base);
}
