# Tasks: Windows Version with Laptop Parity and Low-Energy Face Detection

**Branch**: `windows-version` **Specs**: [spec.md](spec.md) **Architecture**: [architecture.md](architecture.md) **Contracts**: [contracts.md](contracts.md) **Status**: Implemented in code and covered by automated tests; the manual, hardware and visual tasks remain ⏰ (see Implementation notes at the end)

Conventions: tasks marked **(manual)** need the user at the machine (hardware, visual review, Task Manager) and cannot be done by an agent alone; **(spike)** tasks end with a note under `specs/archived/windows-version/validation/` and a go or no-go. A test task comes before the implementation it checks and must be seen failing first (spec SC-005, SC-009, SC-010 require automated tests). All shell and platform code is C++20 under `native/windows/`; Rust changes stay additive. Phases 3 and 4 are the MVP.

Story groups: **G1** US-001 + US-011 (flyout) + US-009 (flyout strings); **G2** US-010 + US-009 + US-011 (Settings window); **G3** US-002 + US-003 (+ screensaver protection, FR-010); **G4** US-007; **G5** US-008; **G6** US-006; **G7** US-004 + US-005.

## Phase 1: Setup ✅ (~3h)

- [x] T001 [P] Create `native/windows/Directory.Build.props` (C++20, x64 only for now, warning level 4, treat warnings as errors in Platform, include dir `../../include`, no `timeBeginPeriod`-style globals) and `native/windows/nuget.config`.
- [x] T002 [P] Create `native/windows/Platform/Platform.vcxproj` as a static library with a placeholder `Platform/Platform.h`.
- [x] T003 [P] Create `native/windows/Tests/Tests.vcxproj` that builds every `Tests/*.cpp` into its own executable, plus `Tests/TestHarness.h` (assert macros, test registry, non-zero exit on failure, no third-party framework).
- [x] T004 Create `native/windows/TelaViva/TelaViva.vcxproj`: WinUI 3 unpackaged (`WindowsPackageType` None, `AppxPackage` false), `WindowsAppSDKSelfContained` true, hybrid CRT, `app.manifest` with per-monitor DPI v2 and no elevation, minimal `App` and `main.cpp`; pin Microsoft.WindowsAppSDK 2.5.x and Microsoft.Windows.CppWinRT in `packages.config` or project references.
- [x] T005 Create `native/windows/TelaViva.sln` referencing the three projects, with Tests and TelaViva referencing Platform.
- [x] T006 [P] Add `.gitignore` entries for `native/windows/**/x64/`, `.vs/`, `packages/`, `native/windows/build/`, `dist/`, and downloaded runtimes under `tools/inference/runtime/`.
- [x] T007 Run `msbuild native/windows/TelaViva.sln /p:Platform=x64 /p:Configuration=Release`; fix skeleton errors; record the exact toolchain versions (VS 17.14, MSVC 14.44, SDK 26100, WinAppSDK pin) in `specs/archived/windows-version/validation/toolchain.md`.

## Phase 2: Foundational ✅ (~14h)

⚠️ CRITICAL: blocks everything below. Includes spikes S1, S9, S10 that can stop or change the design.

Rust DLL build and exports (S1):
- [x] T008 [P] Write `tools/windows/build-rust-dlls.ps1`: puts `%USERPROFILE%\.cargo\bin` on PATH, runs `cargo rustc --release --lib --crate-type cdylib -p telaviva-core` and the same for `telaviva-inference`, copies the DLLs and import libs to `native/windows/build/rust/`.
- [x] T009 [P] Write `tools/windows/check-dll-exports.ps1`: locates `dumpbin` through `vswhere`, lists exports of both DLLs, parses function names from `include/telaviva_core.h` and `include/telaviva_inference.h`, fails on any missing or extra `tv_*` symbol.
- [x] T010 (spike) S1: run T008 and T009; write `validation/spike-s1-exports.md` with the export lists and the decision. If exports are missing, add a re-export shim or `.def` file as a fix and re-run before continuing.

Core bridge and common services (tests first):
- [x] T011 [P] Write failing `Tests/json_codec.cpp`: builds each command JSON in `contracts.md` (monitor_discovered, monitor_disconnected, monitor_display_state, power, screensaver, presence_observation, camera_status, builtin_display, set_brightness, set_volume, update_settings, hardware_result) and parses a recorded snapshot fixture `Tests/fixtures/snapshot-basic.json`.
- [x] T012 Implement `Platform/JsonCodec.h/.cpp` on `Windows.Data.Json`: command builders and `Snapshot` structs (settings, monitors, pending_actions, ambient, camera, power, screensaver); unknown or missing fields tolerated on read.
- [x] T013 [P] Write failing `Tests/core_bridge_smoke.cpp`: loads `telaviva_core.dll`, checks `tv_abi_version() == 1`, creates a core with a temp `config_path`, submits a `power` command, reads the snapshot revision, shuts down.
- [x] T014 Implement `Platform/CoreBridge.h/.cpp`: owns handles, a serialized worker queue, snapshot callback that only posts "revision changed", `refresh()`, sink subscription, `submit()`, `shutdown()` with pending-action drain (8 s deadline, 0.1 s poll).
- [x] T015 [P] Implement `Platform/IClock.h` and `SystemClock` (`steady_clock` seconds, epoch milliseconds) and `FakeClock` in `Tests/Fakes.h`.
- [x] T016 [P] Implement `Platform/Scheduler.h/.cpp` on thread-pool timers with a tolerable-delay window (10% of the delay, at most 1 s), cancel, and a `FakeScheduler` in `Tests/Fakes.h`.
- [x] T017 [P] Implement `Platform/AppPaths.h/.cpp` (`%LOCALAPPDATA%\TelaVivaOLED`, settings, calibration session and failure file paths, directory creation, test override).
- [x] T018 [P] Implement `Platform/Diagnostics.h/.cpp`: log file under `AppPaths`, errors by default, verbose by flag, never logs camera data or device identifiers.

