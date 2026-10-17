#include "TestHarness.h"

#include "Platform/MonitorIdentity.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Recomputes byte 127 so the base block sums to 0 modulo 256.
void fixChecksum(std::vector<uint8_t>& edid)
{
    uint32_t sum = 0;
    for (std::size_t i = 0; i < 127; ++i) {
        sum += edid[i];
    }
    edid[127] = static_cast<uint8_t>((256 - sum % 256) % 256);
}

// Builds a valid 128-byte EDID base block. An empty name means no 0xFC descriptor.
std::vector<uint8_t> makeEdid(const std::string& letters, uint16_t product, uint32_t serial,
                              const std::string& name = std::string())
{
    std::vector<uint8_t> edid(128, 0);
    const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (std::size_t i = 0; i < 8; ++i) {
        edid[i] = header[i];
    }

    const uint32_t packed = (static_cast<uint32_t>(letters[0] - 'A' + 1) << 10) |
                            (static_cast<uint32_t>(letters[1] - 'A' + 1) << 5) |
                            static_cast<uint32_t>(letters[2] - 'A' + 1);
    edid[8] = static_cast<uint8_t>(packed >> 8);
    edid[9] = static_cast<uint8_t>(packed & 0xFF);
    edid[10] = static_cast<uint8_t>(product & 0xFF);
    edid[11] = static_cast<uint8_t>(product >> 8);
    for (std::size_t i = 0; i < 4; ++i) {
        edid[12 + i] = static_cast<uint8_t>((serial >> (8 * i)) & 0xFF);
    }

    if (!name.empty()) {
        // Descriptor at offset 54: 00 00 00 FC 00, then 13 text bytes ending with 0x0A and space padding.
        const std::size_t at = 54;
        edid[at + 3] = 0xFC;
        const std::string text = name.substr(0, 12);
        for (std::size_t i = 0; i < 13; ++i) {
            uint8_t value = 0x20;
            if (i < text.size()) {
                value = static_cast<uint8_t>(text[i]);
            } else if (i == text.size()) {
                value = 0x0A;
            }
            edid[at + 5 + i] = value;
        }
    }

    fixChecksum(edid);
    return edid;
}

}  // namespace

TEST(monitor_identity_parse_lg_fields)
{
    const std::vector<uint8_t> edid = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    const auto info = telaviva::parseEdid(edid);
    CHECK(info.has_value());
    if (!info) {
        return;
    }
    CHECK_EQ(info->manufacturer, std::string("GSM"));
    CHECK_EQ(info->productCode, 23456);
    CHECK_EQ(info->serial, 873459u);
    CHECK_EQ(info->name, std::string("LG ULTRAFINE"));
}

TEST(monitor_identity_parse_without_name_descriptor)
{
    const std::vector<uint8_t> edid = makeEdid("DEL", 41234, 7u);
    const auto info = telaviva::parseEdid(edid);
    CHECK(info.has_value());
    if (!info) {
        return;
    }
    CHECK_EQ(info->manufacturer, std::string("DEL"));
    CHECK_EQ(info->name, std::string());
}

TEST(monitor_identity_parse_rejects_short_data)
{
    std::vector<uint8_t> edid = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    edid.resize(127);
    CHECK(!telaviva::parseEdid(edid).has_value());
}

TEST(monitor_identity_parse_rejects_bad_header)
{
    std::vector<uint8_t> edid = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    edid[0] = 0x01;
    fixChecksum(edid);
    CHECK(!telaviva::parseEdid(edid).has_value());
}

TEST(monitor_identity_parse_rejects_bad_checksum)
{
    std::vector<uint8_t> edid = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    edid[20] = static_cast<uint8_t>(edid[20] ^ 0x01);
    CHECK(!telaviva::parseEdid(edid).has_value());
}

TEST(monitor_identity_build_id_format)
{
    telaviva::EdidInfo info;
    info.manufacturer = "GSM";
    info.productCode = 23456;
    info.serial = 873459u;
    info.name = "LG ULTRAFINE";
    const std::string id = telaviva::buildMonitorId(info, "1f3a9c2b");
    CHECK_EQ(id, std::string("win-GSM-23456-873459-1f3a9c2b"));
    CHECK(id.size() < 512);
}

