# Architecture: Windows Version with Laptop Parity and Low-Energy Face Detection

**Feature**: Windows version (shell, platform layer, inference runtime, packaging, CI)
**Date**: 2026-10-17
**Branch**: `windows-version`
**Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary

Build a Windows 11 x64 tray application as a second native shell over the unchanged Rust core and C ABI (ADR 0001). The shell is split into a UI-free C++ platform layer (monitors and DDC, power and session events, audio ownership, volume-key hook, camera, laptop panel, efficiency mode) that is testable with fakes, and a thin C++/WinRT WinUI 3 UI. The core and inference crates already build and pass all tests on `x86_64-pc-windows-msvc`; the only Rust change is an additive inference ABI value for DirectML, driven by a measured-device allowlist that is empty until measurements exist, so the first release runs CPU inference everywhere (Q2 = A).

## Technical Context

**Language/Stack**: Rust 1.99 core and inference (unchanged policy), C++20/C++/WinRT shell, WinUI 3 on Windows App SDK 2.x (self-contained, hybrid CRT), MSBuild, PowerShell tooling.
**Key Dependencies**: `ort =2.0.0-rc.9` with `load-dynamic` and ONNX Runtime 1.20.1 (MIT); Windows App SDK (source MIT on GitHub, but the redistributed NuGet runtime ships under Microsoft Software License Terms, not MIT; see `licenses/DEPENDENCIES.md`; open decision for the owner against the open-source redistribution rule, see `adr-notes.md` section 10); Windows system APIs only for everything else (dxva2, DisplayConfig, WMI, powrprof, WinRT MediaCapture, MMDevice, Shell_NotifyIcon). In-box `DirectML.dll` is used from `System32`, never redistributed. The only additional candidate is the Windows Community Toolkit `SettingsControls` package (MIT) for settings rows, adopted only if spike S11 shows it supports Windows App SDK 2.x; otherwise in-house styled rows are used.
**Storage**: `%LOCALAPPDATA%\TelaVivaOLED\settings.json` (core, schema 1, unchanged); shell-owned `calibration-session.json` and `inference-failures.json` in the same folder; read-only `acceleration-profiles.toml` shipped with the app.
**Testing**: `cargo test` (already green on Windows, 196 tests), plain C++ test executables with fake backends (mirrors `tools/tests/*.sh` on macOS), packaged `--smoke-test`, DLL export check against the headers, Windows CI, and a hardware checklist with "Not run" rows.
**Platform**: Windows 11 x64 (22H2 or newer for the app; 24H2 only matters for a future NPU path). ARM64 is a later feature but nothing here may assume x64.

## Technical Decisions

### Decision 1: Keep C++/WinRT + WinUI 3 over the C ABI, split platform from UI
**What**: One process. `native/windows/Platform` is a static library with no XAML dependency; `native/windows/TelaViva` is the WinUI 3 app (popup, settings, tray host). Platform code talks to the core only through `CoreBridge`, and to the OS only through small interfaces (`IDdcPort`, `IDisplayCatalog`, `IPowerEvents`, `IAudioOwner`, `ICameraBackend`, `ILaptopPanel`, `IKeyHook`, `IClock`, `IEfficiency`) that tests replace with fakes.
**Why**: ADR 0001 already chose it. The split gives the same test seam the macOS shell has, and lets `--smoke-test` run before any XAML initialization.
**Alternatives Considered**: Win32/Direct2D-only shell (lower idle cost, much more UI code, not the decided stack); a Rust-native shell (breaks ADR 0001). Both stay as fallbacks only if spike S10 shows the WinUI 3 idle cost misses the budget.
**Trade-offs**: WinUI 3 idle CPU and memory are unmeasured, so a gate (S10) decides before UI work grows.

### Decision 2: Two Rust cdylibs, not two static libraries
**What**: Build `telaviva_core.dll` and `telaviva_inference.dll` with `cargo rustc --lib --crate-type cdylib` (no manifest change for core). The shell links the import libraries.
**Why**: Linking two Rust staticlibs into one MSVC binary commonly collides on duplicated std symbols. A DLL boundary with a C ABI avoids that and also removes any CRT mismatch with the WinUI hybrid CRT. The inference crate already declares `cdylib`.
**Alternatives Considered**: one combined crate re-exporting both ABIs (exports from dependency rlibs are unreliable on MSVC); two staticlibs (duplicate symbols).
**Trade-offs**: two copies of std (a few hundred KiB) and two DLLs to ship. Spike S1 confirms exports with `dumpbin /exports` against the headers.

