# Tasks: Inference device selection

**Branch**: `inference-device-selection` **Specs**: [spec.md](spec.md), [contracts.md](contracts.md) **Architecture**: [architecture.md](architecture.md) **Status**: Not Started ⏳

Story groups used below (the stories keep their spec IDs):

- **Group A, Windows engine**: US-001 (Automatic offloads to the iGPU), US-003 (force a device or the CPU), US-006 (no crash when a device disappears or fails).
- **Group B, Windows page**: US-002 (see detected devices), US-005 (device in use and fallback notice).
- **Group C, macOS**: US-004 (Automatic, Neural Engine, GPU, CPU) plus the macOS side of US-002, US-003, US-005, US-006.

Per AGENTS.md, both platforms ship in the same change. The groups are an ordering inside this branch, not separate releases. Tasks marked **(Mac)** need an Apple Silicon Mac.

## Phase 1: Setup and spikes ⏳ (~5h)

- [x] T001 Create `specs/inference-device-selection/validation/README.md` with the record template (machine, OS build, driver, command, result, date) used by every hardware record in this plan.
- [x] T002 [P] Write a throwaway probe in the session scratchpad (not in the repo) that lists every DXGI adapter (index, vendor, device, LUID, software flag) and every DXCore hardware adapter (`IsIntegrated`, `InstanceLuid`), then prints whether the LUIDs match. Run it on the dev desktop (Radeon iGPU + RTX) and record the output in `specs/inference-device-selection/validation/windows-adapter-probe.md`.
- [x] T003 [P] Using the shipped `telaviva_inference.dll`, `tools/inference/runtime/windows-x64-directml/onnxruntime.dll`, the YuNet model and `selftest-face.bgra`, measure detect latency on the CPU and on each DirectML `device_id`, and read per-LUID GPU engine utilisation (`typeperf "\GPU Engine(*)\Utilization Percentage"`) to confirm which physical GPU each index uses. Record in `validation/windows-device-mapping.md` and write down the iGPU latency next to the 500 ms budget.
- [ ] T004 [P] **(Mac)** Spike for Decision 8 on a branch of `crates/telaviva-inference`: fetch `onnxruntime-osx-arm64` 1.22.x, run `nm -gU libonnxruntime.dylib | grep CoreML`, check that `ort =2.0.0-rc.9` loads it (`GetApi(20)`), reach the raw `OrtSessionOptions*` of the session builder, call `SessionOptionsAppendExecutionProvider("CoreML", ...)` with `MLComputeUnits` = `CPUAndNeuralEngine`, and create a YuNet session. Record in `validation/macos-ort-spike.md`.
- [ ] T005 Decision gate on T004. If any step fails, stop the macOS phase and take the failure to the owner (options in architecture.md Risks). If T003 shows the iGPU above 500 ms, revise the budget constant in architecture.md and contracts.md before Phase 3.

**Checkpoint**: ✋ LUID mapping, DXGI-to-DirectML index mapping, the latency budget and the macOS ORT route are known.

## Phase 2: Foundational ⏳ (~3h)

⚠️ CRITICAL: the setting must exist in the core before either shell can send it.

- [x] T006 [P] Tests first in `crates/telaviva-core/tests/core_contract.rs`: `inference_device` defaults to `auto`; accepts `auto`, `cpu`, `ane`, `gpu`, `gpu:1002:164e:0`; rejects empty, uppercase, `gpu:1:2:0`, `gpu:1002:164e`, a number, and a string over 32 characters; persists across reload; a schema-1 file without the key loads as `auto`. Confirm they fail.
- [x] T007 [P] Tests first in `crates/telaviva-core/tests/ffi_contract.rs`: an `update_settings` command with `inference_device` round-trips through `tv_submit` and `tv_snapshot`; an invalid value returns the error and leaves the file untouched.
- [x] T008 Add `inference_device: String` to `Settings` (default `"auto"`, `#[serde(default)]`) in `crates/telaviva-core/src/settings.rs`, validate its shape in `validate()`, and add the `"inference_device"` arm to `update_settings` in `crates/telaviva-core/src/core.rs`.
- [x] T009 Run `cargo test -p telaviva-core` and `cargo clippy -p telaviva-core`; fix until green.

**Checkpoint**: ✋ Core accepts and persists the setting.

## Phase 3: Group A, Windows engine (US-001, US-003, US-006) ⏳ 🎯 MVP (~12h)

