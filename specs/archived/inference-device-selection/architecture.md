# Architecture: Inference device selection

**Feature**: Inference device selection (Settings > Advanced) **Date**: 2026-10-18 **Branch**: `inference-device-selection` **Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary

A new `inference_device` setting, stored by the shared Rust core and acted on by each native shell, lets the user choose where face detection runs: Automatic, a specific device, or the CPU. Each shell resolves the setting to a concrete device, validates an accelerated route against a CPU reference before using it, falls back to the CPU on any failure, and reports the device in use and any fallback notice on a new Settings > Advanced page. The core does not learn about inference: it only persists and validates the choice, and the device list, resolution and status stay in the shells.

## Technical Context

**Language/Stack**: Rust core and inference crate (C ABI), SwiftUI/AppKit shell (macOS), WinUI 3 / C++/WinRT shell (Windows).
**Key Dependencies**: ONNX Runtime through `ort =2.0.0-rc.9` with `load-dynamic`; DirectML (OS-supplied, Windows); CoreML (OS-supplied, macOS); DXGI + DXCore for adapter enumeration and iGPU/dGPU classification. No new third-party dependency is added.
**Storage**: the existing settings JSON file (`schema_version` stays 1; the new key is optional with a default). Failure markers keep using `%LOCALAPPDATA%\TelaVivaOLED\inference-failures.json`.
**Testing**: `cargo test` (core and inference), `tools/tests/*.sh` swiftc test binaries (macOS), `tests.exe` MSBuild project (Windows).
**Platform**: Windows 11 x64, macOS on Apple Silicon.

## Technical Decisions

### Decision 1: The core stores a device id string and knows nothing about inference

**What**: `Settings.inference_device: String`, default `"auto"`, validated only for shape (see contracts). Device enumeration, resolution, status and the notice live in each shell.
**Why**: the device list is platform-specific (DXGI/DXCore on Windows, a fixed set on macOS) and the status is runtime-only. Keeping them out of the core avoids a new core command, a snapshot field and an ABI revision, and keeps the "snapshot has no messages" contract intact.
**Alternatives Considered**: a core `inference_status` command that feeds the snapshot (symmetrical, but adds ABI and core state for data only the Advanced page reads); a core-side resolver (needs the device list across the ABI).
**Trade-offs**: resolution rules are written twice (C++ and Swift). Mitigated by one rule table in contracts.md, with the same test vectors in both test suites.

### Decision 2: Windows ships one DirectML-capable `onnxruntime.dll`

**What**: the package ships the DirectML variant of ONNX Runtime 1.20.1 as the only `onnxruntime.dll` (14.7 MB vs 11.6 MB for the CPU build); `directml\onnxruntime.dll`, the second runtime path and the process-bound `boundRuntime_` special case go away.
**Why**: verified with `dumpbin` on the local DirectML build: `d3d12.dll`, `dxgi.dll`, `dxcore.dll` and `DirectML.dll` are all delay-loaded, so the runtime loads and runs the CPU provider even when DirectML is absent. One runtime means the device can change live, with no restart (FR-009), and the CPU reference session in the self-test already runs on that same runtime today.
**Alternatives Considered**: keep two runtime variants and require a restart for CPU to DML switches (more code, a restart prompt, and a CPU path that differs from what is validated); load the DirectML runtime lazily (impossible: the runtime path is bound once per process).
**Trade-offs**: about 3 MB larger package for every user; every CPU user now runs the DirectML build of the library. Covered by CPU regression tests and the hardware checklist. `DirectML.dll` is still never redistributed (not open source); the system's own copy is used, and any failure to create the session falls back to the CPU.

### Decision 3: The Windows Automatic chain is iGPU then CPU, classified by DXCore

