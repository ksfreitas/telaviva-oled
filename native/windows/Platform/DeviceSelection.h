#pragma once
// Inference device choice: the option list the Advanced page shows, the parsing of "gpu:" ids, and
// the resolution of the stored inference_device value to one device. Pure logic with no windows.h,
// so the rules can be tested without a GPU.
#include "AdapterCatalog.h"
#include "InferenceFailureStore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

enum class DeviceKind { Auto, Cpu, IntegratedGpu, DiscreteGpu, Gpu };   // Gpu = kind Unknown
enum class DeviceState { Ok, AutoCpu, Unavailable, Failed };

struct DeviceOption {
    std::string id;            // "auto", "cpu", or "gpu:<vendor4>:<device4>:<ordinal>" (lowercase hex)
    DeviceKind kind = DeviceKind::Auto;
    std::string description;   // adapter description for GPUs, empty for auto and cpu
};

// "gpu:1002:164e:0". vendor and device as 4 lowercase hex digits, ordinal decimal without padding.
std::string gpuDeviceId(uint32_t vendorId, uint32_t deviceId, uint32_t ordinal);
struct GpuId { uint32_t vendorId = 0; uint32_t deviceId = 0; uint32_t ordinal = 0; };
std::optional<GpuId> parseGpuDeviceId(const std::string& id);   // nullopt for anything that is not that exact form

// Hardware (non-software) adapters only, in catalog order. ordinal = number of earlier non-software
// adapters with the same vendorId and deviceId. Result: Automatic, one entry per GPU, then CPU.
std::vector<DeviceOption> buildDeviceOptions(const std::vector<AdapterInfo>& adapters);
// Id of adapters[position]; empty string for a software adapter or an out-of-range position.
std::string adapterDeviceId(const std::vector<AdapterInfo>& adapters, size_t position);

struct DeviceChoice {
    bool useGpu = false;               // false means the CPU provider
    AdapterInfo adapter;               // valid when useGpu
    std::string id = "cpu";            // id of the chosen device ("cpu" or the gpu id)
    DeviceState state = DeviceState::Ok;   // Ok, AutoCpu or Unavailable (never Failed here)
    bool viaAuto = false;              // the candidate came from the Automatic chain
};

// Contracts.md resolution table, Windows rows.
//  "cpu"                -> CPU, Ok.
//  "auto"               -> first hardware adapter with kind Integrated and no marker: GPU, Ok, viaAuto.
//                          None: CPU, AutoCpu. Discrete and Unknown kinds are never picked.
//  "gpu:..." present    -> that adapter (matched by vendor, device, ordinal; software never matches), Ok,
//                          markers ignored, viaAuto = false.
//  anything else that is not "auto"/"cpu" (missing GPU, malformed, "ane", bare "gpu") -> resolve as "auto",
//                          viaAuto as that resolution, and state = Unavailable.
// hasMarker(id) is called only for Automatic candidates.
DeviceChoice resolveDevice(const std::string& requestedId, const std::vector<AdapterInfo>& adapters,
                           const std::function<bool(const std::string& id)>& hasMarker);

// What the Advanced page reads. Produced by InferenceHost.
struct InferenceReport {
    std::string requestedId = "auto";
    std::string activeId = "cpu";
    std::string activeLabel = "CPU";   // adapter description for a GPU, "CPU" otherwise
    DeviceState state = DeviceState::AutoCpu;
    std::optional<FailureReason> reason;   // set when state is Failed (and for an Automatic fallback that wrote a marker)
    bool decided = false;              // false until the first session was created for the current preference
};

// "inference.notice.unavailable" for Unavailable, "inference.notice.failed" for Failed, nullopt otherwise.
std::optional<std::string> inferenceNoticeKey(const InferenceReport& report);

}  // namespace telaviva