App host:
- [x] T019 Implement `--smoke-test` in `TelaViva/main.cpp` before any XAML initialization: loads `telaviva_core.dll`, checks the ABI handshake, loads `telaviva_inference.dll`, prints a one-line result, returns a non-zero exit code on failure.
- [x] T020 Implement `TelaViva/PlatformWindow.h/.cpp`: hidden top-level Win32 window with a message-handler registry (power, session, display, device change, end session) and a named-mutex single instance that activates the first instance.
- [x] T021 Implement `TelaViva/TrayHost.h/.cpp`: `Shell_NotifyIcon` V4 with `NIF_GUID`, `TaskbarCreated` re-add, left click and keyboard events raised to the app, native context menu (Settings, Quit), icon chosen from the system theme.
- [x] T022 [P] Write `tools/windows/make-tray-icons.ps1` that draws the three presence states (dot, empty, ring) for light and dark at 16, 20, 24, 32 and 48 px with `System.Drawing` and writes `.ico` files under `native/windows/TelaViva/Assets/`. (The geometry comes from the existing `assets/branding/menubar/*.svg`.)
- [x] T022a [P] Write `tools/windows/make-app-icon.ps1` that builds the Windows application icon from the existing layered artwork (original, repository MIT license) as **the monitor only**: compose `assets/branding/liquid-glass/screen.svg`, `stand.svg` and `dot.svg` without `background.svg` and without the rounded-square tile or edge highlight of `app-icon.svg`, on a fully transparent canvas, cropped to the monitor's bounding box with a small margin so it fills the icon. Output a multi-size `AppIcon.ico` (16, 24, 32, 48, 64, 128 and 256 px, PNG-compressed entries) committed at `packaging/windows/Resources/AppIcon.ico`. Rasterize without a new dependency (Microsoft Edge headless screenshot with a transparent background, falling back to drawing the layer geometry with `System.Drawing`). Document the command in `assets/branding/README.md` next to the macOS regenerate steps, noting that the Windows icon is the monitor without the tile (owner decision, 2026-10-17).
- [x] T022b Embed the application icon in the executable: add `native/windows/TelaViva/TelaViva.rc` with an `ICON` resource that uses `packaging/windows/Resources/AppIcon.ico`, register it as `ResourceCompile` in `TelaViva.vcxproj`, and verify that `TelaViva.exe` shows the icon in Explorer and the taskbar.
- [x] T023 Implement `TelaViva/AppLifecycle.h/.cpp`: start order, stop order, `WM_QUERYENDSESSION` and `WM_ENDSESSION` handling that runs the same shutdown drain, and the quit path from the tray menu.

Strings and idle gate (S9, S10):
- [x] T024 [P] Write failing `Tests/strings_keys.cpp`: every key exists in both `Strings/en-US/Resources.resw` and `Strings/pt-BR/Resources.resw`, no value contains "MacBook" or "Mac", and no value is empty.
- [x] T025 Create `TelaViva/Strings/en-US/Resources.resw`, `Strings/pt-BR/Resources.resw` with the tray menu keys and implement `TelaViva/Strings.h/.cpp` (`Str(key)` using `ResourceManager` and a `ResourceContext` Language override from the app language setting).
- [x] T026 (spike) S9: verify in the built unpackaged app that `.resw` resources load in both languages and switch at runtime; if not, write `tools/windows/gen-strings.ps1` that generates a C++ string table from the `.resw` files and switch `Str()` to it; record in `validation/spike-s9-resources.md`.
- [x] T027 (spike, manual) S10: run the skeleton (tray icon, hidden window, XAML initialized, no windows open) for 5 minutes and record CPU, working set, GPU and thread count; decide go or fallback (new ADR for a Win32 resident agent) in `validation/spike-s10-idle.md`.

Build, CI and packaging skeleton:
- [x] T028 [P] Create `.github/workflows/windows.yml` with job `rust` (cache, `cargo fmt --check`, `cargo test --workspace --locked`, `cargo clippy -D warnings`) and job `native` (build DLLs, export check, `msbuild`, run every test executable, run `--smoke-test`).
- [x] T029 [P] Create `packaging/windows/build-app.ps1` that builds the DLLs, runs `msbuild`, and assembles `dist/TelaVivaOLED/` (app, DLLs, model, licenses) with a placeholder for the ONNX runtime.
- [x] T030 Run the whole local pipeline (T008, T009, msbuild, tests, `--smoke-test`) and fix failures; record the result in `validation/foundation.md`.

**Checkpoint**: ✋ Foundation ready: DLLs export correctly, the bridge talks to the core, the tray host and single instance run, strings load in both languages, idle cost recorded.

## Phase 3: G1 External monitor control and native flyout (US-001, US-011, US-009) (P1) ⏳ 🎯 MVP (~18h)

**Goal**: The tray flyout shows each external monitor and controls its brightness and volume through DDC with accurate unavailable states.
**Independent Test**: With one DDC monitor attached, move the brightness slider and see the readback-verified value; unplug it and see the card disappear; a non-DDC monitor shows an availability reason.

### Tasks