### Decision 3: Inference stays on `ort rc.9` and CPU; DirectML is allowlist-only (Q1, Q2)
**What**: Keep `ort =2.0.0-rc.9` and ORT 1.20.1. Add provider `TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE = 2`, `tv_inference_directml_available()`, and gate CoreML code to macOS. The shell asks for DirectML only when the device matches an entry in `assets/inference/acceleration-profiles.toml`; the file ships empty. The first release therefore loads the CPU-only ONNX Runtime build. A DirectML-enabled `onnxruntime.dll` (MIT, from the DirectML NuGet package) is added to the manifest as a second runtime variant only when a measured profile exists. DirectML itself comes from `System32` (verified present on this machine: 1.15.5).
**Why**: Q2 = A requires proof before enabling. The model is fp32, static `[1,3,640,640]`, 8 plain ONNX operator types at opset 11 (verified), so DirectML is plausible but its per-operator support and energy are unproven. Keeping the CPU runtime as the default avoids shipping and loading DirectML-linked code until it earns its place.
**Alternatives Considered**: ship the DirectML runtime always (adds a dependency path and failure mode with no proven benefit); upgrade to `ort rc.14` now (needs Rust 1.100 per reported release notes, API churn, and only matters for plugin NPU providers, deferred); Windows ML plugin NPU providers (not documented for non-WinRT Rust, no NPU on the dev machine).
**Trade-offs**: NPU is out of the first release; the provider enum reserves value 3 for it. A later ADR covers the `ort` upgrade if spike S5 shows a route.

### Decision 4: Provider selection is data-driven and fails safe
**What**: At detector creation the shell enumerates adapters (DXGI vendor id, device id, driver version), matches the allowlist, and requests the listed provider. The Rust side registers the provider and returns the provider actually used. A startup self-test runs the same embedded frame on the chosen engine and on CPU; a mismatch, any error, or a slow result marks the profile as failed in `inference-failures.json` (key = profile id + driver version + app version) and the detector falls back to CPU. A failed key is not retried until the driver or app version changes. A runtime error during detection falls back to CPU for that and later frames and never reports "absent".
**Why**: `ort rc.9` has no `error_on_failure` and `build()` cannot fail; failure surfaces only from `register()`. ORT's `session.disable_cpu_ep_fallback` config key exists and is used in the self-test so a silent partial CPU fallback is visible.
**Alternatives Considered**: runtime heuristics ("use GPU if present"); rejected by Q2.
**Trade-offs**: every enabled profile needs a recorded measurement in `specs`/validation notes before it is added to the file.

### Decision 5: Monitor identity from EDID, excluding the connector path
**What**: `monitor_id = "win-" + manufacturer + "-" + product + "-" + serial + "-" + edidHash8`, where the EDID comes from the registry node of the DisplayConfig target. The connector path goes to `connection_path` only. `identity_status` is `stable` when the id is unique among the attached monitors; an unusable serial (`0`, `1`, `0x01010101`, `0xFFFFFFFF`) does not by itself make a monitor ambiguous. If two or more attached monitors produce the same id (identical EDID content, typically identical monitors without a usable serial), all of them are `ambiguous`, which makes the core enqueue no hardware action for them and the UI show them read-only with a reason. Amended after spike S2; contracts.md is authoritative.
**Why**: macOS embeds the connector location in the id, so curves orphan on a port change (ADR 0008 notes this). Excluding the path keeps `ambient_curves`, `protection_disabled` and `screensaver_targets` across docks. Guessing between identical monitors could write to the wrong panel.
**Alternatives Considered**: path-based ids (orphan on every port change); ordinal disambiguation (can swap monitors silently).
**Trade-offs**: two identical serial-less monitors are not controllable in the first release (known limitation, shown in the UI). The HMONITOR to target bridge uses `GetMonitorInfo` GDI name to DisplayConfig source name to target name, validated by EDID bytes (spike S2).

### Decision 6: DDC through dxva2 with probe-by-read and macOS timing as defaults
**What**: One DDC worker thread serializes all monitors. Capabilities strings are not requested (slow); support is probed by reading VCP `0x10` (brightness) and `0x62` (volume). Timing defaults mirror macOS (60 ms between transactions, duplicate Set 10 ms apart where a profile says so, write then readback must equal the target, retry delays 0.2 s and 1.0 s) and live in a `DdcTimingProfile` table. A 2 s grace follows display wake or resume before the first DDC call. Per-monitor display state is not observable on Windows, so `monitor_display_state` follows the session display state for every monitor.
**Why**: Microsoft does not document latency or thread safety, and community projects report failures on resume, standby, docks and some drivers. The core already bounds retries and treats failures as not-success.
**Alternatives Considered**: capabilities-string parsing (seconds per monitor); polling VCP `0xD6` for per-monitor power (values conflict across sources).
**Trade-offs**: timings are macOS-derived assumptions until hardware runs; the `DdcTimingProfile` table is the single tuning point.

