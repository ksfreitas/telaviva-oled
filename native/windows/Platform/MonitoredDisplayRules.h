#pragma once
// Pure rules for the monitored display picker and the per-display presence threshold. They mirror
// monitored_candidates and resolve_monitored in crates/telaviva-core/src/core.rs: a monitor counts
// only with a stable identity and supported brightness, and the built-in screen only while
// protectable and available. They hold no UI or core state.
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "JsonCodec.h"

namespace telaviva {

// The reserved id of the built-in screen, as the core names it.
inline constexpr const char* kBuiltinDisplayId = "builtin";

// One entry of the monitored display picker.
//   id        the value sent as monitored_display_id: empty for Automatic, "builtin" for the
//             built-in screen, otherwise the monitor id.
//   automatic true only for the Automatic entry (index 0), which sends null.
//   builtin   true only for the built-in screen entry.
struct MonitoredDisplayItem {
    std::string id;
    bool automatic = false;
    bool builtin = false;
};

// A monitor counts only with a stable identity and supported brightness, as the core requires.
inline bool isUsableMonitor(const MonitorSnapshot& monitor)
{
    return monitor.identityStatus == "stable" && monitor.brightness.supported;
}

// The built-in screen is listed only while it is protectable and available (visibility rule).
inline bool builtinEntryVisible(const BuiltinProtectionSnapshot& builtin)
{
    return builtin.protectable && builtin.available;
}

// Index 0 is Automatic. The usable monitors follow in snapshot order, then the built-in entry
// when builtinEntryVisible is true.
inline std::vector<MonitoredDisplayItem> monitoredDisplayItems(const std::vector<MonitorSnapshot>& monitors,
                                                               const BuiltinProtectionSnapshot& builtin)
{
    std::vector<MonitoredDisplayItem> items;
    items.push_back(MonitoredDisplayItem{std::string(), true, false});
    for (const MonitorSnapshot& monitor : monitors) {
        if (isUsableMonitor(monitor)) {
            items.push_back(MonitoredDisplayItem{monitor.id, false, false});
        }
    }
    if (builtinEntryVisible(builtin)) {
        items.push_back(MonitoredDisplayItem{kBuiltinDisplayId, false, true});
    }
    return items;
}

// Returns the index of the item for the selected id. Null, an unknown id, or an id that is not
// listed selects Automatic (index 0). Index 0 is never matched by id.
inline int selectedMonitoredDisplayIndex(const std::vector<MonitoredDisplayItem>& items,
                                         const std::optional<std::string>& monitoredDisplayId)
{
    if (!monitoredDisplayId) {
        return 0;
    }
    for (std::size_t i = 1; i < items.size(); ++i) {
        if (!items[i].automatic && items[i].id == *monitoredDisplayId) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// Returns the id to send for a picker index. Index 0 (Automatic) and any out-of-range index
// return nullopt, which sends null.
inline std::optional<std::string> monitoredDisplayIdForIndex(const std::vector<MonitoredDisplayItem>& items,
                                                             int index)
{
    if (index <= 0 || static_cast<std::size_t>(index) >= items.size()) {
        return std::nullopt;
    }
    return items[static_cast<std::size_t>(index)].id;
}

// Returns the display whose presence threshold the General page edits: the resolved monitored
// display id. Empty when none resolves, which means the global value is edited instead.
inline std::string thresholdTargetId(const MonitoredDisplaySnapshot& monitored)
{
    return monitored.id.value_or(std::string());
}

}  // namespace telaviva
