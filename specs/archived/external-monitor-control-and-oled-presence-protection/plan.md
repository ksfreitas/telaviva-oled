# Tasks: External Monitor Control and OLED Presence Protection

**Branch**: `feat/oled-monitor-protection` **Specs**: [spec.md](spec.md) **Architecture**: [architecture.md](architecture.md) **Status**: In Progress ⏰ — macOS implementation; Windows deferred by the user; macOS scope implemented, physical acceptance and measurements pending

Implementation started on 2026-10-16. The user narrowed this execution to macOS; Windows-native tasks and physical Windows validation remain deferred and unchecked. The shared Rust core stays portable. This plan follows the native-shell design: a shared Rust core static library and testable rlib, with SwiftUI/AppKit on macOS and C++/WinRT/WinUI 3 plus the Win32 tray shell on Windows. Rust owns policy, state, timers, and workers; native shells own UI and main-thread integration. The core exposes a bounded typed C ABI through `include/telaviva_core.h`; Rust panics never unwind across it. Locales use Fluent messages formatted into snapshots for native presentation.

## Deviations

On 2026-10-16 the user narrowed execution to macOS, and Windows implementation and validation move to a separate, later stage. The Windows tasks in the phases below stay unchecked because they are not done. They are intentionally deferred, not abandoned, and the plan is complete for the macOS scope.

**Windows-only tasks**:

| Task | What is deferred |
|---|---|
| T015 | Windows physical-monitor VCP read-only spike |
| T017 | Windows shared-camera acquisition and permission spike |
| T019 | Windows screensaver startup and transition probe |
| T031 | Windows physical-monitor VCP DDC adapter |
| T033 | Windows monitor capability and control checks |
| T048 | WinUI 3 project with `App.xaml` bootstrap |
| T049 | C++/WinRT core bridge with ABI-safe buffers |
| T050 | Win32 notification-area icon and popup activation |
| T051 | WinUI monitor popup with per-monitor controls |
| T052 | WinUI settings window for preferences |
| T053 | WinUI project linking and camera declaration |
| T055 | Popup dismissal, tray activation, settings navigation, quit |
| T062 | Windows launch-at-login registration and removal |
| T074 | Windows shared-camera adapter and permission status |
| T079 | Windows 11 camera-sharing verification with another app |
| T090 | Windows battery and power observation |
| T098 | Windows screensaver observation implementation |
| T106 | Optional OpenVINO integration on supported Intel hardware |
| T117 | Windows 11 x64 WinUI packaging with camera access |
| T120 | Windows clean-account install and launch-at-login checks |

**Mixed tasks** (only the Windows half is deferred):

| Task | Deferred Windows half |
|---|---|
| T003 | Windows Rust library build script in `native/windows/` |
| T005 | Windows build check in CI workflows |
| T013 | Visual Studio project shell in `native/windows/` |
| T022 | Windows ABI smoke client and lifecycle verification |
| T060 | WinUI and platform redistribution terms documentation |

**Handoff**: When the Windows stage starts, it should reuse the portable Rust core and its C ABI unchanged, and it must open its own plan or reopen these tasks.

## Phase 1: Setup ⏰ (~2h)

- [x] T001 Record the supported developer toolchains and target triples in `README.md`.
- [x] T002 Create the Cargo workspace and `crates/telaviva-core/Cargo.toml` with `staticlib` and `rlib` outputs.
- [ ] T003 Add platform-specific build scripts for the Rust library to `native/macos/` and `native/windows/`.
- [x] T004 Create `include/telaviva_core.h` with ABI versioning and opaque handle declarations.
- [ ] T005 Add CI jobs for Rust formatting, linting, unit tests, and macOS/Windows build checks in `.github/workflows/`.

**Checkpoint**: ✋ Rust toolchains build the shared library; native linking is verified in the feasibility phase.
**Notes**: README, Cargo workspace with core/inference crates, and C headers exist. macOS CI checks were added. Rust 1.99.1, Swift 6.4 and Xcode 27.0 are available on the arm64 host; Windows work is deferred. Native integration/build verification is in progress.

## Phase 2: Foundational Core ⏳ (~3h)
⚠️ CRITICAL: generic ABI, runtime, and adapter boundaries block all stories.