**What**: the catalog gains `luid` and `kind` (`integrated`, `discrete`, `unknown`) per adapter. `kind` comes from DXCore `IsIntegrated`, matched to the DXGI adapter by LUID. Automatic picks the first non-software adapter of kind `integrated` that has no failure marker, validates it, and otherwise uses the CPU. A discrete GPU is never picked by Automatic. `kind = unknown` (DXCore missing, no LUID match) is never picked by Automatic but can still be chosen manually.
**Why**: the spec requires that Automatic resolves to the CPU when the iGPU cannot be identified reliably, and requires the dGPU to be manual-only. A DXCore property query does not create a device, so classifying does not wake a sleeping discrete GPU (creating a D3D12 device per adapter, the other reliable method, would).
**Alternatives Considered**: D3D12 `ARCHITECTURE1.UMA` (reliable, but creates a device on every adapter); dedicated-memory heuristics (wrong on APUs); DXGI `EnumAdapterByGpuPreference(MINIMUM_POWER)` (reports the preferred low-power adapter, not a classification).
**Trade-offs**: relies on DXCore `InstanceLuid` equalling `DXGI_ADAPTER_DESC1.AdapterLuid`, which is not documented and must be confirmed on hardware; the dev machine (Radeon iGPU plus RTX) can confirm it. Until confirmed, no GPU is classified and Automatic stays on the CPU.

### Decision 4: Persisted GPU ids are `gpu:<vendor>:<device>:<ordinal>`, never an index or a LUID

**What**: the id is the PCI vendor and device ids in lowercase hex plus the ordinal among adapters sharing them (DXGI order). The runtime DirectML `device_id` is looked up from the current catalog at session creation time.
**Why**: indexes and LUIDs change across reboots and driver updates; vendor and device ids do not. FR-004 requires a stable identifier.
**Alternatives Considered**: LUID (changes at reboot); description string (localized, changes with drivers).
**Trade-offs**: two identical GPUs are told apart only by order.

### Decision 5: Validation replaces "accelerated must be faster"

**What**: a candidate is accepted when it creates a session, selects the expected provider, returns `present` for the self-test frame on both the candidate and the CPU reference, and the candidate's detect latency is at most 125 ms. The 125 ms budget is half of the shortest allowed sampling interval (the core accepts a return interval down to 0.25 s) and lives in one named constant. Measured on the dev machine (validation/windows-device-mapping.md): iGPU 9.4 ms median, 11 ms worst case. A runtime error afterwards drops to the CPU for the rest of the session, and the frame is retried on the CPU.
**Why**: the owner decided that speed is irrelevant as long as the cadence is met; offloading is the goal. The cadence budget protects the detection result.
**Alternatives Considered**: no latency bound (a pathological device could stall detection); keep "faster than CPU" (excludes the iGPU, measured about 2x slower).
**Trade-offs**: the budget was first written as 500 ms from a wrong 1 s minimum interval; it is now 125 ms and checked against the measured iGPU latency (about 11x headroom). Slower GPUs on other machines are still rejected by this bound.

### Decision 6: Failure markers apply to Automatic only

**What**: a failure of the iGPU under Automatic writes a marker `(device id, driver, app version)` and later launches skip it (avoiding a failed session and self-test every start). A manual choice ignores markers, so choosing the device again is the retry, and a manual failure never writes a permanent block.
**Why**: Automatic must be cheap and quiet; manual must be honest and retryable.
**Alternatives Considered**: markers for both (a manual retry would silently refuse); no markers (repeated failing startup work).
**Trade-offs**: the existing `InferenceFailureStore` format is reused unchanged; its `profile` field now holds the device id.

### Decision 7: Retire the acceleration profile allowlist

**What**: remove `acceleration-profiles.toml`, its parser, matcher and tests, and the installer's requirement for the file.
**Why**: the allowlist existed to keep DirectML off until a measurement was recorded. This feature replaces that gate with runtime validation, so the file (empty today) and its code are dead.
**Alternatives Considered**: keep it as a denylist (a hypothetical need; markers already cover real failures).
**Trade-offs**: large deletion (about 45 tests); it is done in its own phase so it can be reviewed separately.

