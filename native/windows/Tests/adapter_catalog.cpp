#include "TestHarness.h"

#include "Platform/AccelerationProfiles.h"
#include "Platform/AdapterCatalog.h"

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using telaviva::AccelerationProfile;
using telaviva::AdapterInfo;
using telaviva::DxgiAdapterCatalog;
using telaviva::formatDriverVersion;
using telaviva::IAdapterCatalog;
using telaviva::matchProfile;
using telaviva::parseAccelerationProfiles;

namespace {

class FakeAdapterCatalog : public IAdapterCatalog {
public:
    explicit FakeAdapterCatalog(std::vector<AdapterInfo> adapters) : adapters_(std::move(adapters)) {}

    std::vector<AdapterInfo> adapters() override { return adapters_; }

private:
    std::vector<AdapterInfo> adapters_;
};

AdapterInfo makeAdapter(uint32_t vendor, uint32_t device, const std::string& driver, bool software)
{
    AdapterInfo info;
    info.vendorId = vendor;
    info.deviceId = device;
    info.description = "fake";
    info.driverVersion = driver;
    info.software = software;
    return info;
}

}  // namespace

TEST(adapter_catalog_formats_umd_version_as_four_parts)
{
    // High 32 bits 0x00200000 -> 32.0; low 32 bits 0x523103E8 -> 21041.1000.
    CHECK_EQ(formatDriverVersion(0x00200000523103E8ULL), std::string("32.0.21041.1000"));
    CHECK_EQ(formatDriverVersion(0), std::string("0.0.0.0"));
    CHECK_EQ(formatDriverVersion(0xFFFFFFFFFFFFFFFFULL), std::string("65535.65535.65535.65535"));
}

TEST(adapter_catalog_fake_reports_software_flag_to_matching)
{
    FakeAdapterCatalog fake({makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false),
                             makeAdapter(0x1414, 0x008C, "32.0.21041.1000", true)});
    IAdapterCatalog& catalog = fake;
    const std::vector<AdapterInfo> adapters = catalog.adapters();
    CHECK_EQ(adapters.size(), static_cast<size_t>(2));
    if (adapters.size() == 2) {
        CHECK(!adapters[0].software);
        CHECK(adapters[1].software);
    }

    // A software adapter is listed but never matched, even with a matching profile.
    const std::string text =
        "schema = 1\n[[profile]]\nid = \"amd-test\"\nprovider = \"directml\"\nvendor_id = 0x1414\n"
        "device_ids = []\nmin_driver = \"32.0.21041.1000\"\nmax_driver = \"\"\n"
        "measurement = \"specs/archived/windows-version/validation/measurements.md#amd-test\"\nmeasured_on = \"2026-10-17\"\n";
    const std::vector<AccelerationProfile> profiles = parseAccelerationProfiles(text).profiles;
    CHECK_EQ(profiles.size(), static_cast<size_t>(1));
    if (adapters.size() == 2) {
        CHECK(matchProfile(profiles, adapters[1]) == nullptr);
    }
}

TEST(adapter_catalog_fake_with_no_adapters_is_empty)
{
    FakeAdapterCatalog fake({});
    CHECK(fake.adapters().empty());
}

TEST(adapter_catalog_dxgi_enumeration_does_not_throw)
{
    std::vector<AdapterInfo> adapters;
    bool threw = false;
    try {
        DxgiAdapterCatalog catalog;
        IAdapterCatalog& interfaceView = catalog;
        adapters = interfaceView.adapters();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(!threw);
    // Sanity bound only: a real machine lists a handful of adapters.
    CHECK(adapters.size() < 1000);
}