- [x] T006 Define ABI-safe status, error, command, snapshot, and buffer ownership types for `tv_abi_version`, `tv_create`, `tv_submit`, `tv_snapshot`, `tv_buffer_free`, `tv_shutdown`, and `tv_destroy` in `crates/telaviva-core/src/ffi/`.
- [ ] T007 Add ABI boundary/lifecycle checks that verify invalid UTF-8, bounds, version mismatch, panic containment, and create/destroy behavior in `crates/telaviva-core/tests/ffi_contract.rs`.
- [x] T008 Implement the versioned C ABI in `crates/telaviva-core/src/ffi/`; accept UTF-8 JSON commands up to 64 KiB, validate into typed Rust commands, cap snapshots at 8 MiB, and never unwind panics across the boundary.
- [x] T009 Implement opaque core lifecycle, revision-only ready callbacks, main-thread callback dispatch, and shutdown lifetime draining in `crates/telaviva-core/src/ffi/`.
- [ ] T010 Add ABI ownership and lifecycle examples to `include/telaviva_core.h`.
- [ ] T011 Define bounded worker, shutdown, command-generation, and snapshot-revision interfaces in `crates/telaviva-core/src/runtime/`.
- [ ] T012 Define platform adapter interfaces for DDC, camera, power, screensaver, and settings in `crates/telaviva-core/src/ports/`.

**Checkpoint**: ✋ Foundation ready when ABI contract checks pass and shared runtime/port boundaries compile.
**Notes**: Implementation evidence is tracked below; unchecked native and physical acceptance tasks remain open.

## Phase 3: Platform Feasibility Spikes (US-001, US-003, US-004) ⏳ (~4h)

- [ ] T013 Create the native Xcode and Visual Studio project shells in `native/macos/TelaViva.xcodeproj/project.pbxproj` and `native/windows/TelaViva/TelaViva.vcxproj`.
- [x] T014 [P] [US-001] Build macOS Apple Silicon DDC read-only discovery/capability spike in `native/macos/ddc_bridge.m`.
- [ ] T015 [P] [US-001] Build Windows 11 x64 physical monitor/VCP read-only spike in `native/windows/TelaViva/DdcSpike.cpp`.
- [ ] T016 [P] [US-003] Verify shared-camera acquisition and permission behavior on Apple Silicon/macOS in `native/macos/TelaViva/CameraSpike.swift`.
- [ ] T017 [P] [US-003] Verify shared-camera acquisition and permission behavior on Windows 11 in `native/windows/TelaViva/CameraSpike.cpp`.
- [ ] T018 [P] [US-004] Probe macOS screensaver start/stop and startup-active behavior without substituting lock or display sleep in `native/macos/TelaViva/ScreensaverSpike.swift`.
- [ ] T019 [P] [US-004] Probe Windows screensaver startup and transitions in `native/windows/TelaViva/ScreensaverSpike.cpp`.
- [x] T020 [US-001] [US-003] [US-004] Record spike OS builds, device models, outcomes, and limitations in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/feasibility.md`.
- [x] T021 [US-001] [US-003] [US-004] Preserve unavailable behavior and document a design revision if a capability spike fails in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/feasibility.md`.

- [ ] T022 Add ABI smoke clients to the created native projects and verify `tv_create`/`tv_snapshot`/`tv_destroy` on both targets in `native/macos/TelaViva/` and `native/windows/TelaViva/`.

**Checkpoint**: ✋ ABI smoke link and DDC spike are complete before manual control begins. Camera and screensaver spikes may finish independently; unsupported results constrain only their own features.
**Notes**: Pending.

## Phase 4: Native Manual-Control MVP (US-001, US-006, US-007) ⏳ 🎯 MVP (~15h)
**Goal**: deliver native-feeling tray/menu-bar controls, per-monitor DDC status, settings, persistence, and both initial locales. **Independent Test**: On each OS, launch the tray app, enumerate connected external monitors, adjust supported brightness/volume independently, change settings/language, restart, and inspect accurate failures for unsupported controls.