### Decision 7: Laptop panel through WMI, polling like macOS, adaptive brightness through powrprof
**What**: `ILaptopPanel` reports `available`, `brightness` and a shell-side reason (`noBuiltInDisplay`, `brightnessNotAdjustable`, `apiUnavailable`, `displayOffline`), then submits `builtin_display`. Detection: an internal or embedded DisplayConfig target plus a successful `WmiMonitorBrightness` read; chassis type and battery are hints only. Lid closed, internal target inactive, or session display off gives `displayOffline`. Read and write use `root\wmi` `WmiMonitorBrightness` and `WmiSetBrightness`, snapping to the nearest reported level. Polling is 1 s with 0.5 s leeway only while follow is enabled, lid open and display on, using a cached WMI connection on an MTA thread; spike S7 tests change events and measures cost. Adaptive brightness uses `PowerReadACValueIndex`, `PowerReadDCValueIndex`, `PowerWriteACValueIndex`, `PowerWriteDCValueIndex` and `PowerSetActiveScheme` for GUID `fbd9aa66-9553-4097-ba44-ed6e9d65eab8` in subgroup `7516b95f-f776-4464-8c53-06167f40cc99`. The original scheme GUID and both values are saved in `calibration-session.json` and restored on finish, quit, and next launch after a crash.
**Why**: Direct parity with ADR 0008, 0010, 0012 and 0013 using the Microsoft-documented brightness path. Brightness change events and the adaptive-brightness GUID semantics are unverified, hence spikes, and the dev machine has no panel, so these rows stay "Not run" until a laptop is available.
**Alternatives Considered**: `GetMonitorBrightness` on the internal panel (reported unreliable); shelling out to `powercfg` (fragile parsing, process spawn cost).
**Trade-offs**: WMI polling costs more than the macOS read; SC-003 and SC-007 measurements decide if the interval widens to 2 s.

### Decision 8: Camera through MediaCapture/MediaFrameReader behind `ICameraBackend`
**What**: Shared read-only `MediaCapture` with `MediaFrameReader`, CPU memory, lowest adequate format from `SupportedFormats` (about 640x480, retained capture at about 2 fps, single-frame checks for sampled mode), converted to top-down BGRA8 for `tv_inference_detect_bgra`. Enumeration uses `MediaFrameSourceGroup` and `DeviceWatcher`; infrared groups (`MediaFrameSourceKind.Infrared`) are excluded from selection and listing, with no unhide control in the first release (Q8). The camera id is the symbolic link. Errors map to core camera statuses: `E_ACCESSDENIED` to `permission`, device absent to `unavailable`, start failure with the device present to `busy`, anything else to `error`. The lifecycle (deadlines, settle windows, dark settle) is driven through `tv_camera_lifecycle_step` exactly as `CameraLifecycle.swift` does, with `now` from `steady_clock` seconds.
**Why**: It covers sharing, format and frame-rate choice, source-kind filtering and add/remove notifications with documented APIs. Studio Effects may alter frames on supported hardware; it is recorded as a validation row, not designed around.
**Alternatives Considered**: Media Foundation `IMFSourceReader` (native, but IR classification and enumeration are harder); it remains the fallback behind the same interface if spike S3 shows unpackaged `MediaCapture` problems.
**Trade-offs**: "in use by another app" cannot be reliably separated from other start failures, so the UI says "could not start" for `busy`, matching macOS where `busy` already covers timeouts.

### Decision 9: Volume and mute keys through a trivial low-level hook and a lock-free route table
**What**: A dedicated hook thread with its own message loop installs `WH_KEYBOARD_LL` for `VK_VOLUME_UP`, `VK_VOLUME_DOWN` and `VK_VOLUME_MUTE` only. The callback reads an atomically published `KeyRoute` (enabled, owner monitor id, connection generation, volume supported, step), decides swallow or pass in microseconds, and posts the intent to the bridge queue. The hook thread is exempt from EcoQoS, and a watchdog reinstalls the hook if events stop arriving after the timeout window. Step, rebase guard, mute memory and per-key-sequence latching are a C++ port of `HardwareKeyRouter.swift` logic (`KeyIntentRouter`) with the macOS test vectors ported. Ownership: the default render endpoint (`eConsole`) container id must equal a monitor's PnP container id; no match or no id means pass through to Windows, never a name guess. Brightness keys are not hooked (Q5).
**Why**: `WH_KEYBOARD_LL` is the only documented way to consume the key, Microsoft requires a fast callback, and a timeout silently removes the hook.
**Alternatives Considered**: Raw Input (observe-only, cannot swallow); `RegisterHotKey` (does not document volume-key behavior).
**Trade-offs**: logic duplicated across Swift and C++ until a future ADR moves it into the core; Windows own volume OSD does not show when a key is swallowed (known, listed under Risks). Matching HDMI and DisplayPort endpoints by container id is inferred, not documented (spike S2b).

