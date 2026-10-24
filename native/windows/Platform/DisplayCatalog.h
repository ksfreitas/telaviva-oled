#pragma once
// Display enumeration for the Windows shell. This header stays free of
// windows.h; all Win32 calls live in DisplayCatalog.cpp.
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MonitorIdentity.h"

namespace telaviva {

struct CatalogEntry {
    ResolvedIdentity identity;   // id, stable and name; the id comes from the EDID only
    std::string manufacturer;    // three letters from the EDID, empty when it does not parse
    std::string connectionPath;  // DisplayConfig monitorDevicePath, never part of the id
    std::wstring gdiDeviceName;  // for example \\.\DISPLAY2
    uintptr_t hmonitor = 0;      // 0 when no attached HMONITOR maps to the source
    std::vector<uint8_t> edid;   // raw registry EDID, empty when unavailable
    bool isInternalPanel = false;
    bool active = true;
    bool primary = false;        // MONITORINFOF_PRIMARY of the matched GDI monitor; false when none matches
};

struct IDisplayCatalog {
    virtual ~IDisplayCatalog() = default;
    // All monitors including the internal panel, in a stable order.
    virtual std::vector<CatalogEntry> enumerate() = 0;
};

struct CatalogDiff {
    std::vector<CatalogEntry> added;
    std::vector<std::string> removedIds;
};

// Compares by identity.id. Entries with an empty id are ignored. An id whose
// stable flag flipped is reported as removed and its new entry as added.
CatalogDiff diffCatalogs(const std::vector<CatalogEntry>& previous, const std::vector<CatalogEntry>& current);

// Splits \\?\DISPLAY#<hardwareId>#<instanceId>#{interface-guid} into its
// hardware and instance ids, keeping the input case. nullopt for any other shape.
std::optional<std::pair<std::wstring, std::wstring>> parseDisplayDevicePath(const std::wstring& monitorDevicePath);

class Win32DisplayCatalog : public IDisplayCatalog {
public:
    std::vector<CatalogEntry> enumerate() override;
};

}  // namespace telaviva