### Tasks
- [x] T023 [US-001] Define monitor identity, raw/normalized values, capability state, requested state, and observed state in `crates/telaviva-core/src/monitor/`.
- [x] T024 [US-001] Add monitor discovery and capability snapshot commands to `crates/telaviva-core/src/commands/`.
- [x] T025 [US-001] Add failing fake-adapter contract checks for identity ambiguity, two monitors, independent capabilities, and raw-range conversion in `crates/telaviva-core/tests/monitor_contract.rs`.
- [x] T026 [US-001] Implement stable EDID-plus-connection-path identity checks and ambiguity rejection in `crates/telaviva-core/src/monitor/identity.rs`.
- [x] T027 [US-001] Implement normalized-to-raw brightness and volume mapping with range and resolution validation in `crates/telaviva-core/src/monitor/mapping.rs`.
- [x] T028 [US-001] Implement serialized, capped per-display DDC work queues with stale-generation rejection in `crates/telaviva-core/src/monitor/queue.rs`.
- [x] T029 [US-001] Implement bounded retry policy and typed DDC failure snapshots in `crates/telaviva-core/src/monitor/`.
- [x] T030 [US-001] Implement the macOS DDC adapter using the isolated IOAVService bridge in `crates/telaviva-core/src/platform/macos/ddc.rs` and `native/macos/ddc_bridge.{h,m}`.
- [ ] T031 [US-001] Implement the Windows physical-monitor/VCP adapter in `crates/telaviva-core/src/platform/windows/ddc.rs`.
- [x] T032 [US-001] Add macOS monitor capability and control integration checks in `crates/telaviva-core/tests/`.
- [ ] T033 [US-001] Add Windows monitor capability and control integration checks in `crates/telaviva-core/tests/`.
- [x] T034 [US-007] Add failing settings checks for defaults, bounds, corrupt-file recovery, atomic replacement, and future-schema rejection in `crates/telaviva-core/tests/settings_contract.rs`.
- [x] T035 [US-007] Implement versioned preference schema validation and defaults in `crates/telaviva-core/src/config/schema.rs`.
- [x] T036 [US-007] Implement atomic settings replacement, corrupt-file recovery, and future-schema rejection in `crates/telaviva-core/src/config/store.rs`.
- [ ] T037 [US-007] Implement Fluent key loading, locale negotiation, en-US fallback, and snapshot formatting in `crates/telaviva-core/src/i18n.rs`.
- [x] T038 [US-007] Add English messages for controls, settings, errors, status, and accessibility labels in `locales/en-US.ftl`.
- [x] T039 [US-007] Add equivalent Brazilian Portuguese messages in `locales/pt-BR.ftl`.
- [x] T040 [US-007] Check locale key parity and format every message used by the MVP in `crates/telaviva-core/`.
- [x] T041 [US-007] Implement revisioned UI snapshots and typed commands for monitor controls and settings in `crates/telaviva-core/src/ffi/`; native shells copy bounded snapshots on revision-only ready callbacks and release buffers with `tv_buffer_free` in `crates/telaviva-core/src/ffi/` and `native/{macos,windows}/TelaViva/CoreBridge`.
- [x] T042 [US-007] Extend the existing macOS Xcode project with the SwiftUI/AppKit app entry in `native/macos/TelaViva/`.
- [x] T043 [US-007] Implement `CoreBridge.swift` with safe UTF-8 conversion, copied snapshots, and typed command wrappers in `native/macos/TelaViva/CoreBridge.swift`.
- [x] T044 [US-007] Implement `MenuBarController.swift` using `NSStatusItem` and `NSPopover` in `native/macos/TelaViva/MenuBarController.swift`.
- [x] T045 [US-007] Implement `MonitorPopup.swift` with per-monitor brightness/volume controls and capability status in `native/macos/TelaViva/MonitorPopup.swift`.
- [x] T046 [US-007] Implement `SettingsView.swift` with locale, presence, saver, battery, and launch-at-login preferences in `native/macos/TelaViva/SettingsView.swift`.
- [x] T047 [US-007] Add the macOS DDC bridge build/link configuration and required privacy usage strings in `native/macos/TelaViva.xcodeproj/project.pbxproj` and `native/macos/TelaViva/Info.plist`.
- [ ] T048 [US-007] Extend the existing Windows WinUI 3/C++/WinRT project with the `App.xaml` bootstrap in `native/windows/TelaViva/`.
- [ ] T049 [US-007] Implement `CoreBridge.cpp` with ABI-safe buffers, UTF-8 conversion, and typed command wrappers in `native/windows/TelaViva/CoreBridge.cpp`.
- [ ] T050 [US-007] Implement Win32 notification-area icon ownership and popup activation in `native/windows/TelaViva/TrayIcon.cpp`.
- [ ] T051 [US-007] Implement `MonitorPopup.xaml` with per-monitor brightness/volume controls and truthful capability status in `native/windows/TelaViva/MonitorPopup.xaml`.
- [ ] T052 [US-007] Implement `SettingsWindow.xaml` with locale, presence, saver, battery, and launch-at-login preferences in `native/windows/TelaViva/SettingsWindow.xaml`.
- [ ] T053 [US-007] Add WinUI project linking and camera access declaration in `native/windows/TelaViva/TelaViva.vcxproj` and `native/windows/TelaViva/Package.appxmanifest`.
- [x] T054 [US-007] Implement macOS popup dismissal, one-click tray behavior, settings navigation, and quit handling in `native/macos/TelaViva/MenuBarController.swift`.
- [ ] T055 [US-007] Implement Windows popup dismissal, tray activation, settings navigation, and quit handling in `native/windows/TelaViva/TrayIcon.cpp`.
- [ ] T056 [US-007] Verify both native shells acknowledge input without waiting on DDC completion and refresh from snapshots. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/results.md`.
- [ ] T057 [US-001] [US-007] Verify monitor selection changes only the selected display on two supported physical monitors per OS where available. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/results.md`.
- [ ] T058 [US-007] Verify settings survive restart and all visible MVP strings switch between en-US and pt-BR. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/results.md`.
- [x] T059 [US-006] Add per-OS dependency/model provenance manifests and license notices in `licenses/DEPENDENCIES.md` and `assets/model/manifest.toml`.
- [ ] T060 [US-006] Confirm Rust dependencies and redistributed assets are open source; document WinUI/platform redistribution terms before packaging in `licenses/DEPENDENCIES.md`.

- [x] T061 [US-007] Implement macOS launch-at-login registration and removal through the supported OS facility in `native/macos/TelaViva/LoginItemController.swift`.
- [ ] T062 [US-007] Implement Windows launch-at-login registration and removal through the supported OS facility in `native/windows/TelaViva/LaunchAtLogin.cpp`.

**Checkpoint**: ✋ Manual-control MVP works on each validated host. This does not claim automatic OLED protection is implemented or hardware support is universal.
**Notes**: Pending.

## Phase 5: Presence Capture and Detection (US-003, US-006) ⏳ (~10h)
**Goal**: add privacy-bounded local any-face detection, configurable intervals, camera status, and low-light handling. **Depends on**: Foundation and MVP ABI/snapshot path.

- [x] T063 [US-003] Define frame-free camera events and Present/Absent/InsufficientLight/Unavailable observations in `crates/telaviva-core/src/presence/`.
- [x] T064 [US-003] Define capture-session generation, sample deadlines, and settings-change rescheduling in `crates/telaviva-core/src/core/scheduler.rs`.
- [x] T065 [US-003] Add YuNet model digest, license, source, and redistribution record in `assets/model/manifest.toml`.
- [x] T066 [US-003] Add YuNet model fixtures and expected any-face results in `crates/telaviva-core/tests/fixtures/`.
- [x] T067 [US-003] Add detector checks for faces, empty scenes, stale observations, and low-light fixtures before wiring the runtime in `crates/telaviva-core/tests/detector_contract.rs`.
- [x] T068 [US-003] Implement model input normalization and output decoding with no identity output in `crates/telaviva-core/src/presence/detector.rs`.
- [x] T069 [US-003] Implement ONNX Runtime CPU provider initialization and inference lifecycle in `crates/telaviva-core/src/presence/provider.rs`.
- [x] T070 [US-003] Implement confidence, frame freshness, and usable-light classification in `crates/telaviva-core/src/presence/quality.rs`.
- [x] T071 [US-003] Implement one-frame-at-a-time acquisition and immediate frame release in `crates/telaviva-core/src/presence/worker.rs`.
- [ ] T072 [US-003] Add camera contention, permission denial, device removal, and no-frame-storage contract checks in `crates/telaviva-core/tests/camera_contract.rs`.
- [x] T073 [US-003] Implement the macOS AVFoundation shared-camera adapter and permission status in `crates/telaviva-core/src/platform/macos/camera.rs`.
- [ ] T074 [US-003] Implement the Windows shared-camera adapter and permission status in `crates/telaviva-core/src/platform/windows/camera.rs`.
- [x] T075 [US-003] Implement low-light policy: suspend face inference, run light-only observations every 10 seconds, and require two usable observations to resume in `crates/telaviva-core/src/presence/quality.rs`.
- [x] T076 [US-003] Ensure disabling presence or battery suspension stops all camera acquisition, including low-light recovery observations in `crates/telaviva-core/src/core/scheduler.rs`.
- [x] T077 [US-003] Wire presence enablement and validated interval settings through the shared ABI and native settings controls in `crates/telaviva-core/src/ffi/`.
- [ ] T078 [US-003] Verify camera sharing with another camera application on a supported macOS configuration and record exact hardware/OS details. `specs/archived/external-monitor-control-and-oled-presence-protection/validation/camera-sharing-macos.md`
- [ ] T079 [US-003] Verify camera sharing with another camera application on Windows 11 and record exact hardware/OS details. `specs/archived/external-monitor-control-and-oled-presence-protection/validation/camera-sharing-windows.md`

**Checkpoint**: ✋ Camera failures and low light suspend inference visibly; no camera frame enters ABI, state snapshots, settings, or logs.
**Notes**: Pending.

## Phase 6: Protection Policy and Battery (US-002, US-005) ⏳ (~11h)
**Goal**: apply presence dimming/restoration under conservative ownership, overlap, and battery rules. **Depends on**: presence observations and DDC snapshots.

- [x] T080 [US-002] Define protection episode, baseline ownership, active reasons, and manual override state in `crates/telaviva-core/src/core/state.rs`.
- [x] T081 [US-002] Add reducer checks for misses, return, overlap, manual override, disconnect, shutdown, and stale completions before implementation wiring in `crates/telaviva-core/tests/protection_contract.rs`.
- [x] T082 [US-002] Implement pure reducer transitions for two consecutive valid absences and a returning face in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T083 [US-002] Implement effective target selection as the minimum of active reason targets in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T084 [US-002] Implement fresh baseline capture only when monitor identity and brightness readback are valid in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T085 [US-002] Implement compare-before-restore ownership checks and invalidate baselines on disconnect/restart in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T086 [US-002] Implement manual adjustment precedence, episode suppression, and baseline update behavior in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T087 [US-002] Implement restore/recompute behavior for low light, camera failure, presence disablement, and normal shutdown in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T088 [US-002] Implement generation-aware reconciliation after stale or delayed native DDC completions in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T089 [US-005] Implement macOS battery/power observation behind `crates/telaviva-core/src/platform/macos/power.rs`.
- [ ] T090 [US-005] Implement Windows battery/power observation behind `crates/telaviva-core/src/platform/windows/power.rs`.
- [x] T091 [US-005] Implement battery suspension priority, cancellation of capture, presence-reason clearing, and post-AC rescheduling in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T092 [US-002] Expose protection reason, desired value, observed value, ownership, and status independently in UI snapshots in `crates/telaviva-core/src/ffi/snapshot.rs`.
- [x] T093 [US-005] Add en-US and pt-BR messages for presence, protection, restoration, battery, and recovery outcomes in `locales/en-US.ftl` and `locales/pt-BR.ftl`.
- [ ] T094 [US-005] Verify battery transitions stop capture immediately and preserve independent active dimming reasons on each OS. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/results.md`.