### Decision 8: macOS gets real compute-unit control through ONNX Runtime 1.23 and string options

**What**: the macOS runtime is bumped to ONNX Runtime 1.23.0 (pinned by SHA-256 in `assets/model/manifest.toml`). CoreML is registered with `SessionOptionsAppendExecutionProvider("CoreML", ...)` and `MLComputeUnits` set to `ALL`, `CPUAndNeuralEngine`, `CPUAndGPU`, per the chosen entry. CPU uses the plain CPU provider, with no CoreML. The call goes through the `ort-sys` API table on the session builder's raw options pointer, because `ort` rc.9's wrapper only exposes flag bits and cannot express `CPUAndNeuralEngine`.
**Why**: ORT 1.20.1 cannot select the Neural Engine (its flags only express CPU only, CPU+GPU, and an ANE-device gate), and the owner chose Automatic, Neural Engine, GPU and CPU. `MLComputeUnits` was first accepted in 1.21.0; the values are exactly `ALL`, `CPUAndNeuralEngine`, `CPUAndGPU`, `CPUOnly`.
**Alternatives Considered**: stay on 1.20.1 and offer only Automatic, GPU and CPU (needs an owner decision, since it drops the Neural Engine entry); bump the whole stack to a newer `ort` crate (larger, riskier change).
**Trade-offs**: this is gated by a spike (Plan phase 0): confirm that `ort` rc.9 loads a 1.22 dylib (`GetApi(20)`), that the raw options pointer is reachable, that the release archive includes the CoreML provider, and that `CPUAndNeuralEngine` creates a session. If the spike fails, the Neural Engine entry is not shipped and the owner decides what replaces it. Windows stays on ORT 1.20.1 (not needed there).

**Implementation amendment (2026-10-18):** the 1.22.0 spike reproduced an upstream process-exit mutex abort even with CPU-only sessions. The owner authorized the changes needed to conclude macOS. Official 1.23.0 retains API 20 compatibility with `ort` rc.9 and passes CPU/CoreML session creation and clean shutdown; it replaces the initial 1.22.x target. See [the spike evidence](validation/macos-ort-spike.md). Windows keeps its existing 1.20.1 runtime.

### Decision 9: Device status lives in a shell-owned report, polled by the page

**What**: each shell exposes a small report: requested id, active id and label, and a state (`ok`, `auto_cpu`, `unavailable`, `failed`). Windows pages already poll the host snapshot once per second; macOS publishes the report through an observable object next to the core bridge.
**Why**: no core or ABI change, and both pages need the same four facts.
**Alternatives Considered**: logging only (current behavior, invisible at the default Error log level).
**Trade-offs**: the report is only as fresh as the last session creation; the session is created lazily, so before the first detection the page shows what Automatic would resolve to and states that the device is chosen when detection starts.

### Decision 10: The inference C ABI changes additively only

**What**: no new function. `tv_inference_create_with_provider_device(runtime, model, provider, device_index, out)` keeps its signature. For provider 1 (CoreML), `device_index` selects the compute units (0 `ALL`, 1 `CPUAndNeuralEngine`, 2 `CPUAndGPU`); for provider 2 (DirectML) it stays the adapter index. Provider 0 is the CPU. Registration for an explicit device uses error-on-failure registration so a hidden failure cannot report success; `selected_provider` stays the single source of truth for what was actually attached.
**Why**: avoids another ABI surface and header-contract churn. The existing CoreML code path (`with_execution_providers`, which hides failures) is replaced.
**Alternatives Considered**: a new `..._with_options` function taking a JSON or struct of options (cleaner for the future but unnecessary for three compute-unit values).
**Trade-offs**: `device_index` is overloaded per provider, documented in `api.md` and the header.

## Architecture Overview