- [x] T031 [P] [US1] Write failing `Tests/monitor_identity.cpp` with EDID fixtures in `Tests/fixtures/` (normal serial, zero serial, placeholder serial, two identical serial-less monitors): expects the `win-<mfg>-<product>-<serial>-<hash8>` id, `stable` or `ambiguous`, and id length under 512 bytes.
- [x] T032 [US1] Implement `Platform/MonitorIdentity.h/.cpp` (EDID parsing, id, ambiguity rule).
- [x] T033 [US1] Implement `Platform/IDisplayCatalog.h` and `Platform/DisplayCatalog.h/.cpp`: `EnumDisplayMonitors`, GDI name to DisplayConfig source and target, registry EDID lookup, `GetPhysicalMonitorsFromHMONITOR` (more than one physical handle marks ambiguous), refresh with added and removed ids.
- [x] T034 (spike, manual) [US1] S2: run the catalog on this machine's monitors, print ids, `connection_path` and identity status, confirm EDID bytes match the id; record `validation/spike-s2-identity.md`.
- [x] T035 [P] [US1] Write failing `Tests/ddc_transaction.cpp` with a fake `IDdcPort`: write, 60 ms spacing, readback equals target means ok; readback mismatch means `ok:false`; duplicate Set for profiles that need it; read retries at 0.2 s and 1.0 s; `read_brightness` returns the raw value; no hardware call for an asleep monitor.
- [x] T036 [US1] Implement `Platform/IDdcPort.h` and `Platform/Dxva2DdcPort.cpp` (`GetVCPFeatureAndVCPFeatureReply`, `SetVCPFeature`, physical handle lifetime).
- [x] T037 [US1] Implement `Platform/DdcTransaction.h/.cpp` and `Platform/DdcTimingProfile.h/.cpp` (defaults 60 ms, 10 ms duplicate, retry delays, wake grace 2 s).
- [x] T038 [P] [US1] Write failing `Tests/display_state_events.cpp`: session display off and on, resume, and wake grace produce `monitor_display_state` for every external monitor with the current generation; writes wait out the grace.
- [x] T039 [US1] Implement `Platform/SessionEvents.h/.cpp` for `GUID_SESSION_DISPLAY_STATUS`, `PBT_APMRESUMEAUTOMATIC`, `WM_WTSSESSION_CHANGE`, and the `WM_DISPLAYCHANGE` plus monitor device-arrival debounce (500 ms), registered on the hidden window.
- [x] T040 [US1] Implement `Platform/DdcController.h/.cpp`: discovery with probe-by-read of VCP `0x10` and `0x62`, `monitor_discovered`/`monitor_disconnected`, generation echo, one worker thread with per-monitor serialization, execution of `pending_actions` (first action per monitor, snapshot re-read after each), `hardware_result` submission.
- [x] T041 [US1] Implement `TelaViva/AppController.h/.cpp`: wires `CoreBridge`, `DisplayCatalog`, `DdcController`, `SessionEvents` and the snapshot fan-out; handles `--background` start.
- [ ] T042 (spike, manual) [US1] S2 hardware: with the user present, read brightness and volume on the attached monitors, write one small change and restore it, record per-monitor results and failures in `validation/hardware-checklist.md` (rows start as "Not run").
- [x] T043 [P] [US11] (spike) S11 prototype: in `TelaViva/PopupWindow.xaml/.cpp`, a borderless `AppWindow` flyout anchored with `Shell_NotifyIconGetRect`, rounded corners, Acrylic, light dismiss, hidden from Alt+Tab; test `IsShownInSwitchers`, `DWMWA_WINDOW_CORNER_PREFERENCE`, `DWMWA_BORDER_COLOR` fallbacks; also check `SettingsControls` against Windows App SDK 2.x; record in `validation/spike-s11-flyout.md`.
- [ ] T044 (manual) [US11] Compare the S11 prototype side by side with the Windows 11 Quick Settings and Settings app (light, dark, high contrast, 100/150/225%); the user approves or lists changes before T047.
- [x] T045 [P] [US1] Write failing `Tests/manual_controls.cpp` for the port of macOS `ManualMonitorControls`: local draft state, latest-target coalescing, 128-target cap, no polling, authorization claim, key-intent adoption.
- [x] T046 [US1] Implement `Platform/ManualControls.h/.cpp` and wire it to `CoreBridge::set_brightness` and `set_volume` with the connection generation.
- [x] T047 [US11] Implement the real flyout in `TelaViva/PopupWindow.xaml/.cpp`: lazy creation on first click, destruction after a hidden timeout, entrance motion honoring `UISettings.AnimationsEnabled`, system theme resources, footer with Settings and Quit.
- [x] T048 [US1] Implement `TelaViva/MonitorCard.xaml/.cpp`: brightness and volume sliders with value tooltips, draft state from `ManualControls`, availability and ambiguity reasons, UI Automation names that include the monitor name.
- [x] T049 [P] [US1] Write failing `Tests/popup_layout.cpp` and implement `TelaViva/PopupLayout.h`: height for zero to many monitors, three cards then scroll, independent of DPI and text scale.
- [x] T050 [P] [US9] Add flyout string keys (card labels, availability reasons, ambiguous identity reason, empty state) to both `.resw` files and satisfy `Tests/strings_keys.cpp`.
- [ ] T051 [US11] Keyboard navigation and focus order for the flyout; Escape closes it; fix clipping at 225% text scale.
- [x] T052 [US1] Create `specs/archived/windows-version/validation/hardware-checklist.md` with all rows ("Not run"): DDC read and write per monitor, wake and resume, dock, flyout visuals, accessibility.

**Checkpoint**: ✋ MVP: the flyout controls external monitors on Windows 11 and idles cleanly. Complete and independently testable.

## Phase 4: G2 Settings window, language and launch at login (US-010, US-009, US-011) (P1) ⏳ (~12h)

**Goal**: A Settings window in the Windows Settings idiom with General page, language switching and launch at login.
**Independent Test**: Change language and see all text switch; enable launch at login and see the Run value; a failed registration leaves the setting off.

### Tasks

- [x] T053 [P] [US10] Write failing `Tests/launch_at_login.cpp` with a fake registry: write, read back, only then report success; failure leaves the setting unchanged and returns an error.
- [x] T054 [US10] Implement `Platform/LaunchAtLogin.h/.cpp` (HKCU Run value `TelaVivaOLED`, quoted path and `--background`).
- [x] T055 [P] [US11] Write failing `Tests/settings_payload.cpp`: the full `update_settings` payload never contains `selected_camera_id` or `camera_history`, ranges are clamped before send, unknown keys are never sent.
- [x] T056 [US11] Implement `Platform/SettingsModel.h/.cpp` (UI state to `update_settings` payload, camera selection as its own payload).
- [x] T057 [US11] Implement `TelaViva/SettingsWindow.xaml/.cpp`: `TitleBar` with `ExtendsContentIntoTitleBar`, Mica, left `NavigationView` (General, Keyboard, Automatic brightness), created on demand and destroyed on close.
- [x] T057a [US11] Give the Settings window (and the flyout when it needs one) the application icon through `AppWindow.SetIcon`, so the title bar, taskbar button and Alt+Tab show `AppIcon.ico` instead of the generic icon.
- [x] T058 [US11] Implement `TelaViva/SettingRow.xaml/.cpp` as the setting-card control: `SettingsCard` from the Community Toolkit if T043 passed, otherwise an in-house styled row; add `licenses/` entry if the toolkit is used.
- [x] T059 [US9] Implement `TelaViva/GeneralPage.xaml/.cpp` (language ComboBox auto, en-US, pt-BR; launch at login; about) and runtime language switching that re-renders all open windows.
- [x] T060 [P] [US9] Add all Settings string keys in en-US and pt-BR (pt-BR reviewed), including the "Laptop" and "Notebook" keys from `contracts.md`; satisfy `Tests/strings_keys.cpp`.
- [ ] T061 [US11] UI Automation names, keyboard navigation, and 225% text scale pass for the Settings window; honor transparency and high contrast fallbacks.
- [ ] T062 (manual) [US10] Verify launch at login on a real sign-in and that uninstalling later removes the Run value; record in `hardware-checklist.md`.

**Checkpoint**: ✋ Settings window complete for this phase and independently testable.

