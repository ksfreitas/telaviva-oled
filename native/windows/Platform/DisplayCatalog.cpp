#include "DisplayCatalog.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <unordered_map>
#include <wchar.h>

#include "MonitorIdentity.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace telaviva {

namespace {

// EDID blocks are 128 bytes, extensions included; the cap only bounds bad registry data.
constexpr std::size_t kMaxEdidBytes = 4096;

bool isHexDigit(wchar_t c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

// Braced 8-4-4-4-12 hex GUID, as used for the device interface class.
bool isInterfaceGuid(const std::wstring& text)
{
    if (text.size() != 38 || text.front() != L'{' || text.back() != L'}') {
        return false;
    }
    for (std::size_t i = 1; i < 37; ++i) {
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? text[i] != L'-' : !isHexDigit(text[i])) {
            return false;
        }
    }
    return true;
}

std::string toUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, out.data(), size, nullptr, nullptr);
    return out;
}

// Closes the registry key on every exit path.
class RegKey {
public:
    RegKey() = default;
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    ~RegKey()
    {
        if (handle_ != nullptr) {
            RegCloseKey(handle_);
        }
    }

    HKEY* put() { return &handle_; }
    HKEY get() const { return handle_; }

private:
    HKEY handle_ = nullptr;
};

bool isInternalTechnology(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
{
    return technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
}

// The topology can change between the size query and the read, so retry a few times.
bool queryPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return false;
        }
        paths.assign(pathCount, DISPLAYCONFIG_PATH_INFO{});
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount, DISPLAYCONFIG_MODE_INFO{});
        if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) ==
            ERROR_SUCCESS) {
            paths.resize(pathCount);
            return true;
        }
    }
    return false;
}

bool queryTargetName(const DISPLAYCONFIG_PATH_TARGET_INFO& target, DISPLAYCONFIG_TARGET_DEVICE_NAME& out)
{
    out = DISPLAYCONFIG_TARGET_DEVICE_NAME{};
    out.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    out.header.size = static_cast<UINT32>(sizeof(out));
    out.header.adapterId = target.adapterId;
    out.header.id = target.id;
    return DisplayConfigGetDeviceInfo(&out.header) == ERROR_SUCCESS;
}

std::wstring queryGdiName(const DISPLAYCONFIG_PATH_SOURCE_INFO& source)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = static_cast<UINT32>(sizeof(name));
    name.header.adapterId = source.adapterId;
    name.header.id = source.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(name.viewGdiDeviceName);
}

struct MonitorHandle {
    HMONITOR handle;
    std::wstring deviceName;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    // A C callback must not let exceptions escape.
    try {
        auto* out = reinterpret_cast<std::vector<MonitorHandle>*>(data);
        MONITORINFOEXW info{};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        if (GetMonitorInfoW(monitor, &info)) {
            out->push_back(MonitorHandle{monitor, std::wstring(info.szDevice)});
        }
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

std::vector<MonitorHandle> listMonitors()
{
    std::vector<MonitorHandle> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

// The GDI name is the only key shared by GetMonitorInfo and DisplayConfig sources.
uintptr_t findMonitor(const std::vector<MonitorHandle>& monitors, const std::wstring& gdiName)
{
    if (gdiName.empty()) {
        return 0;
    }
    for (const MonitorHandle& monitor : monitors) {
        if (_wcsicmp(monitor.deviceName.c_str(), gdiName.c_str()) == 0) {
            return reinterpret_cast<uintptr_t>(monitor.handle);
        }
    }
    return 0;
}

// The registry EDID is the only source of the monitor's own bytes; the
// DisplayConfig target name does not carry them.
std::vector<uint8_t> readEdidFromRegistry(const std::wstring& monitorDevicePath)
{
    const auto parts = parseDisplayDevicePath(monitorDevicePath);
    if (!parts) {
        return {};
    }
    const std::wstring subkey = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\") + parts->first +
                                L"\\" + parts->second + L"\\Device Parameters";
    RegKey key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key.get(), L"EDID", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_BINARY || size == 0 || size > kMaxEdidBytes) {
        return {};
    }
    std::vector<uint8_t> edid(size);
    DWORD actual = size;
    if (RegQueryValueExW(key.get(), L"EDID", nullptr, &type, edid.data(), &actual) != ERROR_SUCCESS ||
        type != REG_BINARY) {
        return {};
    }
    edid.resize(actual);
    return edid;
}

// The identity keeps the DisplayConfig friendly name until resolveIdentities
// supplies the EDID name, so applying it here preserves that fallback.
void applyIdentity(CatalogEntry& entry, const ResolvedIdentity& resolved)
{
    const std::string friendlyName = entry.identity.name;
    entry.identity = resolved;
    if (entry.identity.name.empty()) {
        entry.identity.name = friendlyName;
    }
}

void assignIdentities(std::vector<CatalogEntry>& entries)
{
    std::vector<std::vector<uint8_t>> externalEdids;
    std::vector<std::size_t> externalIndex;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        CatalogEntry& entry = entries[i];
        if (const auto info = parseEdid(entry.edid)) {
            entry.manufacturer = info->manufacturer;
        }
        if (entry.isInternalPanel) {
            // Resolved alone so the internal panel never joins duplicate grouping.
            const auto resolved = resolveIdentities({entry.edid});
            if (!resolved.empty()) {
                applyIdentity(entry, resolved.front());
            }
        } else if (!entry.edid.empty()) {
            externalIndex.push_back(i);
            externalEdids.push_back(entry.edid);
        }
    }

    // Duplicates among external monitors are marked unstable together here.
    const auto resolved = resolveIdentities(externalEdids);
    for (std::size_t k = 0; k < externalIndex.size() && k < resolved.size(); ++k) {
        applyIdentity(entries[externalIndex[k]], resolved[k]);
    }
}

void gatherEntries(std::vector<CatalogEntry>& entries)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    if (!queryPaths(paths)) {
        return;
    }
    const std::vector<MonitorHandle> monitors = listMonitors();
    std::unordered_map<std::wstring, std::size_t> indexByDevicePath;

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        if (!queryTargetName(path.targetInfo, target)) {
            continue;
        }
        const std::wstring devicePath = target.monitorDevicePath;
        if (devicePath.empty()) {
            continue;
        }
        const bool internal = isInternalTechnology(target.outputTechnology);
        // An internal panel is kept while its lid is closed, so it stays known as inactive.
        if (path.targetInfo.targetAvailable == FALSE && !internal) {
            continue;
        }
        const bool active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        const std::wstring gdiName = queryGdiName(path.sourceInfo);

