// Tests for the monitored display rules in native/windows/Platform/MonitoredDisplayRules.h.
#include "TestHarness.h"

#include "../Platform/MonitoredDisplayRules.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using telaviva::BuiltinProtectionSnapshot;
using telaviva::MonitorSnapshot;
using telaviva::MonitoredDisplayItem;
using telaviva::MonitoredDisplaySnapshot;
using telaviva::builtinEntryVisible;
using telaviva::isUsableMonitor;
using telaviva::monitoredDisplayIdForIndex;
using telaviva::monitoredDisplayItems;
using telaviva::selectedMonitoredDisplayIndex;
using telaviva::thresholdTargetId;

// A monitor with the given identity status and brightness support.
MonitorSnapshot monitorWith(const std::string& id, const std::string& identityStatus, bool brightnessSupported)
{
    MonitorSnapshot monitor;
    monitor.id = id;
    monitor.identityStatus = identityStatus;
    monitor.brightness.supported = brightnessSupported;
    return monitor;
}

BuiltinProtectionSnapshot builtinWith(bool protectable, bool available)
{
    BuiltinProtectionSnapshot builtin;
    builtin.protectable = protectable;
    builtin.available = available;
    return builtin;
}

std::vector<std::string> idsOf(const std::vector<MonitoredDisplayItem>& items)
{
    std::vector<std::string> ids;
    for (const MonitoredDisplayItem& item : items) {
        ids.push_back(item.id);
    }
    return ids;
}

}  // namespace

TEST(monitoredDisplayRules_itemOrderIsAutomaticMonitorsThenBuiltin)
{
    const std::vector<MonitorSnapshot> monitors{
        monitorWith("win-b", "stable", true),
        monitorWith("win-a", "stable", true),
    };
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(true, true));

    CHECK(items.size() == 4);
    const std::vector<std::string> expectedIds{"", "win-b", "win-a", "builtin"};
    CHECK(idsOf(items) == expectedIds);
    CHECK(items[0].automatic);
    CHECK(!items[0].builtin);
    CHECK(!items[1].automatic);
    CHECK(!items[1].builtin);
    CHECK(!items[2].automatic);
    CHECK(!items[2].builtin);
    CHECK(!items[3].automatic);
    CHECK(items[3].builtin);
}

TEST(monitoredDisplayRules_onlyAutomaticWhenNothingIsUsable)
{
    const std::vector<MonitorSnapshot> none;
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(none, builtinWith(false, false));

    CHECK(items.size() == 1);
    CHECK(items[0].automatic);
    CHECK(items[0].id.empty());
}

TEST(monitoredDisplayRules_usableNeedsStableIdentityAndSupportedBrightness)
{
    CHECK(isUsableMonitor(monitorWith("win-a", "stable", true)));
    CHECK(!isUsableMonitor(monitorWith("win-a", "ambiguous", true)));
    CHECK(!isUsableMonitor(monitorWith("win-a", "", true)));
    CHECK(!isUsableMonitor(monitorWith("win-a", "stable", false)));
    CHECK(!isUsableMonitor(monitorWith("win-a", "ambiguous", false)));
}

TEST(monitoredDisplayRules_unusableMonitorsAreExcludedFromItems)
{
    const std::vector<MonitorSnapshot> monitors{
        monitorWith("win-stable", "stable", true),
        monitorWith("win-ambiguous", "ambiguous", true),
        monitorWith("win-unsupported", "stable", false),
        monitorWith("win-unknown", "", false),
    };
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(false, false));

    CHECK(items.size() == 2);
    const std::vector<std::string> expectedIds{"", "win-stable"};
    CHECK(idsOf(items) == expectedIds);
}

TEST(monitoredDisplayRules_builtinEntryVisibilityRule)
{
    CHECK(!builtinEntryVisible(builtinWith(false, false)));
    CHECK(!builtinEntryVisible(builtinWith(false, true)));
    CHECK(!builtinEntryVisible(builtinWith(true, false)));
    CHECK(builtinEntryVisible(builtinWith(true, true)));

    const std::vector<MonitorSnapshot> none;
    CHECK(monitoredDisplayItems(none, builtinWith(true, false)).size() == 1);
    CHECK(monitoredDisplayItems(none, builtinWith(false, true)).size() == 1);
    CHECK(monitoredDisplayItems(none, builtinWith(true, true)).size() == 2);
}

TEST(monitoredDisplayRules_selectedIndexForNullAndUnknown)
{
    const std::vector<MonitorSnapshot> monitors{monitorWith("win-a", "stable", true)};
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(true, true));

    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::nullopt), 0);
    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::optional<std::string>(std::string())), 0);
    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::optional<std::string>("win-z")), 0);
}

TEST(monitoredDisplayRules_selectedIndexForKnownIds)
{
    const std::vector<MonitorSnapshot> monitors{monitorWith("win-a", "stable", true)};
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(true, true));

    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::optional<std::string>("win-a")), 1);
    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::optional<std::string>("builtin")), 2);
}

TEST(monitoredDisplayRules_selectedBuiltinIsAutomaticWhenHidden)
{
    const std::vector<MonitorSnapshot> monitors{monitorWith("win-a", "stable", true)};
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(true, false));

    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::optional<std::string>("builtin")), 0);
}

TEST(monitoredDisplayRules_selectedUnusableMonitorIsAutomatic)
{
    const std::vector<MonitorSnapshot> monitors{monitorWith("win-a", "ambiguous", true)};
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(false, false));

    CHECK_EQ(selectedMonitoredDisplayIndex(items, std::optional<std::string>("win-a")), 0);
}

TEST(monitoredDisplayRules_idForIndex)
{
    const std::vector<MonitorSnapshot> monitors{monitorWith("win-a", "stable", true)};
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(true, true));

    CHECK(!monitoredDisplayIdForIndex(items, 0));
    CHECK(monitoredDisplayIdForIndex(items, 1) == std::optional<std::string>("win-a"));
    CHECK(monitoredDisplayIdForIndex(items, 2) == std::optional<std::string>("builtin"));
}

TEST(monitoredDisplayRules_idForOutOfRangeIndexIsAutomatic)
{
    const std::vector<MonitorSnapshot> monitors{monitorWith("win-a", "stable", true)};
    const std::vector<MonitoredDisplayItem> items = monitoredDisplayItems(monitors, builtinWith(false, false));

    CHECK(!monitoredDisplayIdForIndex(items, 2));
    CHECK(!monitoredDisplayIdForIndex(items, 99));
    CHECK(!monitoredDisplayIdForIndex(items, -1));
}

TEST(monitoredDisplayRules_thresholdTargetIsResolvedDisplayOrEmpty)
{
    MonitoredDisplaySnapshot none;
    none.id = std::nullopt;
    none.automatic = true;
    CHECK(thresholdTargetId(none).empty());

    MonitoredDisplaySnapshot monitor;
    monitor.id = std::optional<std::string>("win-a");
    monitor.automatic = false;
    CHECK_EQ(thresholdTargetId(monitor), std::string("win-a"));

    MonitoredDisplaySnapshot builtin;
    builtin.id = std::optional<std::string>("builtin");
    builtin.automatic = true;
    CHECK_EQ(thresholdTargetId(builtin), std::string("builtin"));
}