### Decision 10: Event sources through one hidden top-level window and minimal polling
**What**: A hidden top-level (not message-only) Win32 window on the UI thread receives: `GUID_ACDC_POWER_SOURCE`, `GUID_SESSION_DISPLAY_STATUS`, `GUID_LIDSWITCH_STATE_CHANGE`, `PBT_APMRESUMEAUTOMATIC`, `WM_WTSSESSION_CHANGE`, `WM_DISPLAYCHANGE`, `WM_DEVICECHANGE` for `GUID_DEVINTERFACE_MONITOR` (debounced 500 ms), and `WM_QUERYENDSESSION`/`WM_ENDSESSION`. The screensaver has no documented start or stop event, so `SPI_GETSCREENSAVERRUNNING` is polled every 2 s only while `screensaver_protection_enabled` is true and once at startup. Battery presence comes from `GetSystemPowerStatus` and `GUID_ACDC_POWER_SOURCE`; unknown maps to `on_battery: null`.
**Why**: parity with the macOS observers at the lowest wake-up cost; nothing polls while its feature is off.
**Trade-offs**: screensaver detection latency is up to 2 s.

### Decision 11: Efficiency mode set once at process level, thread exemptions only for UI and hook
**What**: At startup the process requests EcoQoS (`ProcessPowerThrottling` with `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`). It never calls `timeBeginPeriod`. While the popup or settings window is visible, only the UI thread requests normal quality (thread-level). The hook thread always requests normal quality. The shell never reasserts process-level state after startup, and if the process priority class is idle or the process-level state differs from what the app set (the user forced Efficiency mode in Task Manager), it skips all thread exemptions and logs a diagnostic. Timers use thread-pool timers with a tolerable delay window (10% of the delay, at most 1 s), like the macOS leeway.
**Why**: Q7 = A and FR-025 to FR-028. Setting it once and never fighting satisfies "respect the user's choice"; exempting only the UI thread keeps the camera and inference threads efficient.
**Trade-offs**: how Task Manager's toggle looks to the process is inferred; spike S8 verifies detection. Effects on camera frame delivery and keyboard-hook latency under EcoQoS are undocumented and measured in S8.

### Decision 12: Distribution is a zip with a per-user PowerShell installer, self-contained Windows App SDK (Q6)
**What**: `packaging/windows` builds a self-contained, hybrid-CRT unpackaged app folder, adds the Rust DLLs, ONNX Runtime CPU DLL, model and licenses, and produces a zip plus `Install-TelaVivaOLED.ps1` and `Uninstall-TelaVivaOLED.ps1` (per-user copy to `%LOCALAPPDATA%\Programs\TelaVivaOLED`, Start menu shortcut, uninstall removes files and the Run value). Launch at login writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, verified by reading it back before the setting is saved.
**Why**: Inno Setup is not OSI-licensed and requests a commercial license, and WiX v6 charges a maintenance fee for revenue use, both against the open-source rule. The Windows App SDK runtime installer's redistribution terms were not confirmed, so self-contained avoids chaining it.
**Alternatives Considered**: framework-dependent with the runtime installer chained; MSIX (rejected by Q6).
**Trade-offs**: larger download; unsigned builds trigger SmartScreen warnings (documented in the README).

### Decision 13: Strings from `.resw` with an explicit language context
**What**: `en-US` and `pt-BR` `.resw` files, read through `ResourceManager` with a `ResourceContext` whose Language qualifier is set from the app's own language setting (`auto` resolves from the user's preferred UI languages). XAML text is assigned in code, so switching language re-renders without restarting. Windows copy says "Laptop" and "Notebook", never "MacBook" or "Mac". If spike S9 shows unpackaged `.pri` loading fails, a build script generates a C++ string table from the `.resw` files instead.
**Why**: ADR 0015 and FR-030; `PrimaryLanguageOverride` behavior for unpackaged apps is undocumented.

### Decision 14: Process behavior around the core
**What**: A named mutex keeps one instance (a second launch opens the popup of the first). The snapshot callback fires synchronously inside `tv_submit` on the caller's thread, so it only posts a "revision changed" message to the bridge queue; the bridge calls `tv_snapshot` on its own thread and fans out to subsystems. Shutdown mirrors macOS: stop sources, `tv_shutdown`, drain `pending_actions` until empty with an 8 s deadline (also run from `WM_ENDSESSION`), restore adaptive brightness, then exit. `config_path` is `%LOCALAPPDATA%\TelaVivaOLED\settings.json` and `os_locale` is passed for compatibility only.
**Why**: Same lifecycle and failure semantics the core was tested against.