        const auto found = indexByDevicePath.find(devicePath);
        if (found != indexByDevicePath.end()) {
            // Clone mode lists one target through several sources; prefer the active one.
            CatalogEntry& existing = entries[found->second];
            if (active && !existing.active) {
                existing.active = true;
                existing.gdiDeviceName = gdiName;
                existing.hmonitor = findMonitor(monitors, gdiName);
            }
            continue;
        }

        CatalogEntry entry;
        entry.connectionPath = toUtf8(devicePath);
        entry.gdiDeviceName = gdiName;
        entry.hmonitor = findMonitor(monitors, gdiName);
        entry.edid = readEdidFromRegistry(devicePath);
        entry.isInternalPanel = internal;
        entry.active = active;
        entry.identity.name = toUtf8(std::wstring(target.monitorFriendlyDeviceName));
        indexByDevicePath.emplace(devicePath, entries.size());
        entries.push_back(std::move(entry));
    }
}

}  // namespace

CatalogDiff diffCatalogs(const std::vector<CatalogEntry>& previous, const std::vector<CatalogEntry>& current)
{
    using Key = std::pair<std::string, bool>;

    // Counts let a second identical monitor be reported as added; removal stays per id.
    std::map<Key, std::size_t> remaining;
    for (const CatalogEntry& entry : previous) {
        if (!entry.identity.id.empty()) {
            ++remaining[Key{entry.identity.id, entry.identity.stable}];
        }
    }
    std::set<Key> currentKeys;
    for (const CatalogEntry& entry : current) {
        if (!entry.identity.id.empty()) {
            currentKeys.insert(Key{entry.identity.id, entry.identity.stable});
        }
    }

    CatalogDiff diff;
    for (const CatalogEntry& entry : current) {
        if (entry.identity.id.empty()) {
            continue;
        }
        const auto it = remaining.find(Key{entry.identity.id, entry.identity.stable});
        if (it != remaining.end() && it->second > 0) {
            --it->second;
            continue;
        }
        diff.added.push_back(entry);
    }

    std::set<std::string> seen;
    for (const CatalogEntry& entry : previous) {
        if (entry.identity.id.empty()) {
            continue;
        }
        if (currentKeys.count(Key{entry.identity.id, entry.identity.stable}) != 0) {
            continue;
        }
        if (seen.insert(entry.identity.id).second) {
            diff.removedIds.push_back(entry.identity.id);
        }
    }
    return diff;
}

std::optional<std::pair<std::wstring, std::wstring>> parseDisplayDevicePath(const std::wstring& monitorDevicePath)
{
    const std::wstring prefix = L"\\\\?\\";
    if (monitorDevicePath.size() <= prefix.size() || monitorDevicePath.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }

    const std::wstring rest = monitorDevicePath.substr(prefix.size());
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t separator = rest.find(L'#', start);
        if (separator == std::wstring::npos) {
            parts.push_back(rest.substr(start));
            break;
        }
        parts.push_back(rest.substr(start, separator - start));
        start = separator + 1;
    }

    if (parts.size() != 4 || _wcsicmp(parts[0].c_str(), L"DISPLAY") != 0) {
        return std::nullopt;
    }
    if (parts[1].empty() || parts[2].empty() || !isInterfaceGuid(parts[3])) {
        return std::nullopt;
    }
    return std::make_pair(parts[1], parts[2]);
}

std::vector<CatalogEntry> Win32DisplayCatalog::enumerate()
{
    std::vector<CatalogEntry> entries;
    try {
        gatherEntries(entries);
        assignIdentities(entries);
        std::stable_sort(entries.begin(), entries.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
            if (a.identity.id != b.identity.id) {
                return a.identity.id < b.identity.id;
            }
            return a.connectionPath < b.connectionPath;
        });
    } catch (...) {
        // Return whatever was gathered before the failure.
    }
    return entries;
}

}  // namespace telaviva