TEST(monitor_identity_serial_placeholders)
{
    CHECK(!telaviva::serialIsUsable(0u));
    CHECK(!telaviva::serialIsUsable(1u));
    CHECK(!telaviva::serialIsUsable(0x01010101u));
    CHECK(!telaviva::serialIsUsable(0xFFFFFFFFu));
    CHECK(telaviva::serialIsUsable(873459u));
}

TEST(monitor_identity_hash8_format_and_sensitivity)
{
    const std::vector<uint8_t> edid = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    const std::string hash = telaviva::edidHash8(edid);
    CHECK_EQ(hash.size(), std::size_t(8));
    CHECK(hash.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK_EQ(telaviva::edidHash8(edid), hash);

    std::vector<uint8_t> changed = edid;
    changed[20] = static_cast<uint8_t>(changed[20] ^ 0x01);
    fixChecksum(changed);
    CHECK(telaviva::edidHash8(changed) != hash);
}

TEST(monitor_identity_resolve_single_monitor)
{
    const std::vector<uint8_t> edid = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    const auto results = telaviva::resolveIdentities({edid});
    CHECK_EQ(results.size(), std::size_t(1));
    if (results.size() != 1) {
        return;
    }
    CHECK(results[0].id.find("win-GSM-23456-873459-") == 0);
    CHECK(results[0].stable);
    CHECK_EQ(results[0].name, std::string("LG ULTRAFINE"));
}

TEST(monitor_identity_resolve_two_different_monitors)
{
    const std::vector<uint8_t> first = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    const std::vector<uint8_t> second = makeEdid("DEL", 41234, 7u, "DELL U2720Q");
    const auto results = telaviva::resolveIdentities({first, second});
    CHECK_EQ(results.size(), std::size_t(2));
    if (results.size() != 2) {
        return;
    }
    CHECK(results[0].stable);
    CHECK(results[1].stable);
    CHECK(!results[0].id.empty());
    CHECK(results[0].id != results[1].id);
}

TEST(monitor_identity_resolve_identical_pair_unstable)
{
    const std::vector<uint8_t> edid = makeEdid("GSM", 23456, 0u, "LG ULTRAFINE");
    const auto results = telaviva::resolveIdentities({edid, edid});
    CHECK_EQ(results.size(), std::size_t(2));
    if (results.size() != 2) {
        return;
    }
    CHECK(!results[0].id.empty());
    CHECK_EQ(results[0].id, results[1].id);
    CHECK(!results[0].stable);
    CHECK(!results[1].stable);
}

TEST(monitor_identity_resolve_identical_among_three)
{
    const std::vector<uint8_t> same = makeEdid("GSM", 23456, 0u, "LG ULTRAFINE");
    const std::vector<uint8_t> other = makeEdid("DEL", 41234, 7u, "DELL U2720Q");
    const auto results = telaviva::resolveIdentities({same, other, same});
    CHECK_EQ(results.size(), std::size_t(3));
    if (results.size() != 3) {
        return;
    }
    CHECK(!results[0].stable);
    CHECK(results[1].stable);
    CHECK(!results[2].stable);
    CHECK_EQ(results[0].id, results[2].id);
    CHECK(results[0].id != results[1].id);
}

TEST(monitor_identity_resolve_invalid_edid)
{
    std::vector<uint8_t> broken = makeEdid("GSM", 23456, 873459u, "LG ULTRAFINE");
    broken[30] = static_cast<uint8_t>(broken[30] ^ 0x01);
    const std::vector<uint8_t> valid = makeEdid("DEL", 41234, 7u, "DELL U2720Q");
    const auto results = telaviva::resolveIdentities({broken, valid});
    CHECK_EQ(results.size(), std::size_t(2));
    if (results.size() != 2) {
        return;
    }
    CHECK_EQ(results[0].id, std::string());
    CHECK(!results[0].stable);
    CHECK(!results[1].id.empty());
    CHECK(results[1].stable);
}

TEST(monitor_identity_resolve_unique_unusable_serial_is_stable)
{
    // Many panels report serial 0; a unique id stays controllable.
    const std::vector<uint8_t> edid = makeEdid("GSM", 23456, 0u, "LG ULTRAFINE");
    const auto results = telaviva::resolveIdentities({edid});
    CHECK_EQ(results.size(), std::size_t(1));
    if (results.size() != 1) {
        return;
    }
    CHECK(results[0].id.find("win-GSM-23456-0-") == 0);
    CHECK(results[0].stable);
}