Today the detector is created once per process on the CPU (macOS) or the CPU unless a profile matches (Windows, never in practice). After this change:

1. The core persists `inference_device` (default `auto`).
2. A shell observes the setting. On a change, it releases the current detector session and re-runs resolution.
3. Resolution turns the requested id plus the live device list into a candidate (CPU, iGPU/dGPU, or CoreML units). Automatic picks the platform chain; an explicit id that is no longer present resolves like Automatic and sets the `unavailable` state without discarding the stored value.
4. The candidate is created through the inference ABI and validated against a CPU reference session using the bundled self-test frame. Any failure falls back to the CPU and sets `failed` (explicit) or writes a marker (Automatic).
5. The Advanced page shows the list, the device in use and the notice.

The idle release (30 s on both platforms) is unchanged. A re-created session reuses the validated decision and skips the self-test, as today.

### Component Structure

**New Files/Modules:**
- `specs/inference-device-selection/validation/` - hardware validation records (Windows desktop with Radeon iGPU and RTX; macOS Apple Silicon).
- `adr/0027-selectable-inference-device-and-igpu-offload.md` - decision record amending ADR-0001 and ADR-0019.
- `native/windows/TelaViva/AdvancedPage.xaml`, `.xaml.cpp`, `.h` - the Advanced page (device ComboBox, in-use line, notice InfoBar).
- `native/windows/Platform/DeviceSelection.h/.cpp` - pure resolution: id parsing and formatting, device list building, Automatic chain, fallback state. UI-free so it is unit-testable.
- `native/windows/Tests/device_selection.cpp` - tests for the above.
- `native/macos/TelaViva/InferenceDeviceSelection.swift` - the same resolution rules and the observable status object.
- `native/macos/Tests/InferenceDeviceSelectionTests.swift` and `tools/tests/test-inference-device-selection.sh`.

**Modified Files:**
- `crates/telaviva-core/src/settings.rs`, `core.rs` - the new field, its default, validation and the `update_settings` arm.
- `crates/telaviva-core/tests/core_contract.rs`, `ffi_contract.rs` - default, persistence, rejection of bad values, older-snapshot compatibility.
- `crates/telaviva-inference/src/lib.rs` - CoreML compute-unit registration through string options, error-on-failure registration; `include/telaviva_inference.h` and `header_contract_tests.rs` for the documented `device_index` semantics.
- `native/windows/Platform/AdapterCatalog.h/.cpp` - `luid`, `kind`, DXCore classification (dxcore.dll loaded at runtime so a missing DLL only means `unknown`).
- `native/windows/Platform/InferenceHost.h/.cpp` - `setPreference`, `report()`, one runtime path, new `decideLocked` using resolution, validation with the cadence budget, markers for Automatic only.
- `native/windows/Platform/ProviderSelection.h/.cpp` - `judgeSelfTest` uses the latency budget instead of "faster".
- `native/windows/Platform/InferenceFailureStore.*` - unchanged format; only the meaning of the `profile` field changes.
- `native/windows/Platform/SettingsModel.*`, `JsonCodec.*` - `inference_device` in snapshot, draft, payload and defaults.
- `native/windows/TelaViva/SettingsWindow.xaml/.cpp`, `Host/SettingsHost.h`, `Host/AppController.cpp` - Advanced navigation item, host methods for the device list and report, wiring from the settings sink to `InferenceHost::setPreference`.
- `native/windows/TelaViva/Strings/*/Resources.resw` - en-US and pt-BR strings.
- `native/windows/Tests/provider_selection.cpp`, `inference_host.cpp`, `adapter_catalog.cpp`, `settings_payload.cpp`, `json_codec.cpp`, `strings_keys.cpp` - updated and new cases.
- `native/macos/TelaViva/SettingsNavigation.swift`, `SettingsView.swift`, `CoreBridge.swift`, `PresenceCamera.swift`, `Localization.swift` - Advanced tab, setting plumbing, detector recreation, strings.
- `packaging/windows/build-app.ps1`, `Install-TelaVivaOLED.ps1`, `tools/inference/fetch-windows-runtime.ps1` - ship the DirectML runtime as `onnxruntime.dll`; drop the profiles file.
- `packaging/macos/build-app.sh`, `tools/inference/fetch-macos-runtime.sh`, `assets/model/manifest.toml` - ONNX Runtime 1.22 pin for macOS.
- `api.md`, `ARCHITECTURE.md`, `docs/user-guide.md`, `CHANGELOG.md`, `adr/0001-...`, `adr/0019-...` (status line) - documentation.

