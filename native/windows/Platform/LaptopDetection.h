#pragma once
// Laptop (built-in panel) availability for the Windows shell. The reasons are
// shell-only; the core only sees available: bool. Free of windows.h.
#include <atomic>
#include <functional>
#include <optional>

#include "ILaptopEnvironment.h"

namespace telaviva {

enum class LaptopReason {
    None,
    NoBuiltInDisplay,
    DisplayOffline,
    BrightnessNotAdjustable,
    ApiUnavailable,
    Unknown,  // reserved; detectLaptop never returns it
};

struct LaptopAvailability {
    bool available = false;
    LaptopReason reason = LaptopReason::None;
};

// Precedence: NoBuiltInDisplay, DisplayOffline, ApiUnavailable, BrightnessNotAdjustable, None.
LaptopAvailability detectLaptop(ILaptopEnvironment& environment);

// Wire strings that match the macOS BuiltInUnavailableReason raw values.
const char* laptopReasonString(LaptopReason reason);

// Reads the Win32 state. Lid and display state are fed by the app's power
// notifications, so there is no synchronous query for them. Used from the UI thread only.
class Win32LaptopEnvironment final : public ILaptopEnvironment {
public:
    std::optional<DisplayPathInfo> internalDisplayPath() override;
    std::optional<bool> lidOpen() override;
    bool displayOn() override;
    std::optional<bool> hasBattery() override;
    BrightnessReadability panelBrightnessReadable() override;

    // GUID_LIDSWITCH_STATE_CHANGE payload: 0 closed, nonzero open.
    void recordLidState(bool open);

    // GUID_SESSION_DISPLAY_STATUS payload: 0 off; 1 (on) and 2 (dimmed) count as on.
    void recordDisplayStatus(bool on);

    // Set by the app wiring once the WMI reader exists. Unset reports ApiMissing.
    std::function<BrightnessReadability()> panelBrightnessProbe;

private:
    // Written on the UI thread and read on the scheduler thread, so atomic.
    // -1 unknown (nullopt), 0 closed, 1 open.
    std::atomic<int> lidState_{-1};
    std::atomic<bool> displayOn_{true};  // assumed on until the session reports otherwise
};

}  // namespace telaviva
