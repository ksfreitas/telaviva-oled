#include "TestHarness.h"

#include "Platform/AccelerationProfiles.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using telaviva::AccelerationProfile;
using telaviva::AdapterInfo;
using telaviva::loadAccelerationProfiles;
using telaviva::matchProfile;
using telaviva::parseAccelerationProfiles;
using telaviva::ProfileLoadResult;

namespace {

const std::string kSchemaLine = "schema = 1\n";
const std::string kEntryHeader = "[[profile]]\n";
const std::string kIdLine = "id = \"amd-test\"\n";
const std::string kProviderLine = "provider = \"directml\"\n";
const std::string kVendorLine = "vendor_id = 0x1002\n";
const std::string kDeviceLine = "device_ids = [0x164e, 0x1638]\n";
const std::string kMinLine = "min_driver = \"32.0.21041.1000\"\n";
const std::string kMaxLine = "max_driver = \"\"\n";
const std::string kMeasurementLine = "measurement = \"specs/archived/windows-version/validation/measurements.md#amd-test\"\n";
const std::string kMeasuredLine = "measured_on = \"2026-10-17\"\n";

std::string validText()
{
    return kSchemaLine + kEntryHeader + kIdLine + kProviderLine + kVendorLine + kDeviceLine + kMinLine + kMaxLine +
           kMeasurementLine + kMeasuredLine;
}

// Replaces the first occurrence; an absent target leaves the text unchanged, so a
// reject test on the unchanged valid text fails loudly.
std::string replaceOnce(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

bool rejected(const std::string& text)
{
    const ProfileLoadResult result = parseAccelerationProfiles(text);
    return !result.ok() && result.profiles.empty() && !result.error.empty();
}

std::vector<AccelerationProfile> parsedProfiles(const std::string& text)
{
    return parseAccelerationProfiles(text).profiles;
}

AdapterInfo adapter(uint32_t vendor, uint32_t device, const std::string& driver)
{
    AdapterInfo info;
    info.vendorId = vendor;
    info.deviceId = device;
    info.description = "test adapter";
    info.driverVersion = driver;
    info.software = false;
    return info;
}

std::string readText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path uniqueTempFile()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("telaviva-profiles-" + std::to_string(stamp) + "-" + std::to_string(std::random_device{}()) + ".toml");
}

}  // namespace

TEST(acceleration_profiles_shipped_file_is_schema_1_with_no_profiles)
{
    // This test file lives in native\windows\Tests, so the repository root is four levels up.
    const fs::path shipped =
        fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "assets" / "inference" /
        "acceleration-profiles.toml";
    const ProfileLoadResult result = loadAccelerationProfiles(shipped);
    CHECK(result.ok());
    CHECK(result.profiles.empty());
}

TEST(acceleration_profiles_accepts_complete_entry)
{
    const ProfileLoadResult result = parseAccelerationProfiles(validText());
    CHECK(result.ok());
    CHECK_EQ(result.profiles.size(), static_cast<size_t>(1));
    if (result.profiles.size() == 1) {
        const AccelerationProfile& profile = result.profiles[0];
        CHECK_EQ(profile.id, std::string("amd-test"));
        CHECK_EQ(profile.provider, std::string("directml"));
        CHECK_EQ(profile.vendorId, static_cast<uint32_t>(0x1002));
        CHECK_EQ(profile.deviceIds.size(), static_cast<size_t>(2));
        if (profile.deviceIds.size() == 2) {
            CHECK_EQ(profile.deviceIds[0], static_cast<uint32_t>(0x164e));
            CHECK_EQ(profile.deviceIds[1], static_cast<uint32_t>(0x1638));
        }
        CHECK_EQ(profile.minDriver, std::string("32.0.21041.1000"));
        CHECK(profile.maxDriver.empty());
        CHECK_EQ(profile.measurement, std::string("specs/archived/windows-version/validation/measurements.md#amd-test"));
        CHECK_EQ(profile.measuredOn, std::string("2026-10-17"));
    }
}