**Checkpoint**: ✋ Presence protection restores only values still owned by this app and preserves independent reasons across power transitions.
**Notes**: Pending.

## Phase 7: Screensaver Protection (US-004) ⏳ (~5h)
**Goal**: add a separate opt-in screensaver reason with configured target and startup reconciliation. **Depends on**: reducer and validated platform spikes.

- [x] T095 [US-004] Implement typed screensaver Unknown/Inactive/Active/Unavailable state in `crates/telaviva-core/src/platform/screensaver.rs`.
- [x] T096 [US-004] Add reducer checks for startup-active, overlap, target change, disablement, and unknown/unavailable states in `crates/telaviva-core/tests/screensaver_contract.rs`.
- [ ] T097 [US-004] Implement macOS screensaver observation using only the behavior validated in the feasibility spike in `crates/telaviva-core/src/platform/macos/screensaver.rs`.
- [ ] T098 [US-004] Implement Windows screensaver observation using only the behavior validated in the feasibility spike in `crates/telaviva-core/src/platform/windows/screensaver.rs`.
- [ ] T099 [US-004] Reconcile already-active screensaver state after registration without substituting lock or display sleep in `crates/telaviva-core/src/core/reducer.rs`.
- [x] T100 [US-004] Wire screensaver enablement and per-monitor 0–100 target through validated settings and native controls in `crates/telaviva-core/src/config/schema.rs`.
- [x] T101 [US-004] Add en-US/pt-BR text for unavailable screensaver observation and its current status in `locales/en-US.ftl` and `locales/pt-BR.ftl`.
- [ ] T102 [US-004] Verify actual screensaver start/stop and startup-active behavior on each supported OS; retain Unavailable if the gate fails. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/results.md`.

**Checkpoint**: ✋ Saver protection remains independent of camera and is enabled only where native behavior is validated.
**Notes**: Pending.

## Phase 8: Measured Acceleration and Resource Gates (US-006) ⏳ (~7h)
**Goal**: retain CPU fallback and enable acceleration only for measured lower-energy configurations. **Depends on**: stable detection and reference hardware.

- [x] T103 [US-006] Add a failing provider-selection contract check for missing/failed acceleration and CPU fallback in `crates/telaviva-core/tests/provider_contract.rs`.
- [x] T104 [US-006] Add explicit provider selection and capability/status reporting in `crates/telaviva-core/src/presence/provider.rs`.
- [x] T105 [US-006] Integrate macOS CoreML through the selected open-source runtime boundary without claiming OS frameworks are redistributed OSS. in `crates/telaviva-core/src/presence/provider.rs`.
- [ ] T106 [US-006] Add optional Windows OpenVINO direct C API integration only for supported Intel hardware and a license-reviewed package. in `crates/telaviva-core/src/presence/provider.rs`.
- [ ] T107 [US-006] Implement provider initialization failure and energy-profile miss fallback to CPU. in `crates/telaviva-core/src/presence/provider.rs`.
- [x] T108 [US-006] Add a reproducible release-build benchmark harness for CPU, memory, wakeups, camera duty, latency, and incremental energy. in `crates/telaviva-core/benches/resource_acceptance.rs`.
- [ ] T109 [US-006] Measure protections-off and battery-suspended states against ≤0.2% mean CPU and ≤100 MiB pre-inference residency. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.
- [ ] T110 [US-006] Measure present and absent states against ≤1% and ≤5% mean CPU respectively and ≤250 MiB active memory. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.
- [ ] T111 [US-006] Measure idle resource release after 30 seconds and verify steady residency ≤150 MiB. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.
- [ ] T112 [US-006] Compare three matched accelerated/CPU runs; enable only if energy falls by ≥10% without accuracy or latency regression. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.
- [ ] T113 [US-006] Measure return detection, UI acknowledgement, recall, false presence, and insufficient-light fixtures against architecture targets. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.
- [ ] T114 [US-006] Record device, OS, camera, connection, CPU/GPU/driver, model/runtime digests, provider, and raw benchmark results. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.
- [ ] T115 [US-006] Optimize any failed budget and repeat only the affected measurements; document any reviewed design change before claiming acceptance. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`.

