#include "TestHarness.h"

#include "Platform/AdapterCatalog.h"
#include "Platform/DeviceSelection.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using telaviva::AdapterInfo;
using telaviva::AdapterKind;
using telaviva::adapterDeviceId;
using telaviva::buildDeviceOptions;
using telaviva::DeviceChoice;
using telaviva::DeviceKind;
using telaviva::DeviceOption;
using telaviva::DeviceState;
using telaviva::gpuDeviceId;
using telaviva::inferenceNoticeKey;
using telaviva::InferenceReport;
using telaviva::parseGpuDeviceId;
using telaviva::resolveDevice;

namespace {

AdapterInfo makeAdapter(uint32_t vendor, uint32_t device, const std::string& description, AdapterKind kind,
                        size_t index, bool software = false, uint64_t luid = 0)
{
    AdapterInfo info;
    info.vendorId = vendor;
    info.deviceId = device;
    info.description = description;
    info.kind = kind;
    info.index = static_cast<uint32_t>(index);
    info.software = software;
    info.luid = luid;
    return info;
}

bool noMarkers(const std::string&)
{
    return false;
}

}  // namespace

TEST(device_selection_gpu_id_formats_vendor_device_and_ordinal)
{
    CHECK_EQ(gpuDeviceId(0x1002, 0x164e, 0), std::string("gpu:1002:164e:0"));
    CHECK_EQ(gpuDeviceId(0x10de, 0x2684, 12), std::string("gpu:10de:2684:12"));
    CHECK_EQ(gpuDeviceId(0x8086, 0x0a, 3), std::string("gpu:8086:000a:3"));
}

TEST(device_selection_parse_gpu_id_round_trips)
{
    const std::string id = "gpu:10de:2684:12";
    const std::optional<telaviva::GpuId> parsed = parseGpuDeviceId(id);
    CHECK(parsed.has_value());
    if (parsed.has_value()) {
        CHECK(parsed->vendorId == 0x10de);
        CHECK(parsed->deviceId == 0x2684);
        CHECK(parsed->ordinal == 12);
        CHECK_EQ(gpuDeviceId(parsed->vendorId, parsed->deviceId, parsed->ordinal), id);
    }
}

TEST(device_selection_parse_gpu_id_rejects_malformed)
{
    const std::vector<std::string> invalid = {
        "",
        "auto",
        "gpu",
        "gpu:1002:164e",
        "gpu:1002:164E:0",
        "gpu:1:2:0",
        "gpu:1002:164e:",
        "gpu:1002:164e:0:1",
        "ane",
    };
    for (const std::string& id : invalid) {
        CHECK(!parseGpuDeviceId(id).has_value());
    }
}

TEST(device_selection_options_are_automatic_gpus_then_cpu)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
        makeAdapter(0x1414, 0x008c, "Basic Render", AdapterKind::Unknown, 2, true),
    };
    const std::vector<DeviceOption> options = buildDeviceOptions(adapters);
    CHECK_EQ(options.size(), static_cast<size_t>(4));
    if (options.size() == 4) {
        CHECK_EQ(options[0].id, std::string("auto"));
        CHECK(options[0].kind == DeviceKind::Auto);
        CHECK_EQ(options[0].description, std::string(""));
        CHECK_EQ(options[1].id, std::string("gpu:10de:2684:0"));
        CHECK(options[1].kind == DeviceKind::DiscreteGpu);
        CHECK_EQ(options[1].description, std::string("Test GeForce"));
        CHECK_EQ(options[2].id, std::string("gpu:1002:164e:0"));
        CHECK(options[2].kind == DeviceKind::IntegratedGpu);
        CHECK_EQ(options[2].description, std::string("Test Radeon"));
        CHECK_EQ(options[3].id, std::string("cpu"));
        CHECK(options[3].kind == DeviceKind::Cpu);
        CHECK_EQ(options[3].description, std::string(""));
    }
}

TEST(device_selection_options_skip_software_and_map_unknown_to_gpu)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Unknown, 0),
        makeAdapter(0x1414, 0x008c, "Basic Render", AdapterKind::Unknown, 1, true),
    };
    const std::vector<DeviceOption> options = buildDeviceOptions(adapters);
    CHECK_EQ(options.size(), static_cast<size_t>(3));
    if (options.size() == 3) {
        CHECK_EQ(options[1].id, std::string("gpu:1002:164e:0"));
        CHECK(options[1].kind == DeviceKind::Gpu);
        CHECK_EQ(options[2].id, std::string("cpu"));
    }
}

