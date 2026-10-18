# Windows interfaces (Phase 3 and 4 working contract)

Exact C++ shapes every worker follows. Namespace `telaviva`. These are the interface decisions of architecture.md Decisions 3, 4, 5, 6 and contracts.md. No worker may change a name or signature here without the coordinator.

## AdapterCatalog.h (additive)

```cpp
enum class AdapterKind { Unknown, Integrated, Discrete };

struct AdapterInfo {
    // existing fields unchanged: vendorId, deviceId, description, driverVersion, software, index
    uint64_t luid = 0;                 // (HighPart << 32) | LowPart of DXGI_ADAPTER_DESC1::AdapterLuid
    AdapterKind kind = AdapterKind::Unknown;
};

// One adapter as DXCore reports it. Pure data so classification is testable.
struct DxCoreAdapterInfo {
    uint64_t luid = 0;                 // InstanceLuid
    bool integrated = false;           // IsIntegrated
    bool hardware = true;              // IsHardware
};

// Returns a copy of dxgi where each non-software adapter whose luid equals a DxCoreAdapterInfo::luid
// (and luid != 0, and hardware == true) gets kind Integrated or Discrete from `integrated`.
// Adapters without a match, software adapters, and every adapter when `dxcore` is empty stay Unknown.
std::vector<AdapterInfo> classifyKinds(std::vector<AdapterInfo> dxgi, const std::vector<DxCoreAdapterInfo>& dxcore);
```

`DxgiAdapterCatalog::adapters()` fills `luid`, then loads `dxcore.dll` at runtime (LoadLibraryExW with LOAD_LIBRARY_SEARCH_SYSTEM32, GetProcAddress of `DXCoreCreateAdapterFactory`; no static link, no D3D12/DirectML device), lists GPU adapters, reads IsIntegrated / IsHardware / InstanceLuid, and returns `classifyKinds(...)`. Any failure leaves kinds Unknown. It never throws.

## DeviceSelection.h (new, UI-free, no windows.h)

```cpp
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
```

`DeviceSelection.h` includes `AdapterCatalog.h` and `InferenceFailureStore.h` (for `FailureReason`).

## ProviderSelection.h

Remove `AccelerationMatch` and `findAccelerationMatch` (profiles are retired in Phase 6, but `AccelerationProfiles.h` is no longer included by this header). Keep `SelfTestSample`. Add:

```cpp
// Longest accelerated detect call that still fits the detection cadence (seconds).
inline constexpr double kMaxAcceleratedLatencySeconds = 0.125;   // half of the 0.25 s minimum return interval
```

`judgeSelfTest(accelerated, cpu)` (same signature): both samples must have status 1 (present), else `SelfTestMismatch`; then `accelerated.seconds > kMaxAcceleratedLatencySeconds` returns `SelfTestSlow`; speed relative to the CPU no longer matters (equal or slower than the CPU passes). Otherwise `nullopt`.

## InferenceHost.h

- Remove `#include "AccelerationProfiles.h"` and `AccelerationOptions::profiles` and `directmlRuntimePath`. `AccelerationOptions` keeps `adapters`, `failures`, `clock`, `appVersion`, `selfTestFrame`, `diagnostics`. One runtime: the constructor `runtimePath` is the only runtime and is used for the CPU session, the accelerated session and the CPU reference session; drop `boundRuntime_`.
- New public methods (both thread-safe, take `mutex_`):
  - `void setPreference(const std::string& id);` no-op when equal to the current preference. Otherwise stores it, destroys any session (cancelling the idle timer exactly the way `releaseIdle()` does), resets `mode_` to `Undecided` and the report to `{requestedId = id, decided = false}`.
  - `InferenceReport report() const;`
- `Mode { Undecided, Accelerated, Cpu }` stays, but `Cpu`/`Accelerated` are final only for the current preference.
- `decideLocked()`: `adapters = acceleration_.adapters ? adapters->adapters() : {}` (CPU when no catalog). `choice = resolveDevice(preference_, adapters, hasMarker)` where `hasMarker(id)` is `failures && failures->hasMarker({id, driverOf(id), appVersion})`. If `!choice.useGpu`: mode Cpu, report `{activeId "cpu", activeLabel "CPU", state choice.state, decided true}`, log, done. Else build `activeKey_ = {choice.id, adapter.driverVersion, appVersion}`; call `failures->clearIfChanged(driver, appVersion)` first; open the GPU session with `choice.adapter.index` (provider DirectML); reject a result whose `selectedProvider != DirectML` as `RegisterFailed`; run the self-test (accelerated and a CPU reference session, using `judgeSelfTest`). On any failure: destroy, mode Cpu, and
  - if `choice.viaAuto`: write the marker for `activeKey_` with the reason, state `AutoCpu` (or `Unavailable` when the requested id was unavailable), reason set;
  - else (explicit id): no marker, state `Failed`, reason set.
  On success: mode Accelerated, report `{activeId choice.id, activeLabel adapter.description, state choice.state, decided true}`.
- `ensureSessionLocked()` with mode Accelerated reopens without a second self-test, as today; on failure it follows the same failure rule.
- `fallBackToCpuLocked(frame)` (runtime error on an accelerated session): marker only when the candidate was `viaAuto`, state `Failed` for explicit, `AutoCpu` for auto, reason `RuntimeError`; the frame is retried on a CPU session and is never reported absent for the failed frame.
- The Info log lines keep their current shape ("inference engine: directml (...)" / "inference engine: cpu (...)") and add the reason on fallback.

## Persisted setting and snapshot (Phase 3 task T019)

`SettingsSnapshot::inference_device` (`std::string`, default `"auto"` when the key is missing) in `JsonCodec`; `SettingsDraft::inferenceDevice`, copied by `draftFromSnapshotSettings`, included in `buildSettingsPayload` under the key `inference_device`; `clampToCoreRanges` replaces a value that is not `auto`, `cpu`, `ane`, `gpu` or a well-formed `gpu:vvvv:dddd:n` (lowercase hex, 1-3 digit ordinal, total length at most 32) with `"auto"`.

## Host methods for the page (Phase 4)

`ISettingsHost::inferenceDevices()` returns `std::vector<DeviceOption>` (from `buildDeviceOptions` on the live catalog) and `ISettingsHost::inferenceReport()` returns `InferenceReport` (from `InferenceHost::report()`).