TEST(acceleration_profiles_accepts_empty_device_list_and_upper_bound)
{
    const std::string text = replaceOnce(validText(), kDeviceLine, "device_ids = []\n");
    const std::string withMax = replaceOnce(text, kMaxLine, "max_driver = \"33.0.0.0\"\n");
    const ProfileLoadResult result = parseAccelerationProfiles(withMax);
    CHECK(result.ok());
    if (result.profiles.size() == 1) {
        CHECK(result.profiles[0].deviceIds.empty());
        CHECK_EQ(result.profiles[0].maxDriver, std::string("33.0.0.0"));
    }
}

TEST(acceleration_profiles_accepts_only_schema_1)
{
    CHECK(parseAccelerationProfiles(kSchemaLine).ok());
    const std::string schema2 = replaceOnce(validText(), kSchemaLine, "schema = 2\n");
    CHECK(rejected(schema2));
    const std::string schemaString = replaceOnce(validText(), kSchemaLine, "schema = \"1\"\n");
    CHECK(rejected(schemaString));
}

TEST(acceleration_profiles_rejects_missing_schema)
{
    CHECK(rejected(""));
    const std::string noSchema = validText().substr(kSchemaLine.size());
    CHECK(rejected(noSchema));
}

TEST(acceleration_profiles_rejects_schema_after_profile_and_duplicate_schema)
{
    const std::string late = kEntryHeader + kIdLine + kSchemaLine;
    CHECK(rejected(late));
    const std::string twice = kSchemaLine + kSchemaLine;
    CHECK(rejected(twice));
}

TEST(acceleration_profiles_rejects_unknown_top_level_key)
{
    const std::string text = kSchemaLine + "extra = 1\n" + kEntryHeader + kIdLine + kProviderLine + kVendorLine +
                             kDeviceLine + kMinLine + kMaxLine + kMeasurementLine + kMeasuredLine;
    CHECK(rejected(text));
}

TEST(acceleration_profiles_rejects_unknown_profile_key)
{
    const std::string text = validText() + "bogus = 1\n";
    CHECK(rejected(text));
}

TEST(acceleration_profiles_rejects_unknown_table)
{
    const std::string single = validText() + "[profile]\n";
    CHECK(rejected(single));
    const std::string other = validText() + "[[profiles]]\n";
    CHECK(rejected(other));
}

TEST(acceleration_profiles_rejects_provider_other_than_directml)
{
    const char* const providers[] = {"cpu", "coreml", "DirectML", "dml", ""};
    for (const char* provider : providers) {
        const std::string line = std::string("provider = \"") + provider + "\"\n";
        const std::string text = replaceOnce(validText(), kProviderLine, line);
        CHECK(rejected(text));
    }
}

TEST(acceleration_profiles_rejects_entry_without_measurement)
{
    const std::string missing = replaceOnce(validText(), kMeasurementLine, "");
    CHECK(rejected(missing));
    const std::string empty = replaceOnce(validText(), kMeasurementLine, "measurement = \"\"\n");
    CHECK(rejected(empty));
}

TEST(acceleration_profiles_rejects_missing_required_keys)
{
    CHECK(rejected(replaceOnce(validText(), kIdLine, "")));
    CHECK(rejected(replaceOnce(validText(), kProviderLine, "")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "")));
    CHECK(rejected(replaceOnce(validText(), kDeviceLine, "")));
    CHECK(rejected(replaceOnce(validText(), kMinLine, "")));
    CHECK(rejected(replaceOnce(validText(), kMaxLine, "")));
    CHECK(rejected(replaceOnce(validText(), kMeasuredLine, "")));
}

TEST(acceleration_profiles_rejects_duplicate_key_in_profile)
{
    const std::string text = validText() + kVendorLine;
    CHECK(rejected(text));
}

TEST(acceleration_profiles_rejects_duplicate_profile_id)
{
    const std::string text = validText() + kEntryHeader + kIdLine + kProviderLine + kVendorLine + kDeviceLine +
                             kMinLine + kMaxLine + kMeasurementLine + kMeasuredLine;
    CHECK(rejected(text));
}

TEST(acceleration_profiles_rejects_invalid_id)
{
    const std::string upper = replaceOnce(validText(), kIdLine, "id = \"AMD-Test\"\n");
    CHECK(rejected(upper));
    const std::string empty = replaceOnce(validText(), kIdLine, "id = \"\"\n");
    CHECK(rejected(empty));
}