### Decision 15: A Windows 11-native interface, not a port of the macOS layout (FR-034, FR-035, US-011)
**What**: The Windows shell designs its UI from Windows 11 idioms and shares only the functions and rules with macOS.
- **Stack**: WinUI 3 on Windows App SDK 2.5.x (pin the exact version at implementation), standard controls only, system theme resources for light, dark, accent and high contrast, no custom palette. Type uses the Windows 11 type ramp (Segoe UI Variable: Body 14/20, Subtitle 20/28, Title 28/36, sentence case); icons use Segoe Fluent Icons through `FontIcon`.
- **Tray flyout (`PopupWindow`)**: a Quick Settings-style flyout, one column, anchored to the notification-area icon with `Shell_NotifyIconGetRect` and the work area, rounded corners and a system material, opening with the system entrance motion and hiding on deactivation (light dismiss). Each monitor is a card with a brightness slider and a volume slider with value tooltips, a presence status row and the availability reason when something is unavailable; the footer has Settings and Quit. It never shows in Alt+Tab or the taskbar. It is one `AppWindow` with a borderless `OverlappedPresenter` (no title bar, not resizable, always on top), created lazily on the first click and destroyed after it has been hidden for a timeout. Exact width, spacing and corner values are tuned in the prototype (S11) against the real Windows 11 flyouts, not guessed here. The right-click menu is a native menu (Settings, Quit).
- **Settings window**: the Windows Settings app idiom. A `TitleBar` with system caption buttons and `ExtendsContentIntoTitleBar`, Mica as the base material, `NavigationView` with a left pane (General, Keyboard, Automatic brightness), a page header, and setting rows as cards (`SettingsCard` and `SettingsExpander` from the Community Toolkit if S11 passes, otherwise in-house styled rows with the same look). `InfoBar` for warnings (for example camera permission denied, adaptive brightness could not be changed), `ToggleSwitch`, `NumberBox`, `ComboBox` and `Slider` for controls. The calibration flow is a `ContentDialog` with the Laptop slider and the points list. The window is created on demand and destroyed on close.
- **Materials**: Mica for Settings (sampled once, low cost). The flyout uses the desktop Acrylic backdrop because system flyouts are transient light-dismiss surfaces; the research recommended solid or Mica to save GPU, but Acrylic costs only while the flyout is visible, and the window is hidden and then destroyed when idle, so native appearance wins; S10 and S11 measure it and a solid fill is the fallback if idle or visible cost is too high. Windows replaces both with a solid fill when transparency is off, in Battery Saver or in high contrast, and the app adds nothing on top.
- **Motion and accessibility**: animations honor `UISettings.AnimationsEnabled`; all controls get UI Automation names (slider names include the monitor name); keyboard order and focus visuals use system defaults; the manifest declares per-monitor DPI v2; layouts use no fixed heights so 225% text scaling does not clip; the presence status is never conveyed by color alone.
- **Notifications**: none in the first release. Status and errors appear in the flyout and Settings (as on macOS), which avoids the unpackaged toast registration whose C++ path is unverified. Revisit later.
- **Tray icon**: `NIF_GUID` with `NOTIFYICON_VERSION_4`; the icon identity is tied to the executable path, so the installer uses one fixed per-user path. Windows 11 may place a new icon in the overflow; the README explains how to pin it (Settings > Personalization > Taskbar > Other system tray icons, name to confirm on the target build). Tray icons come in light and dark `.ico` sets chosen from the system theme.
**Why**: the owner wants the Windows app to feel like part of Windows 11 the way the macOS app feels like a Mac app. Using the platform's own controls, materials, type and behaviors is the only reliable way to get that and to inherit theming, high contrast and accessibility.
**Alternatives Considered**: a pixel copy of the macOS popover and tabs (feels foreign on Windows); a custom-skinned UI (breaks high contrast and theming, higher maintenance); a Win32/Direct2D owner-drawn flyout (lighter, but cannot match Fluent controls and accessibility without large effort).
**Trade-offs**: the borderless flyout combination (border, corners, Alt+Tab exclusion) is documented only piecemeal for unpackaged WinUI 3, so it is a prototype gate; Acrylic costs GPU while visible; the Community Toolkit dependency is conditional on S11.

## Architecture Overview

Before: one native shell (SwiftUI/AppKit) feeds the core with platform facts and executes its `pending_actions`; Windows is only a documented intent. After: a second shell does the same job with Windows APIs. The core and inference crates stay the single owner of policy, settings, camera lifecycle deadlines, ambient curve math and the presence decision. Only three things are added on the Rust side: the DirectML provider value and availability query, a header comment fix about the clock, and an empty allowlist asset.

### Component Structure

**New Files/Modules:**
- `native/windows/TelaViva.sln` - solution (app, platform lib, tests).
- `native/windows/Platform/` - static library, no XAML:
  - `CoreBridge.{h,cpp}`, `JsonCodec.{h,cpp}` - C ABI wrapper over in-box `Windows.Data.Json`, serialized FFI, revision fan-out.
  - `DisplayCatalog.{h,cpp}`, `MonitorIdentity.{h,cpp}` - enumeration, EDID id, HMONITOR to target mapping.
  - `DdcController.{h,cpp}`, `DdcTransaction.{h,cpp}`, `DdcTimingProfile.{h,cpp}` - dxva2 transport and action execution.
  - `PowerEvents.{h,cpp}`, `SessionEvents.{h,cpp}`, `ScreensaverMonitor.{h,cpp}` - hidden-window observers and polling.
  - `AudioOwner.{h,cpp}` - default endpoint tracking and container-id ownership.
  - `KeyHook.{h,cpp}`, `KeyIntentRouter.{h,cpp}` - hook thread, route table, port of the router logic.
  - `CameraBackend.{h,cpp}`, `CameraLifecycleDriver.{h,cpp}`, `CameraSelection.{h,cpp}`, `InferenceHost.{h,cpp}` - capture, lifecycle events, detector and provider selection.
  - `LaptopPanel.{h,cpp}`, `AdaptiveBrightness.{h,cpp}`, `CalibrationSession.{h,cpp}`, `AmbientCalibrationController.{h,cpp}`.
  - `EfficiencyController.{h,cpp}`, `Scheduler.{h,cpp}`, `Diagnostics.{h,cpp}`, `AppPaths.{h,cpp}`.