## Phase 5: G3 Presence protection, battery suspension and screensaver (US-002, US-003) (P1) ⏳ (~22h)

**Goal**: Webcam-based presence dimming, restore, low-light and permission handling, battery and screensaver reasons, all through the core lifecycle.
**Independent Test**: With presence protection on, leave the camera view and see the monitor dim after two checks, return and see it restore; deny camera privacy and see a status, not dimming; unplug power (laptop only) and see suspension.

### Tasks

- [x] T063 [P] [US2] Write failing `Tests/camera_selection.cpp`: most-recent-first history, first connected else default, infrared cameras excluded from selection and the list, history never written back.
- [x] T064 [US2] Implement `Platform/CameraSelection.h/.cpp`.
- [x] T065 [P] [US2] Write failing `Tests/camera_status_mapping.cpp` for the table in `contracts.md` (`E_ACCESSDENIED` to permission, device absent to unavailable, start failure with device present to busy, conversion or inference error to error).
- [x] T066 [US2] Define `Platform/ICameraBackend.h` and add a scripted `FakeCamera` to `Tests/Fakes.h`.
- [x] T067 [P] [US2] Implement `Platform/MediaCaptureBackend.h/.cpp`: shared read-only `MediaCapture` with `MediaFrameReader`, CPU memory, format near 640x480, about 2 fps retained, BGRA8 conversion, `DeviceWatcher`, `MediaFrameSourceKind.Infrared` classification, status mapping from T065.
- [ ] T068 (spike, manual) [US2] S3: run T067 against the RGB camera on this machine (IR camera present), confirm shared read-only works unpackaged while another app uses the camera, record formats, frame rate, enumeration and any error codes in `validation/spike-s3-camera.md`; if it fails, implement the `IMFSourceReader` backend behind `ICameraBackend`.
- [x] T069 [P] [US2] Write `tools/inference/fetch-windows-runtime.ps1`: downloads `onnxruntime-win-x64-1.20.1.zip` into a new empty directory, computes and checks SHA-256 against the manifest, extracts only `onnxruntime.dll` and the license files into `tools/inference/runtime/windows-x64/`, never executes anything.
- [x] T070 [US2] Add `[runtime.windows-x64.cpu]` to `assets/model/manifest.toml` with the SHA-256 values computed locally (archive and DLL); add `licenses/inference/` provenance for the Windows runtime.
- [x] T071 [P] [US2] Add `crates/telaviva-inference/tests/ort_yunet_smoke_windows.rs` (`cfg(all(target_os = "windows", target_arch = "x86_64"))`) mirroring the macOS smoke test with the CC0 fixture; skipped when the runtime is not fetched.
- [x] T072 [US2] Implement `Platform/InferenceHost.h/.cpp` (CPU only): runtime path chosen once, detector via `tv_inference_create`, release 30 s after leaving face mode, `tv_inference_assess_light_bgra`, `tv_inference_detect_bgra`.
- [x] T073 [P] [US2] Write failing `Tests/camera_lifecycle_driver.cpp` with `FakeCamera`, `FakeClock` and the real core DLL: scripts for acquisition, light assessment, dark settle, timeout, cancel, callback guard, retained capture, resume first sample at 1 s.
- [x] T074 [US2] Implement `Platform/CameraLifecycleDriver.h/.cpp`: maps `tv_camera_lifecycle_step` events to camera and timer calls, submits `presence_observation` with epoch `captured_at_ms` and `camera_generation`, `camera_status` on device changes, 5 s watchdog and settle windows.
- [x] T075 [P] [US3] Write failing `Tests/power_events.cpp`: AC and DC notifications, battery presence from `GetSystemPowerStatus`, unknown maps to `on_battery: null`.
- [x] T076 [US3] Implement `Platform/PowerEvents.h/.cpp` (`GUID_ACDC_POWER_SOURCE` registration and the initial read).
- [x] T077 [P] [US2] Write failing `Tests/screensaver_monitor.cpp`: poll every 2 s only while the setting is enabled, startup reconcile, `active` and `inactive` transitions, polling stops when disabled.
- [x] T078 [US2] Implement `Platform/ScreensaverMonitor.h/.cpp` (`SPI_GETSCREENSAVERRUNNING`).
- [x] T079 [US2] Wire camera, inference, power and screensaver into `TelaViva/AppController.cpp` and the snapshot fan-out.
- [x] T080 [US2] Add presence controls to `TelaViva/GeneralPage.xaml/.cpp`: protection toggle, sampling and return intervals (validated ranges), suspend on battery, keep camera on, brightness threshold, screensaver protection and targets, camera ComboBox (IR hidden).
- [x] T081 [US2] Show presence status in the flyout and switch the tray icon among face, no face and insufficient light from `snapshot.camera.presence`.
- [x] T082 [US2] Show an `InfoBar` in Settings and the flyout when the camera is denied, busy or unavailable, with a link to `ms-settings:privacy-webcam` for denial.
- [ ] T083 (manual) [US2] Hardware rows on this machine: privacy denied, camera shared with a video call, two cameras (RGB and IR), dim and restore timing with a DDC monitor; record results and measured latencies in `hardware-checklist.md`.
- [ ] T084 (manual) [US3] Battery suspension row: record "Not run" with the reason (desktop) unless a laptop is available.

**Checkpoint**: ✋ Presence protection works end to end on this machine and is independently testable.

## Phase 6: G4 Low-energy inference (US-007) (P1) ⏳ (~14h)

**Goal**: Optional DirectML acceleration only for measured device profiles, with safe fallback to CPU and no false "absent".
**Independent Test**: With an empty allowlist the detector reports provider CPU; with a test profile and a failing provider the detector falls back to CPU and records a failure marker.

### Tasks