**Checkpoint**: ✋ Unprofiled configurations use CPU; performance and detection claims are backed by recorded measurements.
**Notes**: Pending.

## Phase 9: Polish, Packaging, Compatibility, and Release Readiness ⏳ (~9h)

- [ ] T116 Package the signed/notarized arm64 macOS `.app` with camera usage description and required capture configuration. in `packaging/macos/README.md`.
- [ ] T117 Package the Windows 11 x64 WinUI application/installer with camera access configured for the chosen packaging mode. in `packaging/windows/README.md`.
- [x] T118 Bundle and verify runtime/model digests, license notices, and provenance; prohibit runtime downloads and working-directory library loading. in `licenses/README.md` and `assets/model/manifest.toml`.
- [ ] T119 Verify clean-account installation, native library loading, camera permission prompts, and launch-at-login opt-in on macOS. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-macos.md`.
- [ ] T120 Verify clean-account installation, native library loading, camera permission prompts, and launch-at-login opt-in on Windows. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-windows.md`.
- [ ] T121 Run the compatibility matrix for supported/unsupported monitor brightness and volume across agreed cables, docks, and M1 paths. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/monitor-matrix.md`.
- [ ] T122 Verify two-monitor selection, popup behavior, high-DPI placement, settings persistence, and both locales on each OS. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/ui-matrix.md`.
- [ ] T123 Verify normal exit restoration, failed restore status, disconnect/reconnect baseline invalidation, and restart behavior. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/protection-lifecycle.md`.
- [ ] T124 Verify no frame/image data appears in logs, settings, crash metadata, ABI snapshots, or persisted state. in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/privacy.md`.
- [x] T125 Produce a release-readiness report listing verified OS/device combinations, unavailable features, and remaining gaps in `specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md`.
- [x] T126 Preserve final design rationale in `adr/0001-native-shell-and-rust-core.md` and update `ARCHITECTURE.md` and `README.md` to distinguish implemented features from hardware-verified support.

