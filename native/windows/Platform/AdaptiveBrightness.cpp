#include "AdaptiveBrightness.h"

#include <windows.h>
#include <powrprof.h>

#include <cstdio>

#pragma comment(lib, "PowrProf.lib")

namespace telaviva {
namespace {

// Adaptive brightness is one setting in the display subgroup of each power scheme.
const GUID kDisplaySubgroup = {0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
const GUID kAdaptiveBrightness = {0xfbd9aa66, 0x9553, 0x4097, {0xba, 0x44, 0xed, 0x6e, 0x9d, 0x65, 0xea, 0xb8}};

std::string formatGuid(const GUID& guid)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned int>(guid.Data1), static_cast<unsigned int>(guid.Data2),
                  static_cast<unsigned int>(guid.Data3), static_cast<unsigned int>(guid.Data4[0]),
                  static_cast<unsigned int>(guid.Data4[1]), static_cast<unsigned int>(guid.Data4[2]),
                  static_cast<unsigned int>(guid.Data4[3]), static_cast<unsigned int>(guid.Data4[4]),
                  static_cast<unsigned int>(guid.Data4[5]), static_cast<unsigned int>(guid.Data4[6]),
                  static_cast<unsigned int>(guid.Data4[7]));
    return std::string(buffer);
}

unsigned long long hexValue(const std::string& text, size_t position, size_t length)
{
    unsigned long long value = 0;
    for (size_t i = position; i < position + length; ++i) {
        const char c = text[i];
        unsigned long long digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned long long>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned long long>(c - 'a' + 10);
        } else {
            digit = static_cast<unsigned long long>(c - 'A' + 10);
        }
        value = value * 16 + digit;
    }
    return value;
}

bool parseGuid(const std::string& text, GUID& out)
{
    if (!isSchemeGuidText(text)) {
        return false;
    }
    out.Data1 = static_cast<unsigned long>(hexValue(text, 0, 8));
    out.Data2 = static_cast<unsigned short>(hexValue(text, 9, 4));
    out.Data3 = static_cast<unsigned short>(hexValue(text, 14, 4));
    out.Data4[0] = static_cast<unsigned char>(hexValue(text, 19, 2));
    out.Data4[1] = static_cast<unsigned char>(hexValue(text, 21, 2));
    // The last six bytes follow the dash at 23 in pairs.
    static const size_t kTailPositions[6] = {24, 26, 28, 30, 32, 34};
    for (size_t i = 0; i < 6; ++i) {
        out.Data4[2 + i] = static_cast<unsigned char>(hexValue(text, kTailPositions[i], 2));
    }
    return true;
}

// Only ERROR_NOT_FOUND means the setting is absent; other codes are plain failures.
PowerStatus fromWin32(DWORD error)
{
    if (error == ERROR_SUCCESS) {
        return PowerStatus::Ok;
    }
    if (error == ERROR_NOT_FOUND) {
        return PowerStatus::NotFound;
    }
    return PowerStatus::Failed;
}

}  // namespace