- [x] T085 [P] [US7] Update `crates/telaviva-inference/src/header_contract_tests.rs` constants table and exported-function list for `TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE = 2` and `tv_inference_directml_available`; run it and see it fail.
- [x] T086 [US7] Add the constant and function to `include/telaviva_inference.h`; in `crates/telaviva-inference/src/lib.rs` add a provider kind enum, DirectML candidate registration (`DmlExecutionProvider`), `tv_inference_directml_available`, CoreML code behind `cfg(target_os = "macos")`, provider 1 on other platforms falling back to CPU, and `selected_provider` reporting what ran.
- [x] T087 [P] [US7] Add Rust tests (Windows only, runtime fetched) that provider 2 on the CPU-only runtime returns a CPU session with `selected_provider == 0`, and that no error is returned for a missing accelerator.
- [x] T088 [P] [US7] Fix the clock comment in `include/telaviva_core.h` (camera lifecycle reads no clock; `presence_observation` compares `captured_at_ms` with wall-clock time) and check `crates/telaviva-core/tests/header_contract.rs` still passes.
- [x] T089 [P] [US7] Write failing `Tests/provider_selection.cpp` for every transition in `contracts.md`: empty allowlist, matching profile, register failure, session failure, self-test mismatch, self-test slow, runtime error, marker reuse, marker cleared by a driver or app version change.
- [x] T090 [US7] Implement `Platform/AccelerationProfiles.h/.cpp` (strict loader for `acceleration-profiles.toml` schema 1) and create `assets/inference/acceleration-profiles.toml` with `schema = 1` and no profiles; add tests that unknown keys, providers other than `directml`, and entries without a `measurement` reference are rejected.
- [x] T091 [P] [US7] Implement `Platform/AdapterCatalog.h/.cpp` (DXGI adapter vendor id, device id and driver version) with a fake in tests.
- [x] T092 [P] [US7] Implement `Platform/InferenceFailureStore.h/.cpp` (`inference-failures.json`, corrupt file renamed to `.corrupt`) with tests.
- [x] T093 [US7] Extend `Platform/InferenceHost.h/.cpp`: profile matching, provider request, self-test (same frame on accelerated and CPU sessions, equal result, latency limit, `session.disable_cpu_ep_fallback` in the self-test), runtime-error fallback to CPU, failure markers, diagnostics line with the active engine.
- [x] T094 [P] [US7] Add `[runtime.windows-x64.directml]` to `assets/model/manifest.toml` (package pin and SHA-256 computed locally, `directml_dll = "system"`) and a `-Variant directml` mode to `tools/inference/fetch-windows-runtime.ps1` that extracts `onnxruntime.dll` and licenses only and never `DirectML.dll`.
- [x] T095 (spike, manual) [US7] S4: with the DirectML runtime on the Radeon iGPU and the RTX GPU of this machine, check session creation, equal fixture results, latency, and per-operator behavior; measure energy as far as this desktop allows (state the method and its limits); record in `validation/measurements.md`.
- [x] T096 (spike) [US7] S5: document the Windows ML NPU route and why it is out of the first release (no NPU here, `ort rc.9` lacks plugin providers); mark NPU rows "Not run" in `hardware-checklist.md`.
- [x] T097 [US7] Check the in-box `DirectML.dll` version on available Windows 11 builds (this machine reports 1.15.5 on build 26300) and record the minimum required version in `validation/measurements.md`.
- [x] T098 [US7] Add a profile entry to `assets/inference/acceleration-profiles.toml` only for a device and driver whose measurement in T095 shows lower energy with equal results; otherwise leave the file empty and record that decision.

**Checkpoint**: ✋ Provider selection works and fails safe; the shipped default is CPU unless a profile is recorded.

## Phase 7: G5 Efficiency mode (US-008) (P2) ⏳ (~8h)

**Goal**: The process runs in EcoQoS, respects a user-forced Efficiency mode, and keeps latency targets.
**Independent Test**: Force Efficiency mode from Task Manager and see presence dim and restore within the recorded latencies and the app not changing its own state afterwards.

### Tasks

- [x] T099 [P] [US8] Write failing `Tests/efficiency_policy.cpp` for the transition table in `contracts.md`: one process-level request at startup, UI thread exemption while a window is visible, no exemption in the user-managed state, hook thread exemption, no process-level reassert.
- [x] T100 [US8] Implement `Platform/IEfficiency.h`, `Platform/EfficiencyController.h/.cpp` (policy) and `Platform/Win32Efficiency.cpp` (`SetProcessInformation`, `SetThreadInformation`, `GetProcessInformation`, `GetPriorityClass`).
- [x] T101 [US8] Call the controller at startup and on flyout and Settings show and hide; add the efficiency state to the diagnostics line.
- [x] T102 [P] [US8] Apply tolerable-delay timers to the camera sample timer, the screensaver poll and the laptop poll through `Scheduler`, with tests that the delay window never exceeds 10% of the delay or 1 s.
- [ ] T103 (spike, manual) [US8] S6 and S8: force Efficiency mode in Task Manager, record how the process looks (priority class, throttling state), whether the app stays out of the way, camera frame delivery, hook latency, and presence and return latency with EcoQoS on and off; write `validation/spike-s6-s8-efficiency.md`.
- [ ] T104 (manual) [US8] Record idle CPU and memory with EcoQoS on versus off (SC-003, SC-006) in `validation/performance.md`.

**Checkpoint**: ✋ Efficiency behavior verified and documented.

## Phase 8: G6 Volume and mute keys (US-006) (P2) ⏳ (~12h)

**Goal**: Opt-in volume and mute keys that control the external monitor only while it owns the sound output; brightness keys untouched.
**Independent Test**: With the monitor as default output, volume keys change its VCP volume and the Windows volume does not move; with speakers as default, the keys behave normally.

### Tasks

- [x] T105 [US6] Read `tools/tests/` Swift tests for the hardware-key router, list the cases (step, rebase guard, mute memory, per-key-sequence latch, outstanding expiry, settle window) and write them as failing `Tests/key_intent_router.cpp`.
- [x] T106 [US6] Implement `Platform/KeyIntentRouter.h/.cpp` (port of `HardwareKeyRouter.swift` logic, volume and mute only).
- [x] T107 [P] [US6] Write failing `Tests/key_route.cpp` for the atomically published `KeyRoute` (publish during reads, swallow decision, latching across a key sequence).
- [x] T108 [US6] Implement `Platform/KeyRoute.h` and `Platform/KeyHook.h/.cpp`: dedicated thread with a message loop, `WH_KEYBOARD_LL` for the three volume keys, trivial callback, watchdog reinstall, fake hook for tests.
- [x] T109 [P] [US6] Write failing `Tests/audio_owner.cpp` with fake endpoints and monitor container ids: container-id equality gives an owner, no id or no match gives none, no name fallback, default output change republishes the route.
- [x] T110 [US6] Implement `Platform/AudioOwner.h/.cpp` (`IMMNotificationClient` for `eConsole`, endpoint `PKEY_Device_ContainerId`, monitor container id from the PnP node).
- [x] T111 (spike, manual) [US6] S2b: on this machine, list default render endpoints and monitor container ids, check whether an HDMI or DisplayPort endpoint matches a monitor, record per GPU vendor in `validation/spike-s2b-audio.md`.
- [x] T112 [US6] Wire route publishing from the snapshot (`hardware_keys_enabled`, `volume_key_step`, display state, monitor generation, volume support) and mute emulation through `set_volume`.
- [x] T113 [US6] Implement `TelaViva/KeyboardPage.xaml/.cpp`: hardware keys toggle and volume step `NumberBox` (1 to 15), with text that brightness keys keep their Windows behavior; add strings in both languages.
- [ ] T114 (manual) [US6] Hardware rows: volume keys, mute, keys with an elevated foreground window, behavior under forced Efficiency mode; record in `hardware-checklist.md`.