**Removed Files:**
- `native/windows/Platform/AccelerationProfiles.h/.cpp`, `native/windows/Tests/acceleration_profiles.cpp`, `assets/inference/acceleration-profiles.toml`, and the profile loading in `AppController.cpp`.

### Component Relationships

`SettingsView` / `AdvancedPage` -> core (`update_settings`) -> snapshot -> shell sink -> `InferenceHost.setPreference` / `PresenceCamera.setInferenceDevice` -> resolution (`DeviceSelection` / `InferenceDeviceSelection`) -> inference ABI (`tv_inference_create_with_provider_device`) -> ONNX Runtime provider. Status flows back from the resolution and host to the page, not through the core.

### Data Flow

- **Change the device (US-002, US-003, US-004)**: the page edits a draft, sends `update_settings` with `inference_device`; the core validates and persists; the snapshot returns the value; the shell sink sees the change, releases the detector and records the new preference; the next detection creates and validates the session; the page's poll shows the device in use.
- **Automatic on Windows (US-001)**: catalog lists adapters with `kind`; resolution takes the first integrated non-software adapter without a marker; session create with its DXGI index; validation against a CPU reference; accept or fall back, with a marker on failure.
- **Device disappears (US-006)**: the stored id is not in the live list; resolution behaves as Automatic for this run, state `unavailable`, stored value kept; the page shows the notice.
- **Runtime error mid-run**: the host records the failure, falls back to the CPU, retries the frame there (never reporting `absent` for a failed frame), sets `failed` for an explicit choice or writes a marker for Automatic.

## Implementation Approach

### User Story Mapping

**US-001 (Windows Automatic offloads to iGPU)** - Files: `AdapterCatalog.*`, `DeviceSelection.*`, `InferenceHost.*`, packaging scripts. Key components: DXCore classification, Automatic chain, validation with the latency budget, single DirectML runtime. Dependencies: Decisions 2, 3, 5, 6. Testing: `device_selection` unit tests with a scripted catalog (integrated present, only discrete, unknown kind, software only, marker present); `inference_host` tests for the validation outcomes; a hardware record on the Radeon iGPU machine.

**US-002 (see detected devices)** - Files: `AdvancedPage.*`, `SettingsHost.h`, `AppController.cpp`, macOS `SettingsView.swift`. Key components: the device list built from the catalog (Windows: Automatic, CPU, one entry per GPU labelled by description and integrated or discrete; macOS: Automatic, Neural Engine, GPU, CPU). Testing: list building tests; strings parity tests.

**US-003 (force a device or the CPU)** - Files: setting plumbing in core and both shells, `InferenceHost.setPreference`. Dependencies: Decisions 1, 4, 6. Testing: core contract tests for valid and invalid ids; host tests that a changed preference drops the session and re-decides; a manual dGPU choice ignores markers.

**US-004 (macOS choices)** - Files: `lib.rs` (CoreML registration), `PresenceCamera.swift`, `InferenceDeviceSelection.swift`, macOS runtime pin. Dependencies: Decision 8 and its spike. Testing: Rust tests for the compute-unit mapping and invalid values; Swift tests for resolution; the Mac hardware record.

