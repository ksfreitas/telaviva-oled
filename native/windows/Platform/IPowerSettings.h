#pragma once
// Seam over the Windows power-scheme API, so the adaptive brightness policy can be
// tested without powrprof. Methods return result codes and never throw. Scheme GUIDs
// are lowercase text in 8-4-4-4-12 form without braces.
#include <cstdint>
#include <string>

namespace telaviva {

enum class PowerStatus { Ok, NotFound, Failed };

// AC and DC value indexes are stored separately for each scheme.
enum class PowerSource { Ac, Dc };

class IPowerSettings {
public:
    virtual ~IPowerSettings() = default;

    virtual PowerStatus activeScheme(std::string& schemeGuid) = 0;
    // NotFound means the adaptive brightness setting is absent from the scheme (desktop machines).
    virtual PowerStatus readAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t& value) = 0;
    virtual PowerStatus writeAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t value) = 0;
    // Makes the scheme active, which applies its stored values.
    virtual PowerStatus setActiveScheme(const std::string& schemeGuid) = 0;
};

}  // namespace telaviva
