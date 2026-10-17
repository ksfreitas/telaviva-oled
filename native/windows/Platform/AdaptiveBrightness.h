#pragma once
// Turns Windows adaptive brightness off during a calibration and restores the saved values.
// The setting is stored per power scheme and per AC/DC source, so both values and the
// scheme GUID are saved. Never throws.
#include "IPowerSettings.h"

#include <cstdint>
#include <functional>
#include <string>

namespace telaviva {

struct SavedAdaptiveState {
    std::string schemeGuid;
    uint32_t acValue = 0;
    uint32_t dcValue = 0;
    bool operator==(const SavedAdaptiveState& other) const = default;
};

enum class SuspendResult {
    NotApplicable,  // setting absent (desktop); nothing was written
    Suspended,      // it was on and is now off; restore with the saved state
    WasOff,         // both values were already 0; nothing was written
    Failed          // a read or write failed; writes are rolled back when possible
};

// True for a GUID in 8-4-4-4-12 hex form without braces.
bool isSchemeGuidText(const std::string& text);

class AdaptiveBrightness {
public:
    // Runs after the state is saved and before the first write. Returning false aborts with no writes.
    using PersistFirst = std::function<bool(const SavedAdaptiveState&)>;

    explicit AdaptiveBrightness(IPowerSettings& power);

    // Saves the active scheme and both values into saved, then writes 0 to both and applies.
    // Pass persistFirst to write the session record before the change; it is the crash-safety hook.
    SuspendResult suspend(SavedAdaptiveState& saved, const PersistFirst& persistFirst = PersistFirst());

    // Writes the saved values back to saved.schemeGuid. Applies them only when that scheme is
    // still active, and never switches the active scheme. False means the state must be kept.
    bool restore(const SavedAdaptiveState& saved);

private:
    bool writeBoth(const std::string& schemeGuid, uint32_t acValue, uint32_t dcValue);
    bool apply(const std::string& schemeGuid);

    IPowerSettings& power_;
};

// Win32 implementation over PowerGetActiveScheme and the Power*ValueIndex functions.
class Win32PowerSettings final : public IPowerSettings {
public:
    PowerStatus activeScheme(std::string& schemeGuid) override;
    PowerStatus readAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t& value) override;
    PowerStatus writeAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t value) override;
    PowerStatus setActiveScheme(const std::string& schemeGuid) override;
};

}  // namespace telaviva