**US-005 (see the device in use and the notice)** - Files: the report in `InferenceHost` and the macOS status object; the pages. Testing: report state transitions in unit tests; UI Automation dump of the Windows page (windows are black when the session is locked, per the project's capture notes).

**US-006 (no crash if a device disappears or fails)** - Files: resolution and host fallback paths. Testing: fake detector API that fails to create, fails self-test, or errors mid-run; assert a detector always exists and frames are never lost.

## Integration Points

- Core settings (`update_settings` whitelist and `Settings::validate`), unchanged ABI version 1.
- Inference C ABI: additive semantics only, header contract test updated.
- Windows packaging, installer checks and the runtime fetch script.
- macOS packaging and signing of the bundled `libonnxruntime.dylib`.
- Documentation set listed under Modified Files.

## Technical Constraints

- Windows 11 x64 and Apple Silicon only. No NPU, no Qualcomm/Arm, no Intel Mac.
- Only open-source redistributed dependencies; `DirectML.dll` and CoreML are OS-supplied and never redistributed.
- No new network use.
- Parity: both platforms in the same change, with en-US and pt-BR strings and tests (AGENTS.md).
- The ONNX Runtime library path is bound once per process (`RUNTIME_PATH`); hence a single runtime on Windows.
- `Settings` uses `deny_unknown_fields`: the Windows payload must include the new key and the core must accept it before the shell sends it.
- CoreML behavior is a preference: unsupported operators run on the CPU and the app cannot report the unit that ran.
- The project's dev machine is a desktop without a battery or NPU: no energy claims (FR-013).

## Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| `ort` rc.9 cannot load or drive ONNX Runtime 1.22, or the raw options pointer is not reachable | macOS Neural Engine entry cannot ship as decided | Plan phase 0 spike before any macOS work; if it fails, stop and ask the owner (options: ship Automatic, GPU and CPU only, or bump the `ort` crate) |
| The macOS 1.22 archive has no CoreML provider | Same | The spike checks the exported symbol and the provider list at runtime |
| DXCore `InstanceLuid` does not match the DXGI LUID | No GPU is classified; Automatic stays on the CPU | Confirm on the dev machine before enabling; manual choice still works because it matches by vendor, device and ordinal |
| DXGI index does not map to the DirectML `device_id` as assumed | The wrong GPU gets the work (for example the RTX instead of the iGPU) | Use the same enumeration API ORT documents, and verify per-GPU engine utilisation on the dev machine during validation; the self-test cannot tell GPUs apart |
| The system's own `DirectML.dll` is older than ONNX Runtime 1.20.1 needs on some Windows 11 builds | Session creation fails on those machines | Automatic falls back to the CPU with a marker; record the minimum build in the validation notes (this machine has 1.15.5); no redistribution |
| The 125 ms budget is tighter than a slower iGPU's real latency | Automatic never uses that iGPU | Measured on the dev machine: 11 ms worst case. Other iGPUs fall back to the CPU, which is safe |
| The dGPU wakes from idle and uses more power when chosen manually | Higher power than the CPU | Manual only; the Advanced help text says so |
| Shipping the DirectML runtime to every Windows user regresses CPU behavior | Regression for users without an iGPU | CPU regression tests, hardware checklist rerun, idle CPU and memory check (SC-005) |
| Removing the profiles allowlist loses a way to block a known-bad driver | A bad driver keeps being tried once per driver version | Markers stop repeat attempts after the first failure; a future denylist can be added if needed |
| Rules drift between the C++ and Swift implementations | Platforms behave differently | One rule table and shared test vectors in contracts.md, mirrored in both test suites |

## Open Questions

None. The macOS ONNX Runtime upgrade is a gated decision (Risks, first row), not an open question: it is resolved by the phase 0 spike, and a failed spike returns to the owner.

## Next Steps

1. Review this architecture and `contracts.md`.
2. Run `/codeadvisor-dev:plan`.