**Checkpoint**: ✋ Volume keys independently testable.

## Phase 9: G7 Laptop brightness follow (US-004, US-005) (P2) ⏳ (~20h)

**Goal**: External monitors follow the laptop panel through user-managed calibration points, with desktop and no-panel cases disabled for a stated reason.
**Independent Test**: On this desktop the Automatic brightness page shows the disabled reason and sends no writes (US-005); on a laptop (if available) follow, pause on lid close and calibration work (US-004).

### Tasks

- [x] T115 [P] [US5] Write failing `Tests/laptop_availability.cpp` for the table in `contracts.md` with fake DisplayConfig, WMI and power inputs (desktop, internal inactive, no readable brightness, API missing, healthy panel).
- [x] T116 [US5] Implement `Platform/LaptopDetection.h/.cpp` (internal or embedded target, lid, display state, battery hint, reason enum).
- [x] T117 [US4] Implement `Platform/ILaptopPanel.h` and `Platform/LaptopPanel.h/.cpp`: cached WMI connection on an MTA thread, `WmiMonitorBrightness` read, nearest-level `WmiSetBrightness`, 1 s poll with 0.5 s leeway only while follow is enabled and the lid is open, `builtin_display` submission.
- [x] T118 (manual) [US5] On this desktop, run the real path and record that Laptop follow is disabled with `noBuiltInDisplay` and that no write is sent; record in `hardware-checklist.md`.
- [x] T119 [P] [US4] Write failing `Tests/adaptive_brightness.cpp` with a fake power API: save scheme and AC and DC values, turn off, restore, scheme changed mid-calibration, failed restore keeps the session file.
- [x] T120 [US4] Implement `Platform/AdaptiveBrightness.h/.cpp` (`PowerGetActiveScheme`, `PowerRead*ValueIndex`, `PowerWrite*ValueIndex`, `PowerSetActiveScheme`, GUID `fbd9aa66-9553-4097-ba44-ed6e9d65eab8`).
- [x] T121 [P] [US4] Write failing `Tests/calibration_session.cpp` for `calibration-session.json` write, delete, and crash recovery at the next launch.
- [x] T122 [US4] Implement `Platform/CalibrationSession.h/.cpp`.
- [x] T123 [US4] Read the macOS `AmbientCalibrationController` tests under `tools/tests/` and port the cases (anchor within 2 points, slider bounds from neighbor points, pause follow, save through `update_settings`) as failing `Tests/ambient_calibration.cpp`.
- [x] T124 [US4] Implement `Platform/AmbientCalibrationController.h/.cpp` (port) wired to `LaptopPanel`, `ManualControls`, `AdaptiveBrightness` and `CalibrationSession`.
- [x] T125 [P] [US4] Write failing `Tests/follow_pause.cpp`: lid close or panel off sets `available` false and pauses; reopening resumes with the current level; calibration is refused while paused.
- [x] T126 [US4] Implement lid and display-off handling in `LaptopPanel` (`GUID_LIDSWITCH_STATE_CHANGE`, session display status, active DisplayConfig path) and submit `builtin_display` accordingly.
- [x] T127 [US4] Implement `TelaViva/AutomaticBrightnessPage.xaml/.cpp`: follow toggle, availability reason with `InfoBar`, per-monitor points list with add and remove (1 to 100%, at most 32, ascending), per-monitor protection toggle.
- [x] T128 [US4] Implement `TelaViva/CalibrationDialog.xaml/.cpp` as a `ContentDialog` with the Laptop slider, anchor status, Done and Cancel.
- [x] T129 [US4] Update the flyout card for followed monitors (volume plus protection row, no brightness slider) and add the per-monitor protection toggle to the flyout.
- [x] T130 [P] [US9] Add all Laptop and Notebook strings (availability reasons, calibration help, notices) to both `.resw` files and satisfy `Tests/strings_keys.cpp`.
- [ ] T131 (spike, manual) [US4] S7: on a laptop, test WMI brightness change events, adaptive-brightness read and write, lid-closed docked behavior; if no laptop is available, record "Not run" and keep the polling design.
- [ ] T132 (manual) [US4] Hardware rows for laptop follow, calibration, lid pause and crash recovery; record "Not run" unless a laptop is available.

**Checkpoint**: ✋ Laptop follow logic verified with fakes and the desktop case verified on hardware.

## Final Phase: Polish ⏳ (~18h)

- [x] T133 [P] Extend `packaging/windows/build-app.ps1` to produce the release zip (app, DLLs, model, CPU ONNX Runtime, licenses) and write `packaging/windows/Install-TelaVivaOLED.ps1` and `Uninstall-TelaVivaOLED.ps1` (per-user path, Start menu shortcut, removes files and the Run value).
- [ ] T133a Make the installer's Start menu shortcut use the application icon (`IconLocation` pointing at `TelaViva.exe,0`), and check the icon in Start, the taskbar, Task Manager and the Settings > Apps list after an install in a temp profile.
- [x] T134 [P] Complete `.github/workflows/windows.yml`: fetch and verify the runtime, run the Rust smoke test, run the installer in a temp profile, run `--smoke-test` on the packaged build, upload the zip artifact, and record the artifact size.
- [x] T135 [P] Add `licenses/` entries and update `licenses/DEPENDENCIES.md` for Windows App SDK, ONNX Runtime Windows build, and the Community Toolkit package if used.
- [x] T136 [P] Update `README.md` (Windows section: status, SmartScreen note for unsigned builds, how to pin the tray icon), `CHANGELOG.md` (Unreleased), `ARCHITECTURE.md` (Windows status and targets), and `api.md` (inference provider values and the clock note).
- [ ] T137 (manual) Run the full visual and accessibility review (SC-012): light, dark and high contrast, 100/150/225% scaling, Narrator and keyboard-only pass; record in `validation/accessibility.md`.
- [ ] T138 (manual) Record idle CPU and memory for SC-003 with all protections off and with presence protection on; add to `validation/performance.md`.
- [x] T139 Complete `validation/hardware-checklist.md` and write `validation/results.md` with every row "Not run", "Pass" or "Fail" plus device and driver; list claims the README may make.
- [x] T140 Run `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo test --workspace --all-targets --locked`, the native build and every `Tests` executable; fix anything that fails.
- [x] T141 Write `specs/archived/windows-version/adr-notes.md` listing the decisions to turn into ADRs in `/codeadvisor-dev:finalize`: Windows shell and platform split, two cdylibs, monitor identity, provider allowlist, efficiency policy, packaging, native UI idiom.