**Checkpoint**: ✋ Release claims match the measured compatibility matrix, package provenance, and platform validation evidence.
**Notes**: Pending.

## Dependencies & Execution Order

```text
Setup → Foundation core → Native project shells + ABI smoke link → DDC feasibility + manual-control core → Manual-control UI MVP
                                                                  ├→ Camera feasibility → Presence detection → Presence/battery policy
                                                                  ├→ Screensaver feasibility → Screensaver policy
                                                                  └───────────────────────────────────────────────→ Acceleration/resource gates
All shipped features → Polish, packaging, and release readiness
```

Only the shared ABI, native project shells, and DDC feasibility/integration block manual-control delivery. Camera and screensaver spikes gate only their respective capabilities; an unavailable result leaves that feature visibly unavailable and does not block manual monitor controls. These spike results remain unverified until their acceptance tasks are complete. The manual-control MVP may proceed after T013, the DDC spike tasks, and T022; it does not wait for camera or screensaver spikes. Presence detection also depends on settings, model provenance, and camera adapters. Protection policy depends on typed DDC and camera observations. Acceleration follows stable CPU inference and depends on integrated presence and protection behavior. Packaging and hardware acceptance depend on all included features. Tasks marked `[P]` touch independent platform spike files and may proceed concurrently within their phase; integration and tasks sharing files remain sequential. The plan assumes a macOS Apple Silicon development host for macOS packaging; Windows compilation and hardware acceptance require an available Windows 11 x64 host.