TEST(device_selection_empty_adapter_list_offers_auto_and_cpu)
{
    const std::vector<DeviceOption> options = buildDeviceOptions({});
    CHECK_EQ(options.size(), static_cast<size_t>(2));
    if (options.size() == 2) {
        CHECK_EQ(options[0].id, std::string("auto"));
        CHECK_EQ(options[1].id, std::string("cpu"));
    }
}

TEST(device_selection_identical_adapters_get_catalog_ordinals)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Test Radeon A", AdapterKind::Integrated, 0),
        makeAdapter(0x1002, 0x164e, "Test Radeon B", AdapterKind::Integrated, 1),
    };
    const std::vector<DeviceOption> options = buildDeviceOptions(adapters);
    CHECK_EQ(options.size(), static_cast<size_t>(4));
    if (options.size() == 4) {
        CHECK_EQ(options[1].id, std::string("gpu:1002:164e:0"));
        CHECK_EQ(options[2].id, std::string("gpu:1002:164e:1"));
    }
}

TEST(device_selection_software_adapter_does_not_count_toward_ordinal)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Basic Render", AdapterKind::Unknown, 0, true),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
    };
    const std::vector<DeviceOption> options = buildDeviceOptions(adapters);
    CHECK_EQ(options.size(), static_cast<size_t>(3));
    if (options.size() == 3) {
        CHECK_EQ(options[1].id, std::string("gpu:1002:164e:0"));
    }
    CHECK_EQ(adapterDeviceId(adapters, 1), std::string("gpu:1002:164e:0"));
}

TEST(device_selection_adapter_device_id_for_hardware_only)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1414, 0x008c, "Basic Render", AdapterKind::Unknown, 0, true),
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 1),
    };
    CHECK(adapterDeviceId(adapters, 0).empty());
    CHECK_EQ(adapterDeviceId(adapters, 1), std::string("gpu:10de:2684:0"));
    CHECK(adapterDeviceId(adapters, 2).empty());
}

TEST(device_selection_auto_picks_integrated_and_never_discrete)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
    };
    const DeviceChoice choice = resolveDevice("auto", adapters, noMarkers);
    CHECK(choice.useGpu);
    CHECK(choice.adapter.index == 1u);
    CHECK(choice.adapter.vendorId == 0x1002);
    CHECK_EQ(choice.id, std::string("gpu:1002:164e:0"));
    CHECK(choice.state == DeviceState::Ok);
    CHECK(choice.viaAuto);
}

TEST(device_selection_auto_without_integrated_uses_cpu)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
    };
    const DeviceChoice choice = resolveDevice("auto", adapters, noMarkers);
    CHECK(!choice.useGpu);
    CHECK_EQ(choice.id, std::string("cpu"));
    CHECK(choice.state == DeviceState::AutoCpu);
}

TEST(device_selection_auto_unknown_kind_uses_cpu)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Unknown, 0),
    };
    const DeviceChoice choice = resolveDevice("auto", adapters, noMarkers);
    CHECK(!choice.useGpu);
    CHECK(choice.state == DeviceState::AutoCpu);
}

TEST(device_selection_auto_software_only_uses_cpu)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1414, 0x008c, "Basic Render", AdapterKind::Integrated, 0, true),
    };
    const DeviceChoice choice = resolveDevice("auto", adapters, noMarkers);
    CHECK(!choice.useGpu);
    CHECK(choice.state == DeviceState::AutoCpu);
}

TEST(device_selection_auto_marked_integrated_uses_cpu)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 0),
    };
    const auto marked = [](const std::string& id) { return id == "gpu:1002:164e:0"; };
    const DeviceChoice choice = resolveDevice("auto", adapters, marked);
    CHECK(!choice.useGpu);
    CHECK(choice.state == DeviceState::AutoCpu);
}

TEST(device_selection_auto_checks_marker_only_for_candidate)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
    };
    std::vector<std::string> asked;
    const auto recording = [&asked](const std::string& id) {
        asked.push_back(id);
        return false;
    };
    const DeviceChoice choice = resolveDevice("auto", adapters, recording);
    CHECK(choice.useGpu);
    CHECK_EQ(asked.size(), static_cast<size_t>(1));
    if (asked.size() == 1) {
        CHECK_EQ(asked[0], std::string("gpu:1002:164e:0"));
    }
}