- `native/windows/TelaViva/` - WinUI 3 app: `App`, `TrayHost` (Shell_NotifyIcon V4 with a GUID, context menu via native menu), `PopupWindow` (the tray flyout, Decision 15), `MonitorCard`, `SettingsWindow` (NavigationView pages General, Keyboard, Automatic brightness), `CalibrationDialog`, `Strings/en-US/Resources.resw`, `Strings/pt-BR/Resources.resw`, tray `.ico` sets (light and dark), `app.manifest`.
- `native/windows/Tests/` - plain executables: `ddc_transaction`, `monitor_identity`, `key_intent_router`, `camera_lifecycle_driver`, `calibration_session`, `laptop_availability`, `efficiency_policy`, `provider_selection`, `popup_layout`, `core_bridge_smoke`.
- `packaging/windows/` - `build-app.ps1`, `Install-TelaVivaOLED.ps1`, `Uninstall-TelaVivaOLED.ps1`, `README.md`.
- `tools/inference/fetch-windows-runtime.ps1` - downloads the pinned ONNX Runtime zip into an empty directory, verifies SHA-256, extracts only the DLL and licenses.
- `assets/inference/acceleration-profiles.toml` - allowlist, shipped empty.
- `.github/workflows/windows.yml` - CI.
- `specs/archived/windows-version/validation/` - hardware checklist and measurement notes (created during implementation).

**Modified Files:**
- `crates/telaviva-inference/src/lib.rs` - provider kind enum, DirectML candidate and availability, CoreML code behind `cfg(target_os = "macos")`, selected-provider report, self-test hook.
- `include/telaviva_inference.h` and `crates/telaviva-inference/src/header_contract_tests.rs` - additive provider value and function, constants table.
- `include/telaviva_core.h` - comment fix only ("The core reads no clock" is true for the camera lifecycle; `presence_observation` compares `captured_at_ms` with wall-clock time).
- `assets/model/manifest.toml` - Windows runtime variants (`cpu` now, `directml` reserved) with pinned SHA-256 values.
- `licenses/`, `ARCHITECTURE.md`, `README.md`, `CHANGELOG.md`, `adr/` - provenance for the Windows runtime, status text, and decision records (done in `finalize`).

### Component Relationships

`CoreBridge` is the only holder of core handles. Sources (`DisplayCatalog`, `PowerEvents`, `SessionEvents`, `ScreensaverMonitor`, `CameraBackend`, `LaptopPanel`, `AudioOwner`, `KeyHook`) submit commands through it. After each revision, `CoreBridge` publishes the parsed snapshot to `DdcController` (executes `pending_actions`), `CameraLifecycleDriver` (mode, capture mode, `next_sample_in_ms`), `KeyIntentRouter` (route table), `LaptopPanel` (follow enabled, calibrating), `ScreensaverMonitor` (poll on/off), `AudioOwner`, `EfficiencyController`, and the UI view models. The UI submits `update_settings`, `set_brightness` and `set_volume` through the same bridge.

### Data Flow

- **Presence dim (US-002)**: `CameraLifecycleDriver` opens a shared capture, receives a frame, calls `tv_inference_detect_bgra`, submits `presence_observation` with `captured_at_ms`, `camera_generation` and status; the core decides, queues `set_brightness`; `DdcController` writes, waits, reads back, and returns `hardware_result`; return checks use the 1 s cadence from the snapshot.
- **Laptop follow (US-004)**: `LaptopPanel` reads the panel level and submits `builtin_display`; the core maps through the monitor's curve and queues writes; the lid or display state changes `available`, pausing follow.
- **Volume keys (US-006)**: hook callback reads the route, swallows, posts intent; the router builds `set_volume` with `connection_generation`; completion arrives via `hardware_result` and updates the rebase guard.
- **Acceleration (US-007)**: `InferenceHost` enumerates adapters, matches the allowlist, loads the matching runtime path once, creates the session with the requested provider, runs the self-test, and falls back to CPU on any failure.

## Implementation Approach

### User Story Mapping