## Implementation Strategy

Deliver the ABI and DDC feasibility evidence first, then the smallest useful MVP: manual multi-monitor control, native tray/menu-bar UI, settings persistence, status, and pt-BR/en-US. This MVP does not include automatic protection. Add camera detection, reducer-controlled presence/battery behavior, and screensaver protection as independently reviewable increments. Treat performance, detection accuracy, sharing, DDC compatibility, and packaging as evidence gates, not assumed outcomes. Contract and policy checks are written before the implementations they verify and confirmed to fail first; physical hardware checks are acceptance tasks performed after adapters exist, never simulated as red tests.

## Estimates and Story Coverage

Estimates include implementation and local integration; reserve 25–50% planning contingency for cross-platform integration, hardware access, signing, and measured acceptance variability.

| Phase | Estimate |
|---|---:|
| Setup | 2h |
| Foundation core | 3h |
| Platform feasibility spikes | 4h |
| Native manual-control MVP (US-001, US-006, US-007) | 15h |
| Presence detection (US-003, US-006) | 10h |
| Presence policy and battery (US-002, US-005) | 11h |
| Screensaver (US-004) | 5h |
| Acceleration and resource gates (US-006) | 7h |
| Packaging and release readiness (US-001–US-007) | 9h |
| **Base effort total** | **66h** |
| Planning contingency (25–50%) | 17–33h |
| **Planning range** | **83–99h** |

The independently testable MVP is approximately **24h base effort** (about 30–36h with contingency), including setup, foundation core, native shell/ABI smoke link, DDC feasibility, and Phase 4 manual-control work. Tagged task counts: US-001 16, US-002 10, US-003 21, US-004 12, US-005 5, US-006 15, US-007 27. Cross-story tasks carry each applicable story tag. Total: 126 tasks.

## Progress Tracking

⏳ Not Started · ⏰ In Progress · ✅ Completed — update phase status and its Notes with evidence as work proceeds. A completed task means its code or validation artifact exists; it does not imply hardware-wide support.

## Current implementation evidence (2026-10-16)