TEST(acceleration_profiles_rejects_wrong_value_types)
{
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = \"0x1002\"\n")));
    CHECK(rejected(replaceOnce(validText(), kDeviceLine, "device_ids = 0x164e\n")));
    CHECK(rejected(replaceOnce(validText(), kIdLine, "id = 5\n")));
    CHECK(rejected(replaceOnce(validText(), kMinLine, "min_driver = 32\n")));
    CHECK(rejected(replaceOnce(validText(), kMeasurementLine, "measurement = [1]\n")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = 1.5\n")));
}

TEST(acceleration_profiles_rejects_trailing_garbage)
{
    CHECK(rejected(replaceOnce(validText(), kSchemaLine, "schema = 1 junk\n")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = 0x1002 x\n")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = 0x1002,\n")));
    CHECK(rejected(replaceOnce(validText(), kDeviceLine, "device_ids = [0x164e,]\n")));
    CHECK(rejected(replaceOnce(validText(), kEntryHeader, "[[profile]] extra\n")));
    CHECK(rejected(replaceOnce(validText(), kMinLine, "min_driver = \"32.0.21041.1000\" x\n")));
}

TEST(acceleration_profiles_rejects_malformed_numbers)
{
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = 0x\n")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = -1\n")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = 0105\n")));
    CHECK(rejected(replaceOnce(validText(), kVendorLine, "vendor_id = 0x10000\n")));
    CHECK(rejected(replaceOnce(validText(), kDeviceLine, "device_ids = [0x164e, 0x164e]\n")));
    CHECK(rejected(replaceOnce(validText(), kDeviceLine, "device_ids = [0x1FFFF]\n")));
}

TEST(acceleration_profiles_rejects_invalid_driver_versions)
{
    CHECK(rejected(replaceOnce(validText(), kMinLine, "min_driver = \"32.0.21041\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMinLine, "min_driver = \"32.0.21041.70000\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMinLine, "min_driver = \"a.b.c.d\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMinLine, "min_driver = \"\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMaxLine, "max_driver = \"32.0.21041\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMaxLine, "max_driver = \"32.0.21040.1000\"\n")));
}

TEST(acceleration_profiles_rejects_invalid_measured_on)
{
    CHECK(rejected(replaceOnce(validText(), kMeasuredLine, "measured_on = \"2026-1-7\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMeasuredLine, "measured_on = \"20261017\"\n")));
    CHECK(rejected(replaceOnce(validText(), kMeasuredLine, "measured_on = \"2026-13-01\"\n")));
}

TEST(acceleration_profiles_accepts_comments_crlf_and_hash_in_strings)
{
    std::string text = replaceOnce(validText(), kVendorLine, "vendor_id = 0x1002 # PCI vendor\n");
    text = "# leading comment\n\n" + text + "   # trailing comment\n";
    CHECK(parseAccelerationProfiles(text).ok());
    // The measurement reference contains '#', which must not start a comment.
    const ProfileLoadResult result = parseAccelerationProfiles(text);
    if (result.profiles.size() == 1) {
        CHECK_EQ(result.profiles[0].measurement,
                 std::string("specs/archived/windows-version/validation/measurements.md#amd-test"));
    }

    std::string crlf;
    for (char c : validText()) {
        if (c == '\n') {
            crlf += "\r\n";
        } else {
            crlf += c;
        }
    }
    CHECK(parseAccelerationProfiles(crlf).ok());
}

TEST(acceleration_profiles_rejects_string_escapes)
{
    const std::string text = replaceOnce(validText(), kIdLine, "id = \"amd\\test\"\n");
    CHECK(rejected(text));
}

TEST(acceleration_profiles_rejects_unterminated_values)
{
    CHECK(rejected(replaceOnce(validText(), kIdLine, "id = \"amd-test\n")));
    CHECK(rejected(replaceOnce(validText(), kDeviceLine, "device_ids = [0x164e\n")));
    CHECK(rejected(replaceOnce(validText(), kProviderLine, "provider =\n")));
}

TEST(acceleration_profiles_load_missing_file_is_error)
{
    const fs::path missing = uniqueTempFile();
    const ProfileLoadResult result = loadAccelerationProfiles(missing);
    CHECK(!result.ok());
    CHECK(result.profiles.empty());
}