**US-001: Control external monitors** - Files: `DisplayCatalog`, `MonitorIdentity`, `DdcController`, `DdcTransaction`, `MonitorCard`, `PopupWindow`. Dependencies: core `monitor_discovered`, `hardware_result`. Testing: fake `IDdcPort` for transaction, retry and readback; identity tests with EDID fixtures including duplicate serial-less monitors; hardware rows per monitor and dock.

**US-002: Presence dimming** - Files: `CameraBackend`, `CameraLifecycleDriver`, `CameraSelection`, `InferenceHost`, `CoreBridge`. Testing: lifecycle driver with a fake camera and fake clock against recorded event scripts; error-to-status mapping table tests; fixture detection through `tv_inference_detect_bgra`; hardware rows for privacy denial, sharing with a video call, and two cameras.

**US-003: Battery suspension** - Files: `PowerEvents`. Testing: fake power events produce `power` commands; hardware row "Not run" on this desktop.

**US-004: Laptop follow** - Files: `LaptopPanel`, `AdaptiveBrightness`, `CalibrationSession`, `AmbientCalibrationController`, Automatic brightness tab. Testing: fake WMI and power API for availability, quantization to reported levels, calibration anchor within 2 points, session restore after a simulated crash; hardware rows "Not run" without a laptop.

**US-005: Desktop behavior** - Files: `LaptopPanel`. Testing: this machine runs the real path (no internal target, WMI returns no instances) and must show the disabled reason; fake variants cover brightness-not-adjustable and API-unavailable.

**US-006: Volume and mute keys** - Files: `KeyHook`, `KeyIntentRouter`, `AudioOwner`. Testing: ported router vectors, route-table race tests, hook watchdog test with a fake clock; hardware rows for container-id matching per GPU vendor.

**US-007: Low-energy inference** - Files: `InferenceHost`, inference crate changes, `acceleration-profiles.toml`, `fetch-windows-runtime.ps1`. Testing: `provider_selection` tests with fake adapters and failure injection (registration error, self-test mismatch, runtime error, marker reuse); Rust tests for provider constants and the header table; measurement notes per engine; NPU stays "Not run".

**US-008: Efficiency mode** - Files: `EfficiencyController`, `Scheduler`. Testing: policy tests with a fake `IEfficiency` (startup request, UI exemption, forced-mode detection, no reassert); measurement of presence and return latency with EcoQoS on and off.

**US-009: pt-BR and naming** - Files: both `.resw` sets. Testing: a test that every key exists in both locales and that no Windows resource contains "MacBook" or "Mac".

**US-010: Launch at login** - Files: `AppPaths`, settings General tab, installer scripts. Testing: fake registry for write, read-back and failure paths; install and uninstall script run in CI.

**US-011: Windows 11-native interface** - Files: `TrayHost`, `PopupWindow`, `MonitorCard`, `SettingsWindow`, `CalibrationDialog`, `.resw` sets, `app.manifest`, tray `.ico` sets. Dependencies: Windows App SDK controls and backdrops, optional Community Toolkit `SettingsControls`. Testing: layout logic in `popup_layout` with a fake DPI and text scale; resource tests for UI Automation names present for every control key; a manual visual and accessibility review (light, dark, high contrast, 100/150/225%, Narrator, keyboard) recorded under `specs/archived/windows-version/validation/`; idle measurement with the flyout hidden and destroyed.

## Integration Points

- Core C ABI v1 (`include/telaviva_core.h`) used unchanged; inference ABI gets one additive provider value and one availability function (ABI stays 1).
- Shared `settings.json` schema 1; `brightness_key_step` stays in the schema and is unused on Windows (Keyboard tab shows volume only).
- Windows APIs: dxva2, DisplayConfig, registry EDID, WMI `root\wmi`, powrprof, WinRT `MediaCapture`, MMDevice, `Shell_NotifyIcon`, DXGI, ThreadPool timers.
- Third party: ONNX Runtime 1.20.1 (MIT) pinned by SHA-256 after local verification, Windows App SDK (source MIT on GitHub, but the redistributed NuGet runtime ships under Microsoft Software License Terms, not MIT; see `licenses/DEPENDENCIES.md`; open decision for the owner, see `adr-notes.md` section 10), in-box DirectML, GitHub-hosted `windows-latest` runner with Visual Studio 2022 and Windows SDK 26100.

## Technical Constraints

