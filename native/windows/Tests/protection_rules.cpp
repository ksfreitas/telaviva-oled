// Tests for the flyout protection rules in native/windows/Platform/ProtectionRules.h.
#include "TestHarness.h"

#include "../Platform/ProtectionRules.h"

#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

using telaviva::clampProtectionBrightness;
using telaviva::isMonitorProtected;
using telaviva::protectionSliderEnabled;
using telaviva::protectionSliderMaximum;
using telaviva::protectionSliderMinimum;
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
    // The row shows only for a protected monitor while presence or screensaver protection is on.
    // Arguments are (presence, screensaver, protected).
    CHECK(!showProtectionBrightnessRow(false, false, false));
    CHECK(!showProtectionBrightnessRow(false, false, true));
    CHECK(!showProtectionBrightnessRow(true, false, false));
    CHECK(showProtectionBrightnessRow(true, false, true));
    CHECK(!showProtectionBrightnessRow(false, true, false));
    CHECK(showProtectionBrightnessRow(false, true, true));
    CHECK(!showProtectionBrightnessRow(true, true, false));
    CHECK(showProtectionBrightnessRow(true, true, true));
}

TEST(protectionRules_untouchedMonitorIsProtected)
{
    const std::set<std::string> none;
    CHECK(isMonitorProtected(none, "win-a"));

    const std::set<std::string> disabled{"win-a"};
    CHECK(!isMonitorProtected(disabled, "win-a"));
    CHECK(isMonitorProtected(disabled, "win-b"));
}

TEST(protectionRules_sliderRangeIsFixed)
{
    CHECK(protectionSliderMinimum() == 1.0);
    CHECK(protectionSliderMaximum() == 50.0);
}

TEST(protectionRules_sliderEnabledOnlyWithBrightnessSupport)
{
    CHECK(protectionSliderEnabled(true));
    CHECK(!protectionSliderEnabled(false));
}

TEST(protectionRules_clampBelowMinimumGivesMinimum)
{
    const auto clamped = clampProtectionBrightness(0.0);
    CHECK(clamped.value == 1.0);
    CHECK(clamped.changed);

    const auto negative = clampProtectionBrightness(-5.0);
    CHECK(negative.value == 1.0);
    CHECK(negative.changed);
}

TEST(protectionRules_clampAboveMaximumGivesMaximum)
{
    const auto clamped = clampProtectionBrightness(80.0);
    CHECK(clamped.value == 50.0);
    CHECK(clamped.changed);

    const auto edge = clampProtectionBrightness(100.0);
    CHECK(edge.value == 50.0);
    CHECK(edge.changed);
}

TEST(protectionRules_clampInRangeLeavesValue)
{
    const auto low = clampProtectionBrightness(1.0);
    CHECK(low.value == 1.0);
    CHECK(!low.changed);

    const auto kept = clampProtectionBrightness(30.0);
    CHECK(kept.value == 30.0);
    CHECK(!kept.changed);

    const auto high = clampProtectionBrightness(50.0);
    CHECK(high.value == 50.0);
    CHECK(!high.changed);
}

TEST(protectionRules_clampNonFiniteGivesMinimum)
{
    const auto nan = clampProtectionBrightness(std::numeric_limits<double>::quiet_NaN());
    CHECK(nan.value == 1.0);
    CHECK(nan.changed);

    const auto inf = clampProtectionBrightness(std::numeric_limits<double>::infinity());
    CHECK(inf.value == 1.0);
    CHECK(inf.changed);

    const auto negInf = clampProtectionBrightness(-std::numeric_limits<double>::infinity());
    CHECK(negInf.value == 1.0);
    CHECK(negInf.changed);
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