TEST(device_selection_auto_no_adapters_uses_cpu)
{
    const DeviceChoice choice = resolveDevice("auto", {}, noMarkers);
    CHECK(!choice.useGpu);
    CHECK_EQ(choice.id, std::string("cpu"));
    CHECK(choice.state == DeviceState::AutoCpu);
}

TEST(device_selection_cpu_request_never_consults_markers)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 0),
    };
    int calls = 0;
    const auto counting = [&calls](const std::string&) {
        ++calls;
        return true;
    };
    const DeviceChoice choice = resolveDevice("cpu", adapters, counting);
    CHECK(!choice.useGpu);
    CHECK_EQ(choice.id, std::string("cpu"));
    CHECK(choice.state == DeviceState::Ok);
    CHECK(!choice.viaAuto);
    CHECK(calls == 0);
}

TEST(device_selection_explicit_discrete_ignores_markers)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
    };
    const auto marked = [](const std::string&) { return true; };
    const DeviceChoice choice = resolveDevice("gpu:10de:2684:0", adapters, marked);
    CHECK(choice.useGpu);
    CHECK(choice.adapter.index == 0u);
    CHECK_EQ(choice.id, std::string("gpu:10de:2684:0"));
    CHECK(choice.state == DeviceState::Ok);
    CHECK(!choice.viaAuto);
}

TEST(device_selection_missing_ordinal_resolves_as_auto_unavailable)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
    };
    const DeviceChoice choice = resolveDevice("gpu:1002:164e:5", adapters, noMarkers);
    CHECK(choice.useGpu);
    CHECK(choice.adapter.index == 1u);
    CHECK_EQ(choice.id, std::string("gpu:1002:164e:0"));
    CHECK(choice.state == DeviceState::Unavailable);
    CHECK(choice.viaAuto);
}

TEST(device_selection_missing_explicit_without_integrated_is_cpu_unavailable)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x10de, 0x2684, "Test GeForce", AdapterKind::Discrete, 0),
    };
    // Ordinal 1 does not exist (only ordinal 0 is present), so the id is missing from the live list.
    const DeviceChoice choice = resolveDevice("gpu:10de:2684:1", adapters, noMarkers);
    CHECK(!choice.useGpu);
    CHECK_EQ(choice.id, std::string("cpu"));
    CHECK(choice.state == DeviceState::Unavailable);
}

TEST(device_selection_non_gpu_requests_resolve_as_auto_unavailable)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 0),
    };
    const std::vector<std::string> requests = {"ane", "gpu", "gpu:zzzz:0:0"};
    for (const std::string& request : requests) {
        const DeviceChoice choice = resolveDevice(request, adapters, noMarkers);
        CHECK(choice.useGpu);
        CHECK(choice.adapter.index == 0u);
        CHECK(choice.state == DeviceState::Unavailable);
        CHECK(choice.viaAuto);
    }
}

TEST(device_selection_explicit_software_id_is_unavailable)
{
    const std::vector<AdapterInfo> adapters = {
        makeAdapter(0x1414, 0x008c, "Basic Render", AdapterKind::Unknown, 0, true),
        makeAdapter(0x1002, 0x164e, "Test Radeon", AdapterKind::Integrated, 1),
    };
    const DeviceChoice choice = resolveDevice("gpu:1414:008c:0", adapters, noMarkers);
    CHECK(choice.state == DeviceState::Unavailable);
    CHECK(!choice.adapter.software);
    CHECK(choice.useGpu);
    CHECK(choice.adapter.index == 1u);
}

TEST(device_selection_notice_keys)
{
    InferenceReport report;
    report.state = DeviceState::Unavailable;
    CHECK(inferenceNoticeKey(report) == std::string("inference.notice.unavailable"));
    report.state = DeviceState::Failed;
    CHECK(inferenceNoticeKey(report) == std::string("inference.notice.failed"));
    report.state = DeviceState::Ok;
    CHECK(!inferenceNoticeKey(report).has_value());
    report.state = DeviceState::AutoCpu;
    CHECK(!inferenceNoticeKey(report).has_value());
}