## UX parity additions (from the macOS history review, 2026-10-17)

Source: [mac-ux-parity.md](mac-ux-parity.md) (checklist, Windows wording and documented deviations). These tasks carry macOS refinements that had no task. Each is done inside the phase named in brackets, together with that phase's checkpoint.

- [x] T150 [Phase 3] [US11] In `TelaViva/MonitorCard.xaml/.cpp` add a right-aligned value label `N%` next to each slider (monospaced digits, fixed width, `round`), updated live from the draft, in addition to the tooltip, as macOS shows it.
- [x] T151 [Phase 3] [US1] Add a `failed` flag to `SliderState` in `Host/FlyoutState.h`, fill it from `ManualControls::status` in `Host/AppController.cpp`, and show a caption under the slider with `flyout.failed` ("Could not apply; showing confirmed value" / "Não foi possível aplicar; exibindo valor confirmado") in the system critical foreground color when it is set; add the keys to both `.resw` files.
- [x] T152 [Phase 3] [US1] Flyout header: title "TelaViva OLED" and a Refresh icon button (`flyout.refresh`, "Refresh status" / "Atualizar estado") that calls a new `AppController::refresh()` (DDC catalog refresh plus a core snapshot); the tray right-click stays the native Settings and Quit menu.
- [x] T153 [Phase 3] [US11] Flyout notice `flyout.coreunavailable` ("Core service is not linked" / "Serviço principal não conectado") with a warning glyph, shown instead of the cards when the core cannot be created.
- [x] T154 [Phase 4] [US9] Port every key of `native/macos/TelaViva/Localization.swift` (general, Keyboard and Automatic brightness pages, calibration, notices, camera statuses, status item) into both `.resw` files with the same key names, applying the Windows wording and the dropped keys from `mac-ux-parity.md`; keep `Tests/strings_keys.cpp` green and extend it to fail on any key from the dropped list.
- [x] T155 [Phase 4] [US10] Launch at login failure: revert the toggle and show a `ContentDialog` titled `loginItemFailureTitle` with `loginItemFailure` plus the error details and an OK button (strings from T154).
- [x] T156 [Phase 4] [US11] Settings window size and behavior: default 880x640 DIP, minimum 640x480, opens centered on the monitor of the tray icon, closes the flyout when it opens, a second open brings the existing window to the front; changing the language updates the Settings text immediately.
- [x] T157 [Phase 4] [US11] Value formats on the Settings pages: presence threshold `N%`, intervals `N s` (return interval with up to two decimals, so 1.25 shows `1.25 s`), key step `N pt`; every control saves on change (no Save button) and the page loads settings once.
- [x] T158 [Phase 5] [US2] General page "Monitors in protection": heading, help text, one toggle per connected monitor named by the monitor, default on, empty state "No monitors connected"; each toggle writes `protection_disabled` as the full sorted set, the same rule as the flyout toggle.
- [x] T159 [Phase 5] [US2] General page "Availability": camera `ComboBox` with "Use default camera" first, inline "Camera: <status>" line with a camera glyph, and the two help captions (camera access, display support naming docks, hubs and adapters).
- [x] T160 [Phase 5] [US2] Camera status wording: the 17 `cameraStatus.*` strings (pt-BR in lowercase, "Câmera: funcionando") and the "Camera: <status>" format; the flyout footer shows the line on one row with an ellipsis, filled from the core `display_status` and `presence`, and the tray starts in the "no face" state until the first snapshot.
- [x] T161 [Phase 5] [US2] Flyout card protection rules: the per-monitor protection toggle shows only when presence or screensaver protection is enabled and defaults to on; the "Protection brightness" row shows when screensaver protection is on and the monitor is protected, slider 0 to cap and disabled when the cap is 0, cap = min(confirmed brightness, lower outstanding draft), value clamped and saved on show and on every brightness change.
- [x] T162 [Phase 5] [US2] Tests for the pure parts of T161 (cap rule, clamping, visibility conditions) in `Tests/protection_rules.cpp` with the logic in `Platform/ProtectionRules.h`.
- [x] T163 [Phase 8] [US6] Keyboard page: status line with the three states (off, active, unavailable: "Windows continues handling the keys"), Windows help text, and a fine step with Shift (1 point) in `KeyIntentRouter` with tests; volume step shown as `N pt`.
- [x] T164 [Phase 9] [US4] Automatic brightness page: per-monitor status captions (Calibrated, Not calibrated, Brightness control not supported, Monitor identity not stable); the calibration panel is hidden for unsupported or unstable monitors and while the laptop panel is unavailable.
- [x] T165 [Phase 9] [US4] Points list rules: ascending rows "Monitor N% → Laptop M%" (Notebook in pt-BR), Calibrate and Remove buttons, Remove disabled for the point being calibrated, add row 1 to 100 with default = first unused of 25, 50, 75, 100 else first unused integer, Add disabled when the value is saved or at 32 points, hint "At least two points are needed to finish the calibration." below two points; tests for the default and disabling rules.
- [x] T166 [Phase 9] [US4] Calibration dialog details: width 420 DIP, title "Calibrate the monitor at N%", help text, "Waiting for the monitor to reach this brightness…" until the monitor is within 2 points, Done as the default button disabled until then, Cancel on Escape, slider "Laptop brightness" bounded by the neighbor points and disabled when the range has zero width.
- [x] T167 [Phase 9] [US4] Calibration end triggers: Done, Cancel or Escape, dialog close, leaving the Automatic brightness page, closing the Settings window, display off or asleep, laptop unavailable, app quit; each restores the laptop level, adaptive brightness and the monitor brightness, clears the calibrating flag and deletes the session file; tests with fakes for every trigger.
- [x] T168 [Phase 9] [US4] The six calibration notices (Laptop and Notebook wording from `mac-ux-parity.md`) shown once under the monitor list, never auto-dismissed, cleared when a new calibration starts, with a test for each trigger.
- [x] T169 [Final] Walk through `mac-ux-parity.md` and record each row as Pass, Not run or Deviation (with the reason) in `specs/archived/windows-version/validation/ux-parity.md`.

