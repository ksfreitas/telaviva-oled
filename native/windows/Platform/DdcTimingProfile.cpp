#include "DdcTimingProfile.h"

#include <cctype>
#include <string>

namespace telaviva {

DdcTimingProfile DdcTimingProfile::forManufacturer(std::string_view edidManufacturer)
{
    // EDID manufacturer codes are case-insensitive in practice; GSM (LG UltraFine)
    // drops the first Set, so it needs the duplicate write.
    std::string code(edidManufacturer);
    for (char& c : code) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    DdcTimingProfile profile;
    profile.duplicateSet = (code == "GSM");
    return profile;
}

}  // namespace telaviva