- Open-source redistribution only; Windows system components are platform facilities (`AGENTS.md`, ADR 0001, FR-022).
- Camera frames never cross the core ABI and are never stored, logged or sent; device identifiers are not logged.
- The Windows shell never calls the core from inside its snapshot callback.
- `tv_inference_*` runtime path is bound once per process (a different path returns `-2`), so the runtime variant is chosen before the first detector is created.
- The core uses wall-clock time in `presence_observation` (60 s freshness) and `steady_clock` seconds in the camera lifecycle; the shell must keep both consistent per event.
- `selected_camera_id` and `camera_history` are never written by the full-settings payload (same rule as macOS).

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| WinUI 3 idle CPU or memory exceeds the budget | Fails SC-003 and the efficiency goal | Spike S10 before UI work; fallback is a tiny Win32 resident agent with the UI as a separate on-demand process (new ADR) |
| Rust DLL exports fail on MSVC | No link | Spike S1 (`dumpbin /exports` against both headers) |
| Monitor id collisions or port-dependent mapping | Wrong monitor written, orphaned curves | EDID-based id without path, `ambiguous` read-only for identical serial-less monitors, validation by EDID bytes (S2) |
| DDC unreliable on resume, docks, some drivers | Missed restores, burn-in risk | Core bounded retries, 2 s wake grace, `DdcTimingProfile`, monitor matrix in the checklist, UI never claims success without readback |
| Audio endpoint to monitor match is inferred | Volume keys hit the wrong device or never work | Container-id equality only, pass-through otherwise, hardware rows per GPU vendor (S2b) |
| Hook silently removed on timeout | Keys stop working | Trivial callback, dedicated thread exempt from EcoQoS, watchdog reinstall; forced Efficiency mode may still delay it, logged |
| Unpackaged `MediaCapture` behavior with sharing and privacy | Camera status wrong or camera blocked | Spike S3 on this machine (RGB and IR cameras); `IMFSourceReader` fallback behind `ICameraBackend` |
| DirectML or provider error or slower than CPU | Energy or latency regression | Allowlist-only, self-test, failure marker, CPU fallback, `disable_cpu_ep_fallback` in the self-test |
| In-box `DirectML.dll` version on older Windows 11 builds | DirectML runtime fails to load | Spike S4 checks 22H2 and 24H2 builds; the DirectML runtime variant is only shipped with an allowlist entry; CPU runtime is the default |
| NPU path undocumented for Rust | NPU not delivered | Deferred; provider value 3 reserved; ADR for `ort` upgrade only after spike S5 |
| WMI brightness events or poll cost | Follow latency or CPU | Spike S7 (laptop required, otherwise "Not run"), interval widening to 2 s, documented |
| Adaptive brightness GUID and admin needs unverified | Calibration cannot toggle | Spike S7, fallback to a warning-only mode with the notice already used for failed toggles |
| `.resw` unpackaged loading and language switch | Missing strings, restart needed | Spike S9, generated string table fallback |
| Self-contained size and SmartScreen warning | Heavy download, user friction | Measure size in CI; document the warning |
| Router logic duplicated in Swift and C++ | Drift | Shared test vectors, ADR note to move it into the core later |
| Borderless flyout combination (no border, rounded corners, hidden from Alt+Tab, light dismiss) behaves badly unpackaged | Flyout looks or behaves non-native | Spike S11 on this machine; Win32 fallbacks (`DWMWA_WINDOW_CORNER_PREFERENCE`, `DWMWA_BORDER_COLOR`, tool-window style) behind one helper |
| Acrylic flyout or WinUI visuals raise GPU or CPU use | Fails the efficiency goal | Flyout window lazy and destroyed after a hidden timeout, no timers while hidden, measurement in S10 and S11, solid fill fallback |
| Community Toolkit `SettingsControls` not compatible with Windows App SDK 2.x | Extra work or an unwanted dependency | In-house styled rows with the same look; decision at S11 |
| Windows 11 puts the new tray icon in overflow | Users cannot find the app | README and first-run hint explain pinning; icon identity fixed by `NIF_GUID` and one install path |
| Dev machine is a desktop | Laptop, battery, NPU rows unverifiable | Rows recorded "Not run", no support claims, fake-backed tests cover the logic |

Phase 0 spikes (each ends with a short validation note and a go or no-go): **S1** DLL exports and link; **S2/S2b** monitor identity mapping, DDC read and write on the monitors attached here, container-id audio match; **S3** unpackaged `MediaCapture` shared read and enumeration with the RGB and IR cameras; **S4** DirectML runtime session on the Radeon iGPU and RTX GPU with energy measurement; **S5** Windows ML NPU route (likely "Not run" here); **S6** efficiency detection; **S7** WMI brightness events and adaptive brightness (laptop required); **S8** EcoQoS effect on camera delivery, hook latency and timers; **S9** `.resw` loading unpackaged; **S10** WinUI 3 idle cost; **S11** native flyout and Settings prototype (borderless flyout window, anchoring, corners, Acrylic and Mica, Alt+Tab exclusion, `SettingsControls` compatibility, light, dark and high contrast, 100/150/225% scaling) compared side by side with the Windows 11 Quick Settings and Settings app.

## Open Questions

None blocking. Items the reviewer may want to confirm, all with a stated default in this design: identical serial-less monitors are read-only in the first release (Decision 5), Windows' own volume OSD does not show when a key is swallowed (Decision 9), and the first release ships CPU inference everywhere because the allowlist is empty (Decision 3).

## Next Steps

1. Review this architecture and [contracts.md](contracts.md).
2. Resolve any open questions.
3. Run `/codeadvisor-dev:plan`.