**Goal**: on Windows, Automatic runs detection on the iGPU when it validates and on the CPU otherwise; an explicit device or the CPU can be chosen by editing the setting; every failure ends on the CPU without losing a frame.
**Independent Test**: set `inference_device` in the settings file to `auto`, `cpu` and `gpu:<vendor>:<device>:0` on the dev desktop and check the log and the per-GPU utilisation; unplugging is simulated by an id that is not present.

### Tests first

- [x] T010 [P] [US-001] Add `native/windows/Tests/device_selection.cpp` (new, with a scripted catalog) covering: id format and parse round-trip, ordinal for two identical GPUs, device option list order (Automatic, GPUs, CPU), and every resolution row of the contracts.md table (integrated present, only discrete, `unknown` kind, software only, marker present, explicit present, explicit missing, `cpu`). Confirm they fail.
- [x] T011 [P] [US-001] In `native/windows/Tests/adapter_catalog.cpp`, add tests for the pure `classifyKinds(dxgiAdapters, dxcoreAdapters)` function: LUID match sets `integrated` or `discrete`, no match or no DXCore sets `unknown`, software stays out.
- [x] T012 [P] [US-001] In `native/windows/Tests/provider_selection.cpp`, replace the "slower than CPU" test with: a slower-than-CPU candidate within budget is accepted; a candidate over the budget returns `SelfTestSlow`; a non-`present` result returns `SelfTestMismatch`.
- [x] T013 [P] [US-003] [US-006] In `native/windows/Tests/inference_host.cpp`, add tests for `setPreference` (drops the session and re-decides), report states `ok`, `auto_cpu`, `unavailable`, `failed`, explicit failure falls back without a marker, Automatic failure writes a marker and the next launch skips the device, a marker is ignored for an explicit choice, a runtime error retries the frame on the CPU, and the idle recreation skips the self-test. Remove the profile-driven cases that no longer apply.
- [x] T014 [P] [US-003] In `native/windows/Tests/json_codec.cpp` and `settings_payload.cpp`, add tests that `inference_device` parses with the default `auto` when missing and is included in the payload.

### Implementation

- [x] T015 [US-001] Extend `native/windows/Platform/AdapterCatalog.h/.cpp`: add `luid` and `kind` to `AdapterInfo`, read `AdapterLuid` in `DxgiAdapterCatalog`, load `dxcore.dll` at runtime, read `IsIntegrated` and `InstanceLuid`, implement `classifyKinds`. A failure to load or query yields `unknown`; never create a D3D12 or DirectML device.
- [x] T016 [US-001] [US-003] Add `native/windows/Platform/DeviceSelection.h/.cpp` (and the project items in `Platform.vcxproj` and `Tests.vcxproj`): device id format, option list builder, and the resolution function from the contracts.md table.
- [x] T017 [US-001] Update `judgeSelfTest` in `native/windows/Platform/ProviderSelection.h/.cpp` to use the named latency-budget constant instead of "accelerated strictly faster".
- [x] T018 [US-001] [US-003] [US-006] Rework `native/windows/Platform/InferenceHost.h/.cpp`: one runtime path (drop `directmlRuntimePath` and `boundRuntime_`), drop the profile match, `decideLocked` uses `DeviceSelection`, add `setPreference(id)` and `report()`, validate with the budget, write markers for Automatic only, keep the CPU retry for a failed frame.
- [x] T019 [P] [US-003] Add `inference_device` to `SettingsSnapshot` in `native/windows/Platform/JsonCodec.h/.cpp`, and to `SettingsDraft`, `draftFromSnapshotSettings`, `clampToCoreRanges` and `buildSettingsPayload` in `native/windows/Platform/SettingsModel.h/.cpp`.
- [x] T020 [US-003] In `native/windows/TelaViva/Host/AppController.cpp`, build `InferenceHost` without profiles and with the single runtime, pass the live adapter catalog, and call `setPreference` from the settings sink whenever the value changes (and once at startup).
- [x] T021 [US-001] Packaging: change `packaging/windows/build-app.ps1` step 8 to copy `tools/inference/runtime/windows-x64-directml/onnxruntime.dll` as `onnxruntime.dll`, make `tools/inference/fetch-windows-runtime.ps1` fetch that variant for packaging, and make `packaging/windows/Install-TelaVivaOLED.ps1` require `onnxruntime.dll`. `DirectML.dll` stays excluded.
- [x] T022 Build and run `scripts\build-windows.ps1`, then `native\windows\build\x64\Release\tests.exe`; fix until green.
- [x] T023 [US-001] Hardware record on the dev desktop in `validation/windows-auto-igpu.md`: Automatic selects the Radeon iGPU, per-LUID utilisation shows the iGPU (not the RTX) doing the work, detection results match the CPU, idle CPU and memory against the previous baseline (SC-005), `cpu` and `gpu:<RTX>` explicit choices, a missing id, and a forced failure.

