#include "TestHarness.h"

#include "Platform/AdapterCatalog.h"

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using telaviva::AdapterInfo;
using telaviva::DxgiAdapterCatalog;
using telaviva::formatDriverVersion;
using telaviva::IAdapterCatalog;

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

TEST(adapter_catalog_fake_reports_software_flag)
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

TEST(adapter_catalog_classify_matching_luid_sets_integrated_or_discrete)
{
    AdapterInfo igpu = makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false);
    igpu.luid = 100;
    igpu.index = 0;
    AdapterInfo dgpu = makeAdapter(0x10de, 0x2684, "552.12.0.0", false);
    dgpu.luid = 200;
    dgpu.index = 1;
    const std::vector<telaviva::DxCoreAdapterInfo> dxcore = {
        telaviva::DxCoreAdapterInfo{100, true, true},
        telaviva::DxCoreAdapterInfo{200, false, true},
    };
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({igpu, dgpu}, dxcore);
    CHECK_EQ(result.size(), static_cast<size_t>(2));
    if (result.size() == 2) {
        CHECK(result[0].kind == telaviva::AdapterKind::Integrated);
        CHECK(result[1].kind == telaviva::AdapterKind::Discrete);
    }
}

TEST(adapter_catalog_classify_without_matching_luid_is_unknown)
{
    AdapterInfo adapter = makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false);
    adapter.luid = 300;
    const std::vector<telaviva::DxCoreAdapterInfo> dxcore = {telaviva::DxCoreAdapterInfo{100, true, true}};
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({adapter}, dxcore);
    CHECK_EQ(result.size(), static_cast<size_t>(1));
    if (result.size() == 1) {
        CHECK(result[0].kind == telaviva::AdapterKind::Unknown);
    }
}

TEST(adapter_catalog_classify_with_empty_dxcore_list_is_unknown)
{
    AdapterInfo adapter = makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false);
    adapter.luid = 100;
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({adapter}, {});
    CHECK_EQ(result.size(), static_cast<size_t>(1));
    if (result.size() == 1) {
        CHECK(result[0].kind == telaviva::AdapterKind::Unknown);
    }
}

TEST(adapter_catalog_classify_leaves_software_adapter_unknown)
{
    AdapterInfo software = makeAdapter(0x1414, 0x008c, "32.0.21041.1000", true);
    software.luid = 100;
    const std::vector<telaviva::DxCoreAdapterInfo> dxcore = {telaviva::DxCoreAdapterInfo{100, false, true}};
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({software}, dxcore);
    CHECK_EQ(result.size(), static_cast<size_t>(1));
    if (result.size() == 1) {
        CHECK(result[0].kind == telaviva::AdapterKind::Unknown);
    }
}

TEST(adapter_catalog_classify_ignores_zero_luid)
{
    AdapterInfo adapter = makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false);
    adapter.luid = 0;
    const std::vector<telaviva::DxCoreAdapterInfo> dxcore = {
        telaviva::DxCoreAdapterInfo{0, true, true},
        telaviva::DxCoreAdapterInfo{0, false, true},
    };
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({adapter}, dxcore);
    CHECK_EQ(result.size(), static_cast<size_t>(1));
    if (result.size() == 1) {
        CHECK(result[0].kind == telaviva::AdapterKind::Unknown);
    }
}

TEST(adapter_catalog_classify_ignores_non_hardware_dxcore_entry)
{
    AdapterInfo adapter = makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false);
    adapter.luid = 100;
    const std::vector<telaviva::DxCoreAdapterInfo> dxcore = {telaviva::DxCoreAdapterInfo{100, true, false}};
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({adapter}, dxcore);
    CHECK_EQ(result.size(), static_cast<size_t>(1));
    if (result.size() == 1) {
        CHECK(result[0].kind == telaviva::AdapterKind::Unknown);
    }
}

TEST(adapter_catalog_classify_preserves_fields_size_and_order)
{
    AdapterInfo first = makeAdapter(0x10de, 0x2684, "552.12.0.0", false);
    first.description = "Test GeForce";
    first.luid = 200;
    first.index = 0;
    AdapterInfo second = makeAdapter(0x1002, 0x164e, "32.0.21041.1000", false);
    second.description = "Test Radeon";
    second.luid = 100;
    second.index = 3;
    const std::vector<telaviva::DxCoreAdapterInfo> dxcore = {telaviva::DxCoreAdapterInfo{100, true, true}};
    const std::vector<AdapterInfo> result = telaviva::classifyKinds({first, second}, dxcore);
    CHECK_EQ(result.size(), static_cast<size_t>(2));
    if (result.size() == 2) {
        CHECK_EQ(result[0].description, std::string("Test GeForce"));
        CHECK(result[0].vendorId == 0x10de);
        CHECK(result[0].index == 0u);
        CHECK_EQ(result[0].driverVersion, std::string("552.12.0.0"));
        CHECK(result[0].luid == 200u);
        CHECK(result[0].kind == telaviva::AdapterKind::Unknown);
        CHECK_EQ(result[1].description, std::string("Test Radeon"));
        CHECK(result[1].vendorId == 0x1002);
        CHECK(result[1].index == 3u);
        CHECK_EQ(result[1].driverVersion, std::string("32.0.21041.1000"));
        CHECK(result[1].luid == 100u);
        CHECK(result[1].kind == telaviva::AdapterKind::Integrated);
    }
}
