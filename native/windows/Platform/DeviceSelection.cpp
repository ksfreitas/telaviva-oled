#include "DeviceSelection.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {
namespace {

bool isDecimalDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isLowerHexDigit(char c)
{
    return isDecimalDigit(c) || (c >= 'a' && c <= 'f');
}

// Reads 4 lowercase hex digits starting at text[pos]. False when the text is too short or any digit is not one.
bool parseHex4(const std::string& text, size_t pos, uint32_t& value)
{
    if (text.size() < pos + 4) {
        return false;
    }
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = text[pos + i];
        if (!isLowerHexDigit(c)) {
            return false;
        }
        const uint32_t digit = isDecimalDigit(c) ? static_cast<uint32_t>(c - '0') : static_cast<uint32_t>(c - 'a' + 10);
        result = (result << 4) | digit;
    }
    value = result;
    return true;
}

// Number of earlier non-software adapters with the same vendor and device as adapters[position].
uint32_t ordinalAt(const std::vector<AdapterInfo>& adapters, size_t position)
{
    uint32_t ordinal = 0;
    for (size_t i = 0; i < position; ++i) {
        if (!adapters[i].software && adapters[i].vendorId == adapters[position].vendorId &&
            adapters[i].deviceId == adapters[position].deviceId) {
            ++ordinal;
        }
    }
    return ordinal;
}

// Position of the hardware adapter that a gpu id names (vendor, device and ordinal), or nullopt.
std::optional<size_t> findGpu(const std::vector<AdapterInfo>& adapters, const GpuId& gpu)
{
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].software || adapters[i].vendorId != gpu.vendorId || adapters[i].deviceId != gpu.deviceId) {
            continue;
        }
        if (ordinalAt(adapters, i) == gpu.ordinal) {
            return i;
        }
    }
    return std::nullopt;
}

DeviceKind gpuKind(AdapterKind kind)
{
    switch (kind) {
    case AdapterKind::Integrated:
        return DeviceKind::IntegratedGpu;
    case AdapterKind::Discrete:
        return DeviceKind::DiscreteGpu;
    case AdapterKind::Unknown:
        break;
    }
    return DeviceKind::Gpu;
}

// The Automatic chain: the first hardware adapter with kind Integrated is the only candidate. A candidate
// that carries a marker is skipped in favour of the CPU.
DeviceChoice resolveAuto(const std::vector<AdapterInfo>& adapters,
                         const std::function<bool(const std::string& id)>& hasMarker)
{
    DeviceChoice choice;
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].software || adapters[i].kind != AdapterKind::Integrated) {
            continue;
        }
        const std::string id = adapterDeviceId(adapters, i);
        if (hasMarker(id)) {
            choice.state = DeviceState::AutoCpu;
            return choice;
        }
        choice.useGpu = true;
        choice.adapter = adapters[i];
        choice.id = id;
        choice.state = DeviceState::Ok;
        choice.viaAuto = true;
        return choice;
    }
    choice.state = DeviceState::AutoCpu;
    return choice;
}

}  // namespace

std::string gpuDeviceId(uint32_t vendorId, uint32_t deviceId, uint32_t ordinal)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "gpu:%04x:%04x:%u", vendorId, deviceId, ordinal);
    return buffer;
}

std::optional<GpuId> parseGpuDeviceId(const std::string& id)
{
    // Exact shape: "gpu:" + 4 hex + ":" + 4 hex + ":" + 1 to 3 decimal digits.
    constexpr size_t kMinLength = 4 + 4 + 1 + 4 + 1 + 1;
    constexpr size_t kMaxLength = 4 + 4 + 1 + 4 + 1 + 3;
    if (id.size() < kMinLength || id.size() > kMaxLength || id.compare(0, 4, "gpu:") != 0) {
        return std::nullopt;
    }
    GpuId gpu;
    if (!parseHex4(id, 4, gpu.vendorId) || id[8] != ':' || !parseHex4(id, 9, gpu.deviceId) || id[13] != ':') {
        return std::nullopt;
    }
    uint32_t ordinal = 0;
    for (size_t i = 14; i < id.size(); ++i) {
        if (!isDecimalDigit(id[i])) {
            return std::nullopt;
        }
        ordinal = ordinal * 10 + static_cast<uint32_t>(id[i] - '0');
    }
    gpu.ordinal = ordinal;
    return gpu;
}

std::vector<DeviceOption> buildDeviceOptions(const std::vector<AdapterInfo>& adapters)
{
    std::vector<DeviceOption> options;

    DeviceOption automatic;
    automatic.id = "auto";
    automatic.kind = DeviceKind::Auto;
    options.push_back(std::move(automatic));

    for (size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].software) {
            continue;
        }
        DeviceOption option;
        option.id = adapterDeviceId(adapters, i);
        option.kind = gpuKind(adapters[i].kind);
        option.description = adapters[i].description;
        options.push_back(std::move(option));
    }

    DeviceOption cpu;
    cpu.id = "cpu";
    cpu.kind = DeviceKind::Cpu;
    options.push_back(std::move(cpu));
    return options;
}

std::string adapterDeviceId(const std::vector<AdapterInfo>& adapters, size_t position)
{
    if (position >= adapters.size() || adapters[position].software) {
        return std::string();
    }
    return gpuDeviceId(adapters[position].vendorId, adapters[position].deviceId, ordinalAt(adapters, position));
}

DeviceChoice resolveDevice(const std::string& requestedId, const std::vector<AdapterInfo>& adapters,
                           const std::function<bool(const std::string& id)>& hasMarker)
{
    if (requestedId == "cpu") {
        return DeviceChoice{};
    }
    if (requestedId == "auto") {
        return resolveAuto(adapters, hasMarker);
    }
    if (const std::optional<GpuId> gpu = parseGpuDeviceId(requestedId)) {
        if (const std::optional<size_t> position = findGpu(adapters, *gpu)) {
            DeviceChoice choice;
            choice.useGpu = true;
            choice.adapter = adapters[*position];
            choice.id = adapterDeviceId(adapters, *position);
            choice.state = DeviceState::Ok;
            choice.viaAuto = false;
            return choice;
        }
    }
    // Missing, malformed or non-Windows requests resolve as auto and are reported as unavailable.
    DeviceChoice choice = resolveAuto(adapters, hasMarker);
    choice.state = DeviceState::Unavailable;
    return choice;
}

std::optional<std::string> inferenceNoticeKey(const InferenceReport& report)
{
    switch (report.state) {
    case DeviceState::Unavailable:
        return std::string("inference.notice.unavailable");
    case DeviceState::Failed:
        return std::string("inference.notice.failed");
    case DeviceState::Ok:
    case DeviceState::AutoCpu:
        break;
    }
    return std::nullopt;
}

}  // namespace telaviva