**Checkpoint**: ✋ Windows engine complete and independently testable without a UI.

## Phase 4: Group B, Windows Advanced page (US-002, US-005) ⏳ (~6h)

**Goal**: the Advanced page lists the devices, changes the setting, shows the device in use and the fallback notice.
**Independent Test**: open Settings > Advanced, pick each entry, watch the in-use line and the notice change.

### Tests first

- [x] T024 [P] [US-002] [US-005] Add the new string keys (below) to both `Resources.resw` files only after extending `native/windows/Tests/strings_keys.cpp` with the expected-key list for the page; confirm the parity test fails until both files carry them.
- [x] T025 [P] [US-002] In `native/windows/Tests/device_selection.cpp`, add tests for the label builder (integrated and discrete notes, unknown kind, long descriptions) and for the report-to-notice mapping (`unavailable` and `failed` each map to a notice key; `ok` and `auto_cpu` map to none).

### Implementation

- [x] T026 [P] [US-002] [US-005] Add strings to `native/windows/TelaViva/Strings/en-US/Resources.resw` and `pt-BR/Resources.resw`: `settings.tab.advanced`, `inference.title`, `inference.help`, `inference.help.discrete`, `inference.auto`, `inference.cpu`, `inference.kind.integrated`, `inference.kind.discrete`, `inference.inUse`, `inference.autoCpu`, `inference.notice.unavailable`, `inference.notice.failed`.
- [x] T027 [US-002] [US-005] Extend `native/windows/TelaViva/Host/SettingsHost.h` with `inferenceDevices()` and `inferenceReport()`, and implement them in `AppController.cpp` from the adapter catalog and `InferenceHost::report()`.
- [x] T028 [US-002] [US-005] Create `native/windows/TelaViva/AdvancedPage.xaml`, `AdvancedPage.xaml.cpp`, `AdvancedPage.h` (and the project items): a ComboBox built inline, bound through `edit(...)` and `buildSettingsPayload`, an "in use" line, and an InfoBar for the notice, refreshed by the 1 s timer like the other pages.
- [x] T029 [US-002] Add the Advanced `NavigationViewItem` and page selection to `native/windows/TelaViva/SettingsWindow.xaml` and `SettingsWindow.xaml.cpp`, including `SetHost` fan-out and `RefreshStrings`.
- [ ] T030 Build, run `tests.exe`, then check the page with a UI Automation dump (a locked session gives black screen captures): list order, selection persists, in-use line, both notices, en-US and pt-BR.

**Checkpoint**: ✋ Windows feature complete (stories US-001, US-002, US-003, US-005, US-006 on Windows).

## Phase 5: Group C, macOS (US-004 and macOS parity) ⏳ (~12h) **(Mac, gated by T005)**

**Goal**: the same setting and page on macOS, with Automatic, Neural Engine, GPU and CPU.
**Independent Test**: on an Apple Silicon Mac, pick each entry; the in-use line and the CoreML log show the requested compute units, and a forced failure ends on the CPU.

### Tests first

- [ ] T031 [P] [US-004] In `crates/telaviva-inference/src/lib.rs` tests, add a pure mapping test `coreml_compute_units(device_index)`: 0 `ALL`, 1 `CPUAndNeuralEngine`, 2 `CPUAndGPU`, anything else invalid; and that provider 1 with an invalid index returns `-1`. Confirm they fail.
- [ ] T032 [P] [US-004] Add `native/macos/Tests/InferenceDeviceSelectionTests.swift` and `tools/tests/test-inference-device-selection.sh` with the same resolution vectors as T010 for the macOS rows (Automatic unvalidated resolves to CPU, validated resolves to `ALL`, `ane`, `gpu`, `cpu`, an id that does not apply, a failed candidate, the report states).
- [ ] T033 [P] [US-004] In `native/macos/Tests/CoreBridgeTests.swift`, add tests that `inference_device` decodes with the default `auto` when missing and is emitted by `CoreSettings.json`.