## Dependencies & Execution Order

```text
Phase 1 Setup
  -> Phase 2 Foundational (T008-T010 S1, T024-T027 S9/S10 are gates)
       -> Phase 3 G1 (MVP)
            -> Phase 4 G2 Settings, language, launch at login
                 -> Phase 5 G3 Presence, battery, screensaver
                      -> Phase 6 G4 Low-energy inference (needs InferenceHost from G3)
                      -> Phase 7 G5 Efficiency (needs camera timers from G3)
                 -> Phase 8 G6 Volume keys (needs G1 DDC and G2 Settings)
                 -> Phase 9 G7 Laptop follow (needs G1 DDC and G2 Settings)
  -> Final Phase Polish (after the groups being released)
```

- **Independent after Phase 4**: G3, G6 and G7 touch different files and can run in parallel; G4 and G5 follow G3.
- **Spike gates**: S1 (T010) blocks all native work; S10 (T027) can change the shell choice (new ADR) before Phase 3 grows; S11 (T043, T044) gates the flyout implementation (T047); S3 (T068) may swap the camera backend; S4 and S5 only affect Phase 6 and the default stays CPU.
- **Parallel inside phases**: tasks marked `[P]` touch different files; test tasks sit before their implementation tasks.
- **Manual tasks** need the user: T027, T034, T042, T044, T062, T068, T083, T084, T095, T103, T104, T111, T114, T118, T131, T132, T137, T138.

## Implementation Strategy

- **MVP first**: Phases 1 to 3 give a Windows 11 tray app that controls external monitors with a native flyout. Ship nothing else as "supported" until its checklist rows have recorded runs.
- **Incremental delivery**: each group ends in a checkpoint that can be demoed on this machine, except the laptop and NPU rows, which stay "Not run" without that hardware.
- **Delegation**: per the owner's rule, the orchestrator designs and verifies; Haiku agents write the code and tests from mechanical briefings (exact files, expected behavior, what not to touch). Independent files go to parallel agents; edits to the same file go to one agent. The orchestrator runs `msbuild`, the test executables and `cargo` checks after each batch.
- **Risk order**: the first hours go to S1, S9, S10 and S11 because they can invalidate the shell approach, the string loading or the flyout design.
- **Rust stays additive**: only T085 to T088 touch Rust and headers, and ABI version stays 1.

## Progress Tracking

⏳ Not Started · ⏰ In Progress · ✅ Completed. Update the status of each phase and add Notes below it while working.

| Phase | Status | Notes |
|-------|--------|-------|
| 1 Setup | ✅ | Solution, Platform, Tests and TelaViva build with msbuild; deviations and toolchain findings in validation/toolchain.md. |
| 2 Foundational | ✅ | S1, S9 go; S10 provisional go (0.078% of one core, 57.6 MB); full local pipeline green, see validation/foundation.md. Tray icon visibility and the first GitHub Actions run are not verified. |
| 3 G1 External monitors and flyout (MVP) | ⏳ | |
| 4 G2 Settings, language, launch at login | ⏳ | |
| 5 G3 Presence, battery, screensaver | ⏳ | |
| 6 G4 Low-energy inference | ⏳ | |
| 7 G5 Efficiency mode | ⏳ | |
| 8 G6 Volume and mute keys | ⏳ | |
| 9 G7 Laptop follow | ⏳ | |
| Final Polish | ⏳ | |

## Implementation notes (2026-10-17)

Done: every code and documentation task. Open tasks are only the ones that need the owner at the machine or a rendered UI: T042 (DDC write test), T044 (flyout versus Quick Settings), T051 and T061 (code applied; 225% scale, Narrator and high contrast still to verify), T062, T068, T083, T084, T103, T104, T114, T131, T132, T133a (shortcut `IconLocation` verified; the drawn icon in Start, taskbar and Settings is not), T137 and T138 (idle cost measured with protections off only).

Plan changes made along the way:
- The inference ABI gained `tv_inference_create_with_provider_device` (adapter index) in addition to the contract's `tv_inference_create_with_provider`; a `directml` cargo feature (off by default) registers the provider, and the shipped `telaviva_inference.dll` is built with it.
- `SettingsSnapshot` gained `ambientCurves` (the core already sent it); `PowerEvents` gained the lid callback; `--probe-laptop` was added for T118.
- The installer default is `%LOCALAPPDATA%\Programs\TelaVivaOLED`; the uninstaller has `-Quiet`; the packaging step removes unused Windows App SDK AI and ML components (including `DirectML.dll`) before zipping.
- Hook thread rule amended: no thread is exempted from EcoQoS in the user-managed state.

Known limitations (also in `adr-notes.md`): the router's outstanding-action inputs are not wired (the core's C ABI `submit` returns only a status); calibration crash recovery restores adaptive brightness only; a key hook removed while idle is noticed on the next key press; the WMI probe cannot tell "API missing" from "not readable"; the DirectML adapter index mapping is inferred from latency.

Open owner decisions: the Windows App SDK runtime license versus the open-source-only rule; flyout spacing and height cap; the first GitHub Actions run of `.github/workflows/windows.yml`.

Changes after the owner's first hands-on review (2026-10-17):
- Settings camera status line showed three empty squares: the glyph was a raw private-use character in the source; it is now an escaped single glyph (`\xE722`), as in the flyout.
- Flyout height now comes from measured card heights (`computePopupHeight`), so one monitor needs no scrollbar and scrolling starts at the fourth monitor; the cards are updated in place instead of being rebuilt on every snapshot, so dragging a slider is no longer interrupted.
- Manual brightness and volume now release the in-flight command when the DDC write completes (a completion observer on `DdcController`, as in the macOS queue), report a failed write, share one per-monitor queue with the hardware keys, and the protection-brightness slider is debounced off the UI thread.
- The tray right-click menu is a WinUI window in the Windows 11 style instead of the classic Win32 menu.
- `.gitignore` now ignores `Generated Files/` and `obj/` under `native/windows`.
- `scripts/build-windows.ps1` is the one-command production build (the counterpart of `scripts/build-macos.sh`).