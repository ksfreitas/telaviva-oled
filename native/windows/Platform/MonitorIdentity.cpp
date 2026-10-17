#include "MonitorIdentity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <unordered_map>

namespace telaviva {

namespace {

constexpr std::size_t kEdidBlockSize = 128;
constexpr std::array<uint8_t, 8> kEdidHeader{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
// Start offsets of the four 18-byte descriptors in the base block.
constexpr std::array<std::size_t, 4> kDescriptorOffsets{54, 72, 90, 108};
constexpr uint8_t kDisplayNameTag = 0xFC;
constexpr uint8_t kDescriptorTextEnd = 0x0A;

std::string trimSpaces(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(' ');
    return std::string(text.substr(first, last - first + 1));
}

}  // namespace

std::optional<EdidInfo> parseEdid(std::span<const uint8_t> edid)
{
    if (edid.size() < kEdidBlockSize) {
        return std::nullopt;
    }
    if (!std::equal(kEdidHeader.begin(), kEdidHeader.end(), edid.begin())) {
        return std::nullopt;
    }
    // The base block is valid when its bytes sum to 0 modulo 256.
    uint32_t sum = 0;
    for (std::size_t i = 0; i < kEdidBlockSize; ++i) {
        sum += edid[i];
    }
    if (sum % 256 != 0) {
        return std::nullopt;
    }

    const auto byteAt = [&edid](std::size_t i) { return static_cast<uint32_t>(edid[i]); };

    // Three 5-bit letters, 1 = 'A'. Values outside 1..26 are not a valid PNP ID.
    const uint32_t packed = (byteAt(8) << 8) | byteAt(9);
    std::string manufacturer;
    for (int shift : {10, 5, 0}) {
        const uint32_t letter = (packed >> shift) & 0x1F;
        if (letter < 1 || letter > 26) {
            return std::nullopt;
        }
        manufacturer.push_back(static_cast<char>('A' + letter - 1));
    }

    EdidInfo info;
    info.manufacturer = manufacturer;
    info.productCode = static_cast<uint16_t>(byteAt(10) | (byteAt(11) << 8));
    info.serial = byteAt(12) | (byteAt(13) << 8) | (byteAt(14) << 16) | (byteAt(15) << 24);

    for (std::size_t offset : kDescriptorOffsets) {
        const bool isDisplayName = edid[offset] == 0 && edid[offset + 1] == 0 &&
                                   edid[offset + 2] == 0 && edid[offset + 3] == kDisplayNameTag;
        if (!isDisplayName) {
            continue;
        }
        std::string text;
        for (std::size_t i = offset + 5; i < offset + 18; ++i) {
            if (edid[i] == kDescriptorTextEnd) {
                break;
            }
            text.push_back(static_cast<char>(edid[i]));
        }
        info.name = trimSpaces(text);
        break;
    }
    return info;
}

std::string edidHash8(std::span<const uint8_t> edid)
{
    if (edid.size() < kEdidBlockSize) {
        return {};
    }
    std::array<UCHAR, kEdidBlockSize> block{};
    std::copy_n(edid.begin(), kEdidBlockSize, block.begin());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }
    std::array<UCHAR, 32> digest{};
    // No HMAC secret: pbSecret is null and cbSecret is zero.
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, block.data(), static_cast<ULONG>(block.size()),
                                       digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return {};
    }

    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(8);
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(kDigits[digest[i] >> 4]);
        out.push_back(kDigits[digest[i] & 0x0F]);
    }
    return out;
}

bool serialIsUsable(uint32_t serial)
{
    return serial != 0 && serial != 1 && serial != 0x01010101u && serial != 0xFFFFFFFFu;
}

std::string buildMonitorId(const EdidInfo& info, const std::string& hash8)
{
    return "win-" + info.manufacturer + "-" + std::to_string(info.productCode) + "-" +
           std::to_string(info.serial) + "-" + hash8;
}

std::vector<ResolvedIdentity> resolveIdentities(const std::vector<std::vector<uint8_t>>& edids)
{
    std::vector<ResolvedIdentity> results(edids.size());
    std::unordered_map<std::string, std::size_t> occurrences;

    for (std::size_t i = 0; i < edids.size(); ++i) {
        const std::optional<EdidInfo> info = parseEdid(edids[i]);
        if (!info) {
            continue;
        }
        const std::string hash8 = edidHash8(edids[i]);
        if (hash8.empty()) {
            continue;
        }
        results[i].id = buildMonitorId(*info, hash8);
        results[i].name = info->name;
        // Stable even when the serial is unusable: many monitors report serial 0, and
        // refusing them would make them uncontrollable. Identical EDIDs are handled below.
        results[i].stable = true;
        ++occurrences[results[i].id];
    }

    // Identical EDIDs cannot be told apart, so none of them may receive writes.
    for (ResolvedIdentity& result : results) {
        if (!result.id.empty() && occurrences[result.id] > 1) {
            result.stable = false;
        }
    }
    return results;
}

}  // namespace telaviva
