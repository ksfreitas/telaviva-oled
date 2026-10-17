#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace telaviva {

struct EdidInfo {
    std::string manufacturer;  // three uppercase letters
    uint16_t productCode = 0;
    uint32_t serial = 0;
    std::string name;  // from the 0xFC display name descriptor, trimmed, may be empty
};

// Returns nullopt unless the data has at least 128 bytes, the EDID header and a valid base block checksum.
std::optional<EdidInfo> parseEdid(std::span<const uint8_t> edid);

// Lowercase hex of the first 4 bytes of SHA-256 over the 128-byte base block; empty on failure.
std::string edidHash8(std::span<const uint8_t> edid);

// False for the placeholder serials that many panels report instead of a real one.
bool serialIsUsable(uint32_t serial);

// "win-" + manufacturer + "-" + product + "-" + serial + "-" + hash8. Never contains a connector path.
std::string buildMonitorId(const EdidInfo& info, const std::string& hash8);

struct ResolvedIdentity {
    std::string id;  // empty when the EDID does not parse
    bool stable = false;
    std::string name;
};

// One result per input, in the same order. Ids shared by more than one attached monitor are unstable.
std::vector<ResolvedIdentity> resolveIdentities(const std::vector<std::vector<uint8_t>>& edids);

}  // namespace telaviva