TEST(acceleration_profiles_load_reads_file_from_disk)
{
    const fs::path file = uniqueTempFile();
    {
        std::ofstream out(file, std::ios::binary);
        out << validText();
    }
    const ProfileLoadResult result = loadAccelerationProfiles(file);
    std::error_code ec;
    static_cast<void>(fs::remove(file, ec));
    CHECK(result.ok());
    CHECK_EQ(result.profiles.size(), static_cast<size_t>(1));
}

TEST(acceleration_profiles_load_rejects_bad_file_from_disk)
{
    const fs::path file = uniqueTempFile();
    {
        std::ofstream out(file, std::ios::binary);
        out << replaceOnce(validText(), kMeasurementLine, "");
    }
    const ProfileLoadResult result = loadAccelerationProfiles(file);
    std::error_code ec;
    static_cast<void>(fs::remove(file, ec));
    CHECK(!result.ok());
    CHECK(result.profiles.empty());
}

TEST(acceleration_profiles_match_vendor_and_device)
{
    const std::vector<AccelerationProfile> profiles = parsedProfiles(validText());
    CHECK_EQ(profiles.size(), static_cast<size_t>(1));
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "32.0.21041.1000")) == &profiles[0]);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x1638, "32.0.21041.1000")) == &profiles[0]);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x9999, "32.0.21041.1000")) == nullptr);
    CHECK(matchProfile(profiles, adapter(0x10DE, 0x164e, "32.0.21041.1000")) == nullptr);
}

TEST(acceleration_profiles_match_empty_device_list_means_any_device)
{
    const std::string text = replaceOnce(validText(), kDeviceLine, "device_ids = []\n");
    const std::vector<AccelerationProfile> profiles = parsedProfiles(text);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x9999, "32.0.21041.1000")) == &profiles[0]);
}

TEST(acceleration_profiles_match_driver_min_is_inclusive)
{
    const std::vector<AccelerationProfile> profiles = parsedProfiles(validText());
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "32.0.21041.1000")) == &profiles[0]);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "32.0.21041.999")) == nullptr);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "32.0.21041.1001")) == &profiles[0]);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "31.9.99999.1")) == nullptr);
}

TEST(acceleration_profiles_match_driver_max_is_inclusive_and_empty_is_open)
{
    const std::string bounded = replaceOnce(validText(), kMaxLine, "max_driver = \"33.0.0.0\"\n");
    const std::vector<AccelerationProfile> profiles = parsedProfiles(bounded);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "33.0.0.0")) == &profiles[0]);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "33.0.0.1")) == nullptr);

    const std::vector<AccelerationProfile> open = parsedProfiles(validText());
    CHECK(matchProfile(open, adapter(0x1002, 0x164e, "99.0.0.0")) == &open[0]);
}

TEST(acceleration_profiles_match_ignores_software_adapter)
{
    const std::vector<AccelerationProfile> profiles = parsedProfiles(validText());
    AdapterInfo warp = adapter(0x1002, 0x164e, "32.0.21041.1000");
    warp.software = true;
    CHECK(matchProfile(profiles, warp) == nullptr);
}

TEST(acceleration_profiles_match_unparsable_driver_is_no_match)
{
    const std::vector<AccelerationProfile> profiles = parsedProfiles(validText());
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "")) == nullptr);
    CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "32.0.21041")) == nullptr);
}

TEST(acceleration_profiles_match_empty_list_is_no_match)
{
    const std::vector<AccelerationProfile> none;
    CHECK(matchProfile(none, adapter(0x1002, 0x164e, "32.0.21041.1000")) == nullptr);
}

TEST(acceleration_profiles_match_returns_first_profile_in_file_order)
{
    const std::string second = kEntryHeader + "id = \"amd-second\"\n" + kProviderLine + kVendorLine + kDeviceLine +
                               kMinLine + kMaxLine + kMeasurementLine + kMeasuredLine;
    const std::vector<AccelerationProfile> profiles = parsedProfiles(validText() + second);
    CHECK_EQ(profiles.size(), static_cast<size_t>(2));
    if (profiles.size() == 2) {
        CHECK(matchProfile(profiles, adapter(0x1002, 0x164e, "32.0.21041.1000")) == &profiles[0]);
    }
}
