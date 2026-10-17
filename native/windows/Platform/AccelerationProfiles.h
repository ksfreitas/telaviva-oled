#pragma once
// Strict loader for assets/inference/acceleration-profiles.toml (schema 1) and
// the rule that matches an adapter to a profile. Errors are returned, never
// thrown; a rejected file means CPU only.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "AdapterCatalog.h"

namespace telaviva {

struct AccelerationProfile {
    std::string id;
    std::string provider;              // "directml" in schema 1
    uint32_t vendorId = 0;             // PCI vendor id
    std::vector<uint32_t> deviceIds;   // empty means any device of the vendor
    std::string minDriver;             // "a.b.c.d", inclusive
    std::string maxDriver;             // "a.b.c.d", inclusive; empty means no upper bound
    std::string measurement;           // reference to the recorded measurement
    std::string measuredOn;            // YYYY-MM-DD
};

struct ProfileLoadResult {
    std::vector<AccelerationProfile> profiles;
    std::string error;  // empty when the file was accepted
    bool ok() const { return error.empty(); }
};

// Accepts only the schema 1 subset: `schema = 1` first, then [[profile]] tables
// with the eight keys above, strings without escapes, integers (decimal or 0x),
// integer arrays, and # comments. Anything else is rejected with the first error.
ProfileLoadResult parseAccelerationProfiles(std::string_view text);

// A missing or unreadable file is an error, so the caller falls back to CPU and logs it.
ProfileLoadResult loadAccelerationProfiles(const std::filesystem::path& file);

// First profile in file order that matches the adapter, or nullptr. Software
// adapters never match, and neither does an adapter whose driver version does
// not parse as a.b.c.d.
const AccelerationProfile* matchProfile(const std::vector<AccelerationProfile>& profiles, const AdapterInfo& adapter);

}  // namespace telaviva
