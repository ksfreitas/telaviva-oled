#pragma once
// Pacing for DDC/CI transactions. The values are macOS-derived assumptions until
// hardware runs, so this table is the single tuning point (architecture Decision 6).
#include <chrono>
#include <string_view>
#include <vector>

namespace telaviva {

struct DdcTimingProfile {
    std::chrono::milliseconds interTransactionDelay{60};
    bool duplicateSet = false;
    std::chrono::milliseconds duplicateGap{10};
    std::vector<std::chrono::milliseconds> readRetryDelays{std::chrono::milliseconds(200),
                                                           std::chrono::milliseconds(1000)};
    std::chrono::milliseconds wakeGrace{2000};

    // Returns duplicateSet for manufacturers that drop the first Set (GSM, LG UltraFine).
    static DdcTimingProfile forManufacturer(std::string_view edidManufacturer);
};

}  // namespace telaviva
