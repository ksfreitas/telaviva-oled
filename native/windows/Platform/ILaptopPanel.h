#pragma once
// Built-in panel brightness seam. No windows.h here; the WMI implementation
// (WmiPanel) lives in LaptopPanel.h/.cpp so the follower can be tested with fakes.
#include <optional>
#include <vector>

namespace telaviva {

struct ILaptopPanel {
    virtual ~ILaptopPanel() = default;

    // Current level as a percent in 0..100, or nullopt when the read fails.
    // Never returns 0 for a failed read.
    virtual std::optional<double> readPercent() = 0;

    // Writes the supported level nearest to percent. False when no level is
    // known or the write fails.
    virtual bool setPercent(double percent) = 0;

    // Supported brightness levels as whole percents, as last read. Empty when
    // unknown. Never reads the panel.
    virtual std::vector<int> supportedLevels() const = 0;
};

}  // namespace telaviva