### Implementation

- [ ] T034 [US-004] Pin the ONNX Runtime 1.22.x macOS archive by SHA-256 in `assets/model/manifest.toml` and update `tools/inference/fetch-macos-runtime.sh` and `packaging/macos/build-app.sh` (signing of `Contents/Frameworks/*.dylib`) accordingly.
- [ ] T035 [US-004] In `crates/telaviva-inference/src/lib.rs`, replace the macOS branch of `attach_accelerator` with registration through `SessionOptionsAppendExecutionProvider("CoreML", ...)` and `MLComputeUnits` from the `device_index` mapping, with error-on-failure semantics so `selected_provider` is 0 when registration fails. Document the `device_index` meaning in `crates/telaviva-inference/include/telaviva_inference.h` and keep `header_contract_tests.rs` green.
- [ ] T036 [US-004] Add `native/macos/TelaViva/InferenceDeviceSelection.swift`: option list, resolution rules from contracts.md, the report, and the constant that keeps macOS Automatic on the CPU until a Mac validation record exists.
- [ ] T037 [US-004] Bundle the self-test frame in the macOS app resources (`packaging/macos/build-app.sh` and the Xcode project) from the same source file Windows uses.
- [ ] T038 [US-003] [US-004] [US-006] In `native/macos/TelaViva/PresenceCamera.swift`, add `setInferenceDevice(_:)` that releases the detector and records the preference; `ensureDetector()` calls `tv_inference_create_with_provider_device` with the resolved provider and compute units, validates against a CPU reference session with the self-test frame and the latency budget, and falls back to the CPU on any failure while keeping the frame retry on the CPU.
- [ ] T039 [P] [US-003] Add `inference_device` to `CoreSettings` and its `json` in `native/macos/TelaViva/CoreBridge.swift` (optional so older snapshots decode).
- [ ] T040 [P] [US-002] [US-005] Add the strings to `native/macos/TelaViva/Localization.swift` for both locales with the same keys as the Windows set (the existing parity test keeps them aligned).
- [ ] T041 [US-002] [US-005] Add `case advanced` in `native/macos/TelaViva/SettingsNavigation.swift`, an `advancedTab` with a Picker, the in-use line and the notice in `SettingsView.swift` (state, `load()`, `save()`, `.onChange` as the locale picker does), and wire the setting change and the status object to `PresenceCamera` from where the app owns it.
- [ ] T042 Run `cargo test -p telaviva-inference`, the swiftc test scripts under `tools/tests/`, and `scripts/build-macos.sh`; fix until green.
- [ ] T043 **(Mac)** Hardware record in `validation/macos-coreml.md`: each entry creates a session with the right compute units, correctness matches the CPU, failure fallback, the CoreML placement log. Only after it passes, flip the Automatic constant from T036 and record that in the file.

**Checkpoint**: ✋ macOS parity complete.

## Phase 6: Retire the profile allowlist ⏳ (~3h)

Own phase so the large deletion reviews separately (Decision 7). Windows only.

- [x] T044 Delete `native/windows/Platform/AccelerationProfiles.h/.cpp`, `native/windows/Tests/acceleration_profiles.cpp` and `assets/inference/acceleration-profiles.toml`, and remove them from `Platform.vcxproj`, `Tests.vcxproj`, `packaging/windows/build-app.ps1` and `Install-TelaVivaOLED.ps1`.
- [x] T045 Remove the leftover profile-loading and profile-related fields in `native/windows/TelaViva/Host/AppController.cpp` and `InferenceHost.h`, and any references in `tools/` scripts or `.github/workflows/windows.yml`.
- [x] T046 Build and run `scripts\build-windows.ps1` plus `tests.exe`; confirm the package no longer contains the profiles file and still starts.

## Final Phase: Polish ⏳ (~4h)