The first macOS implementation consolidates shared policy, commands, and action scheduling in `crates/telaviva-core/src/core.rs`, settings in `settings.rs`, and ABI in `ffi.rs` rather than creating every planned submodule. Platform work runs in native Swift adapters through revisioned JSON snapshots and bounded pending actions; there is no separate Rust worker runtime. DDC is isolated in `native/macos/TelaViva/MacDDCController.swift` rather than the planned Objective-C bridge. YuNet preprocessing, decoding, and provider initialization live in the separate `telaviva-inference` crate. These path changes preserve the shared policy/native-adapter boundary.

Nineteen core tests and five inference tests currently pass, including real pinned ONNX Runtime/model execution on neutral and dark images. Workspace Clippy passes with warnings denied. Core coverage is 80.03% of lines and workspace Rust coverage is 83.48%, excluding native Swift and physical devices. See [coverage](validation/coverage.md), [test strategy](validation/test-strategy.md), and [results](validation/results.md). Integrated native compilation, Xcode build, strict ad-hoc signature verification, and ABI smoke checks pass. Physical read-only DDC brightness and volume reads succeed on the connected Dell; writes and broader hardware acceptance remain open.

Checkboxes distinguish implemented code from acceptance: a completed reducer or DDC adapter task does not establish monitor writes, real-camera recall, camera sharing, screen-saver startup reconciliation, or energy budgets. Mixed macOS/Windows tasks remain unchecked when only their macOS portion exists. Windows remains deferred. The static locale catalog currently supports the simple messages used by the UI; a full Fluent formatter and plural/interpolation support are not implemented, so T037 remains open.

### Phase notes after automated and native build checks

- **Setup/foundation:** Cargo workspace, bounded versioned ABI, buffer ownership and invalid-input tests compile; Rust CI includes lint/test/core coverage and a native arm64 cross-build artifact. CI configuration is present but has not executed remotely in this session. Native shutdown drains owned restoration actions with an eight-second deadline.
- **Feasibility/manual control:** Native shell, per-display controls/settings, login registration error handling, locale catalogs, DDC code, and Xcode/script linking exist. Initial native build and ABI smoke pass. The initial physical read-only probe exposed a platform topology mismatch; successful DDC writes/two-display independence are not established. T032 records integration tooling rather than successful physical control.
- **Presence/policy:** Actual pinned YuNet inference passes positive-face/neutral/dark fixtures; core tests include power suspend/resume, stale camera generations, ownership and low-light recovery. Native single-frame capture compiles, with no webcam access yet. Native detector release after 30 seconds outside face mode is implemented but not measured.
- **Screensaver:** Independent core policy and notification observation exist; startup remains unknown. Physical notification/startup-active acceptance remains unchecked.
- **Resources/packaging:** A process sampler lives in `tools/validation/sample_process.py` rather than a Rust bench. Pinned model/runtime digest checks and notices are bundled; local ad-hoc signature is verified. CPU fallback is operational; no energy benefit is claimed. Developer ID/notarization, clean-account install, UI interactions and hardware resource gates remain open.

A checked implementation/document task can still have an unchecked physical acceptance task. T007 covers malformed UTF-8/null/bounds/version/lifecycle; panic containment is source-guarded, without a forced panic integration fixture. T010 currently documents ownership rules in the C header rather than a standalone example client; the native smoke client provides executable lifecycle coverage.

### Read-only hardware milestone

T014 is verified on the M4 Pro/macOS 27.0.1 host with DELL S3225QC: brightness read 10/100 and volume read 0/100, both supported. Discovery was revised for the current IORegistry framebuffer/provider topology and preserves unsupported displays in the UI. See [monitor-matrix.md](validation/monitor-matrix.md). The macOS portion of T022 passes; the task stays unchecked because its Windows portion is deferred. T013 remains unchecked as a mixed two-platform task even though the macOS Xcode project builds. No monitor write/restoration or camera operation is implied by these checks.

### Local testing handoff

The latest-source arm64 Xcode build succeeds. The final optimized packaging bundle passes ABI smoke and strict ad-hoc signature verification, includes asset hashes/notices and localized camera descriptions, and performs successful read-only DDC probes on the connected Dell. A 60-second protections-off idle sample observed 84.31–84.42 MiB RSS and approximately 0.017% mean CPU; the shorter-than-acceptance run does not complete T109 or active-camera gates.

See [release-readiness.md](validation/release-readiness.md) for the concrete runnable artifact and remaining gates. The macOS implementation is available for local testing, while camera/UI interaction, physical writes/restoration, screensaver startup, energy and distribution acceptance remain open. This plan is not marked complete.