bool isSchemeGuidText(const std::string& text)
{
    if (text.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
            continue;
        }
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

AdaptiveBrightness::AdaptiveBrightness(IPowerSettings& power) : power_(power) {}

SuspendResult AdaptiveBrightness::suspend(SavedAdaptiveState& saved, const PersistFirst& persistFirst)
{
    saved = SavedAdaptiveState{};
    std::string scheme;
    if (power_.activeScheme(scheme) != PowerStatus::Ok || !isSchemeGuidText(scheme)) {
        return SuspendResult::Failed;
    }
    uint32_t acValue = 0;
    uint32_t dcValue = 0;
    const PowerStatus acStatus = power_.readAdaptive(scheme, PowerSource::Ac, acValue);
    if (acStatus == PowerStatus::NotFound) {
        return SuspendResult::NotApplicable;
    }
    if (acStatus != PowerStatus::Ok) {
        return SuspendResult::Failed;
    }
    if (power_.readAdaptive(scheme, PowerSource::Dc, dcValue) != PowerStatus::Ok) {
        return SuspendResult::Failed;
    }
    saved.schemeGuid = scheme;
    saved.acValue = acValue;
    saved.dcValue = dcValue;
    if (acValue == 0 && dcValue == 0) {
        return SuspendResult::WasOff;
    }
    if (persistFirst && !persistFirst(saved)) {
        return SuspendResult::Failed;
    }
    // A failed write is rolled back so the user's values are not left half changed.
    if (!writeBoth(scheme, 0, 0)) {
        static_cast<void>(writeBoth(scheme, acValue, dcValue));
        return SuspendResult::Failed;
    }
    if (!apply(scheme)) {
        static_cast<void>(writeBoth(scheme, acValue, dcValue));
        static_cast<void>(apply(scheme));
        return SuspendResult::Failed;
    }
    return SuspendResult::Suspended;
}

bool AdaptiveBrightness::restore(const SavedAdaptiveState& saved)
{
    if (!isSchemeGuidText(saved.schemeGuid)) {
        return false;
    }
    std::string active;
    if (power_.activeScheme(active) != PowerStatus::Ok) {
        return false;
    }
    if (!writeBoth(saved.schemeGuid, saved.acValue, saved.dcValue)) {
        return false;
    }
    // Switching to the saved scheme would undo the user's own scheme change made during calibration.
    if (active == saved.schemeGuid) {
        return apply(saved.schemeGuid);
    }
    return true;
}

AdaptiveReadResult AdaptiveBrightness::readActive(SavedAdaptiveState& out)
{
    out = SavedAdaptiveState{};
    std::string scheme;
    if (power_.activeScheme(scheme) != PowerStatus::Ok || !isSchemeGuidText(scheme)) {
        return AdaptiveReadResult::Failed;
    }
    uint32_t acValue = 0;
    uint32_t dcValue = 0;
    const PowerStatus acStatus = power_.readAdaptive(scheme, PowerSource::Ac, acValue);
    if (acStatus == PowerStatus::NotFound) {
        return AdaptiveReadResult::NotApplicable;
    }
    if (acStatus != PowerStatus::Ok || power_.readAdaptive(scheme, PowerSource::Dc, dcValue) != PowerStatus::Ok) {
        return AdaptiveReadResult::Failed;
    }
    out.schemeGuid = scheme;
    out.acValue = acValue;
    out.dcValue = dcValue;
    return AdaptiveReadResult::Read;
}

bool AdaptiveBrightness::writeBoth(const std::string& schemeGuid, uint32_t acValue, uint32_t dcValue)
{
    // Both writes run even when the first fails, so a rollback still reaches the other value.
    const bool acOk = power_.writeAdaptive(schemeGuid, PowerSource::Ac, acValue) == PowerStatus::Ok;
    const bool dcOk = power_.writeAdaptive(schemeGuid, PowerSource::Dc, dcValue) == PowerStatus::Ok;
    return acOk && dcOk;
}

bool AdaptiveBrightness::apply(const std::string& schemeGuid)
{
    return power_.setActiveScheme(schemeGuid) == PowerStatus::Ok;
}

PowerStatus Win32PowerSettings::activeScheme(std::string& schemeGuid)
{
    GUID* active = nullptr;
    const DWORD error = PowerGetActiveScheme(nullptr, &active);
    if (error != ERROR_SUCCESS || active == nullptr) {
        if (active != nullptr) {
            LocalFree(active);
        }
        return PowerStatus::Failed;
    }
    schemeGuid = formatGuid(*active);
    // PowerGetActiveScheme allocates the GUID, so the caller frees it.
    LocalFree(active);
    return PowerStatus::Ok;
}

PowerStatus Win32PowerSettings::readAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t& value)
{
    GUID scheme{};
    if (!parseGuid(schemeGuid, scheme)) {
        return PowerStatus::Failed;
    }
    DWORD raw = 0;
    const DWORD error = source == PowerSource::Ac
                            ? PowerReadACValueIndex(nullptr, &scheme, &kDisplaySubgroup, &kAdaptiveBrightness, &raw)
                            : PowerReadDCValueIndex(nullptr, &scheme, &kDisplaySubgroup, &kAdaptiveBrightness, &raw);
    if (error == ERROR_SUCCESS) {
        value = static_cast<uint32_t>(raw);
    }
    return fromWin32(error);
}

PowerStatus Win32PowerSettings::writeAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t value)
{
    GUID scheme{};
    if (!parseGuid(schemeGuid, scheme)) {
        return PowerStatus::Failed;
    }
    const DWORD raw = static_cast<DWORD>(value);
    const DWORD error = source == PowerSource::Ac
                            ? PowerWriteACValueIndex(nullptr, &scheme, &kDisplaySubgroup, &kAdaptiveBrightness, raw)
                            : PowerWriteDCValueIndex(nullptr, &scheme, &kDisplaySubgroup, &kAdaptiveBrightness, raw);
    return fromWin32(error);
}

PowerStatus Win32PowerSettings::setActiveScheme(const std::string& schemeGuid)
{
    GUID scheme{};
    if (!parseGuid(schemeGuid, scheme)) {
        return PowerStatus::Failed;
    }
    return fromWin32(PowerSetActiveScheme(nullptr, &scheme));
}

}  // namespace telaviva