- [x] T047 [P] Update `api.md`: the inference C ABI additions (the `device_index` meaning per provider, the error-on-failure rule) and the `inference_device` setting.
- [x] T048 [P] Update `docs/user-guide.md`: the Advanced page in the Settings section, the replaced "Acceleration" bullet under Windows specifics, the macOS choices, and the preference caveat for CoreML.
- [x] T049 [P] Update `ARCHITECTURE.md` line 31 and the inference sections that still describe the CPU-by-default allowlist, and add the Unreleased entries to `CHANGELOG.md`.
- [ ] T050 Run the full checks: `cargo test --workspace`, `cargo clippy --workspace`, the Windows build and `tests.exe`, the macOS scripts, and `cargo test -p telaviva-core --test version_consistency`.
- [ ] T051 Run `/codeadvisor-dev:finalize` to write ADR-0027 (amending ADR-0001 and ADR-0019), update their status lines, and archive this spec folder. The ADR must exist before merge.
- [x] T052 Bump the application version per AGENTS.md "Release version". The owner chose `0.1.1` (not the proposed 0.2.0), done on 2026-10-18. Change `crates/telaviva-core/Cargo.toml` (source of truth), `crates/telaviva-inference/Cargo.toml`, `native/macos/TelaViva/Info.plist` (`CFBundleShortVersionString` and `CFBundleVersion`) and `native/windows/TelaViva/app.manifest` (`0.1.1.0`); let cargo update `Cargo.lock`; in `CHANGELOG.md` rename `Unreleased` to `0.1.1 - <date>` and open a new empty `Unreleased`; run `cargo test -p telaviva-core --test version_consistency`. Creating the tag `v0.1.1` and publishing the GitHub Release are outward-facing and wait for the owner's explicit go-ahead.

## Dependencies & Execution Order

```
Phase 1 (spikes) ──► Phase 2 (core setting) ──► Phase 3 (Windows engine) ──► Phase 4 (Windows page) ──► Phase 6 (retire allowlist) ──► Polish
        │                         │
        └─ T004/T005 gate ────────┴──► Phase 5 (macOS), needs a Mac
```

- Phase 2 blocks Phases 3, 4 and 5 (both shells need the core to accept the key).
- Phase 3 needs T002 and T003 (LUID match, index mapping, budget). Phase 4 needs Phase 3.
- Phase 5 needs T004 and T005, and can run in parallel with Phases 3 and 4 because it shares only the core setting and the resolution vectors.
- Phase 6 follows Phase 4 so the allowlist stays until the replacement works.
- Within phases, tasks marked [P] touch different files and can run together. In Phase 3, T015, T016 and T017 come before T018, and T018 before T020.

## Implementation Strategy

- **MVP**: Phases 1 to 3 (about 20h). It gives Windows the Automatic iGPU offload and the manual choice (through the settings file), with hardware evidence. It is not shippable alone: AGENTS.md requires macOS in the same change and the page for users to see the setting.
- **Shippable change**: Phases 1 to 6 and Polish (about 45h, assuming the macOS spike passes).
- **Parallel work**: Windows (Phases 3, 4) and macOS (Phase 5) in parallel once Phase 2 is done and T005 passes. The writing tasks inside a phase can go to separate workers when they touch different files; tasks in the same file go to one worker.
- **Delegation**: per the owner's standing rules, Haiku workers apply the edits with mechanical briefings (exact files, before and after, project conventions: English comments, strings in both locales); the coordinator runs every build, test and lint, reviews each diff and checks worker reports.
- **Hardware limits**: the validation records are the evidence. Do not claim energy savings (FR-013), and do not enable macOS Automatic on CoreML before T043.

## Progress Tracking

⏳ Not Started · ⏰ In Progress · ✅ Completed. Update the status of each phase and its Notes as work advances.

| Phase | Status | Notes |
|---|---|---|
| 1 Setup and spikes | ⏰ | T001-T003 done on the Windows dev desktop (records in validation/); T004/T005 need a Mac |
| 2 Foundational | ✅ | Core setting, tests and clippy green |
| 3 Group A, Windows engine (MVP) | ✅ | 651 Windows tests pass; hardware record validation/windows-auto-igpu.md. Latency budget set to 125 ms (0.25 s minimum return interval, not 1 s) |
| 4 Group B, Windows page | ⏰ | Page, strings and navigation built and tested; T030 (UI Automation check) open because an installed 0.1.0 instance was already running |
| 5 Group C, macOS | ⏳ | Needs an Apple Silicon Mac; not started (T004 spike first) |
| 6 Retire allowlist | ✅ | Files removed with git rm; Windows build and 651 tests pass; package has no profiles file |
| Polish | ⏰ | api.md, user-guide, ARCHITECTURE, CHANGELOG, README, provenance docs done for Windows; macOS text, T050 (macOS scripts), T051 finalize and T052 version bump open |
