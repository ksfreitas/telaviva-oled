#pragma once
// Seams for laptop (built-in panel) availability. No windows.h here; the Win32
// reads live in LaptopDetection.cpp, so the decision table can be tested with fakes.
#include <optional>
#include <string>

namespace telaviva {

struct DisplayPathInfo {
    bool active = false;
    std::string name;  // DisplayConfig friendly name, for diagnostics only
};

enum class BrightnessReadability {
    Ok,
    NotReadable,
    ApiMissing,  // the WMI namespace or call is unavailable
};

struct ILaptopEnvironment {
    virtual ~ILaptopEnvironment() = default;

    // DisplayConfig target with outputTechnology INTERNAL, DISPLAYPORT_EMBEDDED or
    // UDI_EMBEDDED. Inactive paths count. nullopt when no such target is known.
    virtual std::optional<DisplayPathInfo> internalDisplayPath() = 0;

    // nullopt when no lid state was reported. The decision treats that as open.
    virtual std::optional<bool> lidOpen() = 0;

    // Session display status; false means the session display is off.
    virtual bool displayOn() = 0;

    // nullopt when the battery state is unknown.
    virtual std::optional<bool> hasBattery() = 0;

    virtual BrightnessReadability panelBrightnessReadable() = 0;
};

}  // namespace telaviva
