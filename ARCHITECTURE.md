# TelaViva OLED Architecture

## Overview

TelaViva OLED is a planned desktop application for macOS and Windows that controls the brightness and volume of external monitors through DDC. It adds configurable OLED-oriented brightness protections using webcam presence and screensaver state, with minimal computer resource usage as a central requirement.

This document records agreed intent with implementation in progress. The repository now contains shared Rust policy and inference crates, a native macOS shell and a Windows 11 x64 shell in progress (see the windows-version section below). Current evidence is recorded under `specs/archived/external-monitor-control-and-oled-presence-protection/validation/`; source code and successful tests do not imply verified device support. Reducing brightness to zero is a requested monitor brightness setting; its physical effect and suitability for OLED protection must be evaluated for supported hardware.

### Agreed requirements

- Control external monitor brightness and volume through DDC where the monitor and connection support the requested capability.
- Offer optional webcam presence detection with a configurable 10-second default interval while the user is present; confirm absence after two valid missed checks.
- Detect any face; do not require identifying the user or enrolling a face.
- When no face is detected, reduce the external monitor brightness to zero and preserve the previous brightness for restoration (see the 2026-10-21 section for the current levels: protection level 1–50, threshold 0–90).
- While presence-dimmed, check for return at a configurable 1-second default interval and restore the prior brightness, subject to other active protections.
- Offer an opt-in screensaver brightness reduction with a configurable target defaulting to zero (see the 2026-10-21 section for the current levels: protection level 1–50, threshold 0–90). Apply the strongest active reduction and restore only after applicable protection reasons clear.
- Offer a configurable option to disable presence checking on battery, ON by default; stop camera acquisition and face detection while suspended.
- Process camera data locally without storing images or video; use camera sharing where supported and validated.
- Suspend face inference when lighting is insufficient rather than treating it as absence. The implemented core clears presence-only dimming, checks light every 10 seconds, and requires two usable-light observations before resuming inference; screensaver protection remains independent. Live-camera behavior still requires hardware validation.
- Provide a tray/menu-bar popup with single-click brightness and volume controls for multiple monitors, plus settings and quit. Initial UI locales are pt-BR and en-US. Each native shell owns its UI strings; the core keeps the `locale` setting and returns status codes rather than translated text ([ADR 0015](adr/0015-native-shells-own-ui-strings.md)). Documentation remains en-US.
- Use low-energy hardware acceleration, such as an integrated GPU, when feasible, with software fallback.
- Minimize CPU, memory, camera, and energy usage.

## Stack

- Repository architecture: modular monolith, delivered as a desktop application.
- Initial target operating systems: macOS on Apple Silicon and Windows 11 x64 (ARM64 later; Windows 10 out of scope). Intel Mac support is deferred; affected M1 monitor/connection limitations are accepted and surfaced explicitly.
- Implementation: Rust for shared application policy and inference, with Swift/macOS and C++/Windows presentation and device adapters.
- Build tooling: Cargo for the shared core, Xcode for the macOS shell, MSBuild for the Windows shell; one repository and modular application.
- UI: SwiftUI/AppKit on macOS and C++/WinRT with WinUI 3 on Windows 11; shared Rust core through a versioned C ABI. No web frontend runtime.
- Face detection: bundled YuNet model with ONNX Runtime via ort. Windows offers Automatic (the default), CPU, or a specific GPU. Automatic uses an integrated GPU through OS-supplied DirectML when it passes the per-session self-test and 250 ms latency budget, and CPU otherwise; a discrete GPU is manual-only. macOS on Apple Silicon offers Automatic (the default), Neural Engine preference, GPU preference, and CPU. Automatic requests all CoreML compute units after platform validation; each session still validates locally and falls back to CPU on failure. CoreML preferences do not guarantee physical per-operator placement. The self-test compares the bundled frame's presence classification with CPU and does not establish universal result equivalence or energy savings. NPU routes are out of scope.
- DDC: Win32 physical monitor/VCP APIs on Windows; isolated IOAVService bridge based on the MIT-licensed m1ddc approach on Apple Silicon.
- Project license: MIT; see `LICENSE`. Third-party libraries and assets retain their own licenses and notices.
- Dependency policy: redistributed libraries, runtimes, and assets must be open source with retained licenses and provenance. Native operating-system APIs are platform facilities.

See [the archived feature architecture](specs/archived/external-monitor-control-and-oled-presence-protection/architecture.md) and [archived runtime contracts](specs/archived/external-monitor-control-and-oled-presence-protection/contracts.md) for decisions, alternatives, planned structure, acceptance budgets, and compatibility gates, and [api.md](api.md) for the implemented native consumer interfaces. These selections are design intent; hardware behavior is not yet verified.

A Cargo workspace now contains `telaviva-core` and `telaviva-inference`. The current Rust implementation groups policy in `crates/telaviva-core/src/core.rs`, settings in `settings.rs`, the ABI in `ffi.rs`, the camera lifecycle in `camera_lifecycle.rs`, the controls handle in `controls/`, the strict JSON parser in `strict_json.rs`, and image inference in `crates/telaviva-inference/src/lib.rs`. The planned module table in the feature architecture describes responsibility boundaries; further file splitting can follow complexity without changing the contracts.

## Modules

The following are intended responsibility boundaries within one application, not existing crates or directories:

- Monitor control: discover external displays, inspect supported controls, and read/write brightness and volume through platform-specific DDC integrations.
- Presence detection: acquire camera samples and detect faces according to the active sampling policy.
- Protection coordination: combine presence and screensaver conditions, preserve previous brightness, and coordinate restoration.
- Operating-system integration: observe screensaver and battery/power state on macOS and Windows.
- Settings and interface: expose per-monitor controls, configurable intervals, protection options, language selection, and application status; persist preferences locally. Native per-OS presentation consumes shared core commands and snapshots.

## Main Flows

### Manual monitor controls

The user selects an external monitor and adjusts its brightness or volume. The application uses the corresponding DDC capability when available. Discovery, unsupported-capability reporting, retry limits, and connection behavior require design and hardware validation. The retry budget is decided (3 attempts per primitive, 0.2 s then 1.0 s) and applies on both platforms; hardware validation is pending. On both platforms, drafts, coalescing and the DDC retry for these controls run on the core controls handle (see [Controls handle](#controls-handle)). Brightness and volume values are stored as whole raw units (see [External monitor reconnect restore and whole-unit manual values](#external-monitor-reconnect-restore-and-whole-unit-manual-values--2026-10-21)).

### Presence protection

1. When presence detection is enabled and permitted by the power policy, sample the webcam at the configured N-second interval.
2. After two valid no-face checks, preserve the monitor's previous brightness once and request brightness zero (see the 2026-10-21 section for the current levels: protection level 1–50, threshold 0–90).
3. Distinguish absent, insufficient-light, and camera-error observations; suspend face inference for insufficient light rather than count it as absence.
4. While presence-dimmed, check for return at a configurable 1-second default interval. On return, restore the previous brightness subject to any other active protection condition.

Two valid missed checks confirm absence. The confirming check is taken after the short return interval, so dimming takes about one sampling interval plus about a second. Camera errors and insufficient illumination do not count as valid no-face observations. Detailed recovery latency, camera errors, and permission handling remain technical design and validation matters.

### Screensaver protection

When screensaver protection is enabled and the screensaver becomes active, reduce external monitor brightness to its configurable target (default zero; see the 2026-10-21 section for the current levels: protection level 1–50, threshold 0–90). Apply the strongest active reduction and restore only after applicable protection reasons clear. Screensaver protection starts disabled; lock and display sleep are outside the initial trigger scope.

### Battery policy

When running on battery and the user has enabled the battery exception (ON by default), stop camera acquisition and face detection. If presence was the only dimming reason, restore its reduction; retain independent screensaver dimming. Re-evaluate presence when the policy permits it again.

### Implementation validation gates

- Minimum macOS and Windows versions, supported hardware, and monitor/connection compatibility remain validation dependencies.
- Validate native camera lifecycle, permission handling, shared-camera behavior, and the designed low-light recovery policy.
- Validate brightness ownership across manual changes, application exit, suspend/resume, and monitor reconnection against the policies in specs/archived/external-monitor-control-and-oled-presence-protection/spec.md.
- Measure CPU, memory, energy, and camera use against the engineering budgets in the feature architecture; no benchmark results are claimed.
- Validate packaging, launch-at-login, native library loading, macOS screensaver observation, and the selected dependency/model compatibility.

The C headers in `include/` are checked against the Rust C ABI by the `header_contract` tests in `crates/telaviva-core/tests/` and `crates/telaviva-inference/src/`.

## CodeAdvisor Conventions

- language: en-US
- docs_location: root
- project_skills_dir: none
- adr_policy: full
- issue_tracker: none
- branch_template: <type>/<short-description>
- commit_types: feat, bug, hotfix, chore
- sonarqube_mode: none
- platform_scope: every new feature is implemented on both macOS and Windows in the same change unless the owner scopes it to one platform

Keep permanent decision records in `adr/`. When a feature is finalized, preserve its reasoning in an ADR and archive its specification rather than discarding it. No project-specific skills directory is configured. SonarQube connectivity is not configured and can be determined when analysis is first requested.

Documentation and commit messages default to en-US; change the `language` convention here if the team chooses another language. No existing Git history or branch pattern was available during initialization.

## external-monitor-control-and-oled-presence-protection — 2026-10-16

The current macOS application consists of a native SwiftUI/AppKit shell, a shared Rust policy/settings core, and a separate Rust inference crate. The shell communicates with the core through ABI version 1, using bounded UTF-8 JSON commands, revisioned JSON snapshots, and explicit buffer ownership. The core decides protection state and action generations; native adapters handle operating-system observations and device calls. Camera frames stay within capture/inference and do not enter the core ABI.

The policy combines presence and screensaver reasons per external monitor, preserves raw brightness baselines, and checks current hardware state before restoring values the app owns. It requires two valid absences before presence dimming; low light and camera errors suspend presence. Battery suspension stops camera acquisition. The native macOS implementation and source/test evidence are recorded in the feature validation reports. A single Dell on one Apple Silicon host has read-only brightness and volume evidence; no physical write or restoration is verified.

The design selects YuNet through ONNX Runtime with CPU fallback. CoreML is a candidate, and no acceleration energy benefit has been established. The Windows native shell is in the repository, and its hardware verification is pending (see the checklist in [specs/archived/windows-version/validation/hardware-checklist.md](specs/archived/windows-version/validation/hardware-checklist.md)). Remaining gates include camera capture/sharing and permissions, screensaver startup behavior, monitor writes/restoration and broader connection coverage, active resource/energy measurements, UI acceptance, signing/notarization, and clean-account installation. Therefore this architecture record describes an implementation in progress and local testing bundle, not release readiness.

### Permanent documentation and archive

The original [feature specification](specs/archived/external-monitor-control-and-oled-presence-protection/spec.md), [design](specs/archived/external-monitor-control-and-oled-presence-protection/architecture.md), [contracts](specs/archived/external-monitor-control-and-oled-presence-protection/contracts.md), [plan](specs/archived/external-monitor-control-and-oled-presence-protection/plan.md) and [validation records](specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md) are archived as historical evidence. Earlier sections above are retained; their proposed module tree and validation wording must be read alongside this implementation update. The former architecture/contract paths contain navigation pointers. [ADR 0002](adr/0002-external-monitor-control-and-oled-presence-protection.md) records the finalized rationale, and [api.md](api.md) documents the implemented native consumer interfaces. Archival does not complete outstanding Windows or hardware acceptance work.

### Current implementation details

- Policy/settings/ABI live in the core crate. The originally proposed separate Rust adapter and worker modules were not built, except the controls handle in `crates/telaviva-core/src/controls/` (see the [Controls handle](#controls-handle) section), which runs manual controls, the hardware-key policy and DDC dispatch on both Windows and macOS. The inference crate contains YuNet preprocessing and execution.
- Presence sampling accepts 5–300 seconds (default 10); return sampling accepts 0.25–5 seconds (default 1). Older valid schema-1 interval values are clamped on load without discarding other preferences.
- Normal checks capture a single frame. Return checks retain the camera session across deadlines, including the 5-second return interval; unused frames are discarded and inference remains limited to the chosen cadence. Disabled, suspended and shutdown states stop acquisition. Low-light recovery samples light every 10 seconds.
- The camera snapshot separates operational status from presentation status: `display_status` carries language-neutral codes such as `checking`, `ready`, `insufficient_light`, `error` and the `suspended_*` reasons, and each native shell maps them to its own localized text ([ADR 0015](adr/0015-native-shells-own-ui-strings.md)). Successful fresh observations update operational readiness.
- The native popup aligns monitor controls, caps the protection-brightness slider at current brightness, and opens settings centered in the invoking monitor's visible area. Settings errors are localized and failed login registration is not persisted as successful.
- MIT applies to project code; bundled third-party artifacts retain their notices. Packaging verifies asset digests and includes both project and dependency licenses.

## Camera lifecycle and display suspension — 2026-10-16

The camera lifecycle extension adds an `always_on_camera` preference (on by default since 2026-10-22; existing saved values are kept, see [ADR 0003, amendment 2026-10-22](adr/0003-camera-lifecycle-and-display-suspension.md#amendment-2026-10-22-on-by-default)) to retain capture between scheduled presence checks. Capture eligibility remains controlled by the shared Rust policy; battery suspension, disabled protection, no eligible external monitor, display sleep, camera errors, and shutdown take priority. Retaining a capture session does not increase inference frequency. Return checks retain their existing continuous capture behavior. The archived plan for the 2026-10-22 default change is at [specs/archived/keep-camera-on-by-default-in-advanced/plan.md](specs/archived/keep-camera-on-by-default-in-advanced/plan.md); hardware behavior is not verified.

A retained camera session (always-on, or the dimmed return check) is set once, right after it starts, to a low frame rate (target 2 fps, snapped to the nearest rate the camera supports) and the rate is never changed while it runs, because changing it restarts the stream and blinks the camera LED. Single-frame sessions are not capped. The original durations are restored when it stops. Sampling cadence is unchanged, and hardware measurements are pending; see the 2026-10-16 amendment in [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md).

The native macOS adapter reports per-monitor OS sleep state with connection generations. Camera monitoring suspends when all eligible external displays are asleep; mixed awake/asleep configurations continue monitoring, while sleeping targets receive no DDC writes. Wake reconciliation uses fresh brightness readback and existing ownership rules. A failed wake readback or restore write keeps ownership and is retried up to three times, and a restore write still requires a readback that matches the value the app last set. Brightness zero and screensaver state do not imply physical display sleep. A monitor manually powered off can remain logically awake depending on its connection.

Low-light recovery uses the existing 10-second retry delay, then a five-second monotonic exposure-settling window before assessing a fresh frame. Initial acquisition and the post-settling frame each have a bounded timeout. Warmup frames do not enter inference or absence counting; two usable-light assessments remain required. The implementation plan and separate hardware checklist live under [camera lifecycle and display suspension](specs/archived/camera-lifecycle-and-display-suspension/README.md). Source and automated checks do not establish physical camera or monitor compatibility.

## camera-lifecycle-and-display-suspension — 2026-10-16

This macOS extension adds continuous camera capture (on by default since 2026-10-22; existing saved values are kept, see [ADR 0003, amendment 2026-10-22](adr/0003-camera-lifecycle-and-display-suspension.md#amendment-2026-10-22-on-by-default)), per-display sleep policy, and exposure-settling behavior for low-light recovery, while retaining existing eligibility and brightness-ownership rules. The accepted decision is in [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md); the standalone plan and the physical acceptance checklist are preserved in the [feature archive](specs/archived/camera-lifecycle-and-display-suspension/). On 2026-10-18, ten of the sixteen checklist scenarios passed on one MacBook Pro (Apple M4 Pro) with one Dell S3225QC; one passed in part and five were not run (two external monitors, light recovery, settling interruptions, camera errors, and an external change while asleep). Other monitors and connections remain unverified, and Windows hardware verification is pending.

## camera-brightness-activation-threshold — 2026-10-17

The shared core gates presence checking on a brightness threshold, `presence_min_brightness_percent` (default 20, range 10 to 90 when it was added, strict "above" comparison; see the 2026-10-21 section for the current levels: protection level 1–50, threshold 0–90). Presence checking runs only while at least one eligible monitor is awake and above the threshold; otherwise the camera is suspended with display status `suspended_low_brightness`, or `suspended_no_monitor` when no eligible monitor exists. The core compares the monitor's baseline while the app owns or dims its brightness, so the app's own presence or screensaver dimming does not stop the camera, and the observed level otherwise. The setting is additive to schema 1 and does not change ABI 1.

The macOS shell does not poll monitor brightness. Levels refresh on discovery, display change, wake, the popup refresh button and the app's own writes, so a change made with the monitor's own buttons is noticed only at the next refresh. Periodic DDC reads were not added because they add bus traffic and can disturb some monitors. Hardware behavior remains unverified, and hardware acceptance is pending.

The decision is recorded in [ADR 0011](adr/0011-camera-brightness-activation-threshold.md), and the archived plan is at [specs/archived/camera-brightness-activation-threshold/spec.md](specs/archived/camera-brightness-activation-threshold/spec.md).

## External monitor hardware keys and fluid controls — 2026-10-16

Opt-in macOS keyboard routing adjusts an external monitor's brightness by pointer (key codes 144 and 145), and its volume and mute only while that monitor is the Mac's default sound output, matched by CoreAudio device UID against the display EDID UUID; other targets pass through to macOS. Menu bar sliders use bounded draft coalescing, and DDC writes are serialized through a transaction that verifies readback. The key step, intent, rebase and mute policy run on the core controls handle (see the [Controls handle](#controls-handle) section), and the event tap makes one synchronous call into it to decide whether to swallow the key. Hardware checks are pending on both platforms. The accepted decision is in [ADR 0004](adr/0004-external-monitor-hardware-keys-and-fluid-controls.md), and the feature records are in the [archive](specs/archived/external-monitor-hardware-keys-and-fluid-controls/).

The key step is configurable separately for brightness and volume, from 1 to 15 percentage points per press (default 5), and is stored in the core settings. Shift+Option is a fixed 1-point fine step. The native router receives plain values through `setSteps(brightness:volume:)`, which clamps them and stores them under its state lock. Popup brightness and volume sliders are continuous and are unaffected by the key step.

Behavior is verified only on one LG ULTRAFINE over DisplayPort and one keyboard, and the individual macOS acceptance checklist rows remain not run. Other monitors, docks, HDMI, USB audio and keyboards are unverified, and Windows hardware verification is pending.

## hardware-key-step-settings — 2026-10-16

The key-step settings are two additive schema-1 fields in the core `Settings`, `brightness_key_step` and `volume_key_step`: integers from 1 to 15 percentage points, defaulting to 5 and validated by `Settings::validate`. `update_settings` maps each key to its field, and the snapshot carries both values to the native shell. `MenuBarController` passes them to `HardwareKeyRouter.setSteps(brightness:volume:)` from the snapshot sink. The router stores them under its state lock, and `reserveStep` reads the step for the feature being pressed, so the router keeps no core types. The Shift+Option fine step stays fixed at 1 point.

The router also no longer rebases a key's intent while one of its own accepted hardware writes is outstanding, or within a 1 s settle window after its completion. Acceptance and completion come from `MenuBarController` through `hardwareActionAccepted` and `hardwareActionCompleted`, matched by action generation; an accepted write whose completion never arrives stops blocking after 10 s. Outside those windows, the existing rebase rule still reports external changes and monitor clamping. The decision and its limits are recorded in [ADR 0005](adr/0005-configurable-hardware-key-step.md), and the archived spec is at [specs/archived/hardware-key-step-settings/spec.md](specs/archived/hardware-key-step-settings/spec.md).

## restore-brightness-on-monitor-wake — 2026-10-16

The core's restore retry is bounded. When a wake verification readback or a restore write fails while no protection reason is active and the app still owns the dimmed value, `Monitor.restore_failures` counts the failure and ownership, baseline and error status are kept; `reconcile_all` then re-reads before any retry write. After `MAX_RESTORE_FAILURES` (3) failures, ownership is dropped as before. Dim, manual and volume writes keep their failure behavior. Once shutdown has started, retries are disabled: a failed read or restore write on the shutdown path drops ownership as before, so quitting never waits on retries. Inside the wake readiness window ([ADR 0033](adr/0033-wait-for-ddc-readiness-after-wake.md)), a transient DDC failure of these reads is retried every 2 s without using up the per-primitive attempts, so a monitor that answers within that window normally restores without reaching this counter.

The counter is internal and is not in the snapshot. It resets on a successful `set_brightness`, a display-state change, a manual brightness set and monitor discovery. A successful readback does not reset it, because a restore write that keeps failing while readbacks succeed would otherwise retry without limit. A restore write is still enqueued only after a readback matches the value the app last set. No native shell or ABI change is part of this feature. The decision is recorded in [ADR 0006](adr/0006-keep-brightness-restore-pending-after-failed-wake-readback.md), and the archived spec is at [specs/archived/restore-brightness-on-monitor-wake/spec.md](specs/archived/restore-brightness-on-monitor-wake/spec.md). Hardware behavior on the physical monitor is not verified.

## macbook-brightness-follow — 2026-10-16

The macbook-brightness-follow extension makes external monitors follow the brightness of a MacBook's built-in display. The core owns the follow policy and the mapping from built-in brightness to each monitor's target through a per-monitor four-point curve, exposed to the native shell as a `builtin_display` command. The macOS shell reads and writes built-in brightness through one protocol, implemented by a private DisplayServices/CoreBrightness backend kept behind that protocol. The shell polls the built-in brightness every second, and readings are quantized and debounced before they reach the core. A calibration session fits each monitor's curve and writes a crash-recovery file so that an interrupted session can be recovered. Per-monitor OLED protection participation is a separate setting, so each monitor can opt in or out. The popup height is measured from its content and scrolls beyond three monitors. Settings is a fixed-size window (760 by 720 points, not resizable) with General, Keyboard and Automatic brightness tabs. Brightness keys pass through to macOS for monitors that follow the MacBook.

[ADR 0007](adr/0007-private-display-apis-for-built-in-brightness.md) records the use of private display APIs for built-in brightness, [ADR 0008](adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md) records the core-owned follow policy and per-monitor protection, and the [feature specification](specs/archived/macbook-brightness-follow/spec.md) defines the requirements.

Nothing in this section is verified on hardware. The following remain unverified:

- the built-in brightness setter and the automatic-brightness setter on the private backend;
- the brightness-change callback, which the polling design does not use;
- monitor ID stability across ports and docks;
- NSPopover content sizing for the measured popup height;
- behavior on a Mac without a built-in display;
- lid-closed behavior;
- notarization with private frameworks;
- macOS 14, 15 and 26.

## always-on-camera-resource-optimization — 2026-10-16

The macOS camera path now limits its own cost without changing sampling cadence, hysteresis, presence semantics or the C ABI. A retained capture session (always-on, or the dimmed return check) is set once, right after `startRunning`, to a low frame rate matched against the frame-rate ranges of the active format, and the rate is never changed while the session runs because changing it restarts the camera stream and blinks the LED. Frame durations are only ever taken from, or strictly inside, a range the format reports, because AVFoundation raises an Objective-C exception that Swift cannot catch for anything else; the original durations are restored before the session stops, and a user-defaults switch turns the control off. The camera queue runs at background QoS while its watchdog and settle deadlines run on a separate utility queue, the sample timer carries leeway, and the session preset falls back from VGA to medium and low.

In the inference crate, 640x480 frames skip interpolation when letterboxed into the fixed 640x640 detector input (the output is identical to the generic path, proven against a reference implementation), output names are constants, and ONNX Runtime thread spinning is disabled. Release builds use thin LTO with one codegen unit; `panic = "unwind"` stays because every FFI export relies on `catch_unwind`.

The decision is recorded in the 2026-10-16 amendment of [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md), and the archived plan is at [specs/archived/always-on-camera-resource-optimization/plan.md](specs/archived/always-on-camera-resource-optimization/plan.md). No CPU, energy or wakeup measurement exists yet, and a real capture session with the new rate control has not been verified on hardware. Snapshot publish amplification was deferred to [specs/archived/snapshot-publish-amplification/plan.md](specs/archived/snapshot-publish-amplification/plan.md).

## app-icons-and-presence-status-icon — 2026-10-16

The core owns the latest presence sample, exposed as `camera.presence`; the native shell only renders it. The menu-bar status item is a drawn template image (`native/macos/TelaViva/StatusItemIcon.swift`) that mirrors the editable glyph masters in `assets/branding/menubar/*.svg`. The app icon comes from the Icon Composer document `assets/branding/AppIcon.icon` (Liquid Glass, compiled by `actool` into `Assets.car`) plus an `.icns` rendered from `app-icon.svg` as the macOS 14 and 15 fallback. The generated resources are committed in `packaging/macos/Resources/`, and `packaging/macos/build-app.sh` copies them into the bundle. The decision is recorded in [ADR 0009](adr/0009-app-icons-liquid-glass-and-presence-status-icon.md), and the archived plan is at [specs/archived/app-icons-and-presence-status-icon/plan.md](specs/archived/app-icons-and-presence-status-icon/plan.md).

Known limitations: a settings or monitor change clears the presence sample until the next one arrives. Liquid Glass, Dock, Finder and menu-bar appearance are unverified on real hardware.

## pause-follow-when-built-in-display-is-off — 2026-10-16

Following the MacBook brightness pauses while the MacBook lid is closed or its built-in display is asleep. `DisplayServicesBackend.status()` now classifies those states as unavailable with the reason `displayOffline`, using the lid state (`AppleClamshellState` on `IOPMrootDomain`) and the display's sleep state through the pure function `offlineReason`. `MenuBarController` re-reads availability on display sleep and display wake, plus one delayed check about 2 seconds after a wake, with no new polling. The Rust core is unchanged: `builtin_display { available: false }` already sets `ambient.active` to false, so the popup shows the brightness slider again and the brightness keys control the external monitor again; on return the core writes the mapped value for the current built-in level, replacing a manual change made during the pause.

The decision is recorded in [ADR 0010](adr/0010-pause-follow-when-built-in-display-is-off.md), and the archived plan is at [specs/archived/pause-follow-when-built-in-display-is-off/spec.md](specs/archived/pause-follow-when-built-in-display-is-off/spec.md). The lid-closed and screen-off behavior is not verified on hardware; section I of the [hardware checklist](specs/archived/macbook-brightness-follow/validation/hardware-checklist.md) is not run.

## invert-macbook-calibration-direction — 2026-10-17

The calibration of a monitor against the MacBook is inverted: the four points (25, 50, 75 and 100%) are external monitor brightnesses, choosing a point sets the monitor to that level through `ManualMonitorControls`, and the slider sets the MacBook brightness through the built-in backend. The settings shape (`ambient_curves` with `p25` to `p100`) is unchanged but each value is now the MacBook level at which the monitor looks equal to the monitor at that percent. The core inverts the stored points when following: `curve_percent` is piecewise linear through (MacBook level, monitor percent) knots from (0, 0), clamps to 100% above `p100`, and tolerates equal consecutive points. The ABI and `schema_version` do not change.

`AmbientCalibrationController` enables Save only after the monitor reports the point's level (within 2 points) and refuses to start, or stops a running session, while the MacBook screen is off or the lid is closed, using the live `status()` and `isAsleep()` of the built-in backend (see [ADR 0010](adr/0010-pause-follow-when-built-in-display-is-off.md)). Calibrations saved by earlier development builds use the opposite meaning and must be redone.

The decision is recorded in [ADR 0012](adr/0012-invert-macbook-calibration-direction.md), and the archived plan is at [specs/archived/invert-macbook-calibration-direction/spec.md](specs/archived/invert-macbook-calibration-direction/spec.md). Hardware behavior is not verified; section F of the [hardware checklist](specs/archived/macbook-brightness-follow/validation/hardware-checklist.md) is not run.

## calibration-low-end-points — 2026-10-17

Calibration accepts five optional low-end points at 1, 5, 10, 15 and 20% of the monitor's range, stored as `p1`, `p5`, `p10`, `p15` and `p20` in `ambient_curves`, and the core includes the present ones in the follow curve. The 1% point was added the same day, for the very dim end of the range, and the calibration shows nine point buttons: the four required points and the five optional ones. The completeness rule is unchanged: a curve is complete with the required 25, 50, 75 and 100% points, so existing calibrations keep working and the low-end points only refine dim levels. The decision is recorded in the [2026-10-17 amendment to ADR 0012](adr/0012-invert-macbook-calibration-direction.md#amendment-2026-10-17-optional-low-end-points); hardware behavior remains unverified and pending acceptance.

## camera-selection-history-and-wake-resume — 2026-10-17

The shared core owns `camera_history`, a bounded list of up to eight selected camera IDs in most-recent-first order. `selected_camera_id` stays and is always its first entry, and the macOS shell sends `selected_camera_id` only when the user picks a camera; `camera_history` is read-only through `update_settings`. Selecting a camera moves it to the front without duplicates, and choosing the system default clears both values. The native shell resolves the effective camera when capture starts and whenever a camera connects or disconnects: it uses the first history entry that is connected, and the system default only when none is. The fallback is a runtime decision and is never written back to settings, so a temporarily missing camera keeps its place and is used again after it reconnects. The shell observes the connect and disconnect notifications by their raw AVFoundation names, so the app also builds with SDKs that lack the typed members. The key is additive to settings schema 1 and leaves ABI 1 unchanged, but an older binary that rejects unknown fields may reset a settings file that contains it.

After wake, presence sampling resumes faster. A resume is triggered by an event rather than by a camera-mode transition: an eligible display going from asleep to awake, power returning from battery to AC while battery suspension is on, presence protection being re-enabled, or a monitor rising above the brightness threshold (or the threshold dropping below a monitor's level). The core sets a `resume_pending` flag, clears stale low-light state, and resets `busy` or `unavailable` camera status to `unknown`; `permission` and `error` keep their handling. In face mode the snapshot reports `next_sample_in_ms` as 1 s until the first accepted camera report, instead of the configured interval. Later samples keep `presence_sampling_interval_s`, whose 5 to 300 second range is unchanged, and light-only cadence is unchanged.

A camera that reports `busy` or `unavailable` is also retried on a timer, as described in [camera-failure-retry](#camera-failure-retry--2026-10-23).

In single-frame face mode, a frame assessed as `insufficient_light` is not reported at once. The camera stays on for 5 s, frames are discarded while exposure settles, and one frame is assessed at the end of that window; only that final result is reported. This reuses the light-only settle mechanism, so a dark frame taken during auto-exposure after wake no longer starts the light-only cycle. A stalled stream during the settle remains bounded by the existing frame timeout. Continuous capture modes still report an insufficient-light frame immediately. In a dark room each single-frame face sample keeps the camera on about 5 s longer, and this should be measured on hardware before the trade-off is treated as settled. The detector is pre-warmed when face mode becomes active, so ONNX session creation overlaps with camera start. Debug-level logs in the `wake` category record timings from `screensDidWake` through each applied sample delay, camera start, the first frame, detector readiness and the accepted presence report. Local diagnostic logs of brightness decisions and DDC attempts, including the wake path, are described in [ADR 0034](adr/0034-local-diagnostic-logs.md): the core records each decision, the shells record display sleep and wake, each failed DDC attempt and each port reopen, and a monitor appears only as a short hashed id. Device identifiers are not logged.

The decision is recorded in the 2026-10-16 amendment of [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md), and the archived plan is at [specs/archived/camera-selection-history-and-wake-resume/spec.md](specs/archived/camera-selection-history-and-wake-resume/spec.md). The manual two-webcam, dark-frame and wake-timing checks are not run; hardware acceptance is pending.

## ambient-calibration-add-remove-points — 2026-10-17

The fixed calibration points (1, 5, 10, 15, 20, 25, 50, 75 and 100%, with only the last four required) are replaced by a list the user manages. `ambient_curves[<monitor-id>]` is now `{ "points": [ { "monitor", "builtin" } ] }`: `monitor` is the monitor brightness percent (above 0, at most 100, strictly ascending), `builtin` is the MacBook level at which the monitor looks equal to it (0 to 100, non-decreasing), with at most 32 points per monitor. A curve is complete, and the monitor eligible to follow, with at least two points. The core rejects unsorted lists instead of reordering them, reads the old `pN` keys as points (so saved calibrations are kept and need no recalibration) and writes only the `points` form. `curve_percent` interpolates through (0, 0) and each `(builtin, monitor)` point and now holds the last point's monitor percent above it, instead of jumping to 100%. `schema_version` and the C ABI do not change.

In Settings > Automatic brightness, each monitor lists its saved points with Calibrate and Remove, and an add row with a stepper for the monitor brightness. Add and Calibrate open a dialog with the MacBook slider: Done saves the point (enabled once the monitor reaches that brightness) and Cancel or Escape discards it; the session capture, anchor check and restore order from [ADR 0012](adr/0012-invert-macbook-calibration-direction.md) are unchanged. The optional low-end points of the previous section are now ordinary points. The panel observes `CoreBridge`, otherwise the list goes stale after a save or a removal.

The decision is recorded in [ADR 0013](adr/0013-user-managed-calibration-points.md), and the archived plan is at [specs/archived/ambient-calibration-add-remove-points/spec.md](specs/archived/ambient-calibration-add-remove-points/spec.md). The contract is in [api.md](api.md). Hardware behavior is not verified; section F of the [hardware checklist](specs/archived/macbook-brightness-follow/validation/hardware-checklist.md) is not run.

## core-camera-lifecycle-abi-contract-and-cleanup — 2026-10-17

Camera session timing moved from the macOS shell into `telaviva-core`, as `crates/telaviva-core/src/camera_lifecycle.rs`, and is exposed through the opaque `TvCameraLifecycle` handle of the C ABI: `tv_camera_lifecycle_create`, `tv_camera_lifecycle_step` and `tv_camera_lifecycle_destroy`. Each step takes one JSON event and returns one JSON result with a `value` and a `state` object. The core owns the 5 s acquisition deadline, the 5 s warm-up and settle window, the once-per-sample dark settle for single-frame face mode, the stale-callback check and the assessment token. The shell passes `now` in seconds from its monotonic clock on every time-dependent event, and the core reads no clock. The shell keeps the `DispatchSource` and `asyncAfter` timers, the monotonic clock and AVFoundation, and the Swift `CameraLifecycle` is now a thin wrapper with the same public API. The core's `camera_mode` and `capture_mode` remain the only policy for whether the camera runs, and the lifecycle owns only session timing for the current mode. The change is additive to ABI version 1, so `tv_abi_version()` still returns `1`, and camera frames still never cross the ABI. Frame-rate selection (`chooseFrameRate` and `frameDuration`) stays in Swift in `native/macos/TelaViva/CameraFrameRate.swift`, because it configures the capture device from AVFoundation frame-rate ranges.

The hand-written headers `include/telaviva_core.h` and `include/telaviva_inference.h` are now checked against the Rust ABI by `header_contract` tests in each crate. The tests use only std, so they add no dependency and need no CI change: they pin every exported function with a typed function-pointer cast and compare prototypes, struct fields and constants with the header text. The core snapshot no longer carries `messages`, and the `locales/en-US.ftl` and `locales/pt-BR.ftl` catalogs were removed. Native shells own their UI strings, so on macOS `native/macos/TelaViva/Localization.swift` is the only catalog; the core keeps the `locale` setting and returns status codes rather than display text. The Xcode Release configuration now links the `aarch64-apple-darwin` release libraries instead of the `target/debug/` ones, and live documentation points to the permanent documents instead of a navigation stub for the archived architecture (that stub folder has since been removed). Two follow-ups stay deferred, each blocked by an existing record: the calibration session machine (`AmbientCalibrationController`) stays in Swift because [ADR 0008](adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md) makes calibration a shell-owned session and needs a superseding record first, and the `HardwareKeyRouter` keystroke latch stays in Swift because [ADR 0004](adr/0004-external-monitor-hardware-keys-and-fluid-controls.md) says its event tap never calls the core and [ADR 0005](adr/0005-configurable-hardware-key-step.md) keeps the router standalone.

The decisions are recorded in [ADR 0014](adr/0014-camera-lifecycle-in-the-core.md) and [ADR 0015](adr/0015-native-shells-own-ui-strings.md), the archived plan is at [specs/archived/core-camera-lifecycle-abi-contract-and-cleanup/spec.md](specs/archived/core-camera-lifecycle-abi-contract-and-cleanup/spec.md), and the contract is in [api.md](api.md); camera hardware behavior is not verified, so acceptance is pending.

## windows-version — 2026-10-17

The Windows version adds a second native shell over the same Rust policy core and C ABI. The requirements and design are in [specs/archived/windows-version/spec.md](specs/archived/windows-version/spec.md) and [specs/archived/windows-version/architecture.md](specs/archived/windows-version/architecture.md); this section records only the repository shape and status.

- Shell split: `native/windows/Platform` is a static library without XAML that holds the platform layer and is tested with fake backends. `native/windows/TelaViva` is the WinUI 3 app with the tray host, flyout and Settings. Both are Windows 11 x64 only; ARM64 is a later feature.
- Rust DLLs: `telaviva_core.dll` and `telaviva_inference.dll`, each built as a cdylib and linked by the shell through its import library, rather than two static libraries in one binary.
- Inference ABI: additive only. Provider value `2` is the DirectML candidate, with `tv_inference_create_with_provider_device` and `tv_inference_directml_available`. Value `3` is reserved and rejected (NPU is out of scope). The core ABI stays at version 1, and CoreML code is compiled only for macOS. See [api.md](api.md).
- Runtime: the package ships one ONNX Runtime 1.20.1, the DirectML build, as `onnxruntime.dll`. Its DirectX and DirectML dependencies are delay-loaded, so it also runs the CPU provider where DirectML is missing, and the device can change while the app runs. `DirectML.dll` is the OS copy and is never redistributed.
- Key hook (`KeyHook`): volume and mute keys only, with the route logic ported from macOS; the key policy after the hook (step, intent, rebase, mute memory) runs on the core controls handle (see [Controls handle](#controls-handle)); see the architecture, Decision 9.
- Audio owner (`AudioOwner`): default output matched to a monitor by container ID, with no name fallback; see the architecture, Decision 9.
- Efficiency (`EfficiencyController`, `Scheduler`): EcoQoS while idle, with a user-forced Efficiency mode left in place; see the architecture, Decision 11.
- Provider selection (`InferenceHost`, `DeviceSelection`): the device is decided per stored preference (`auto`, `cpu` or a `gpu:<vendor>:<device>:<ordinal>` id), with one runtime for every session. Automatic picks the first integrated GPU without a failure marker (classified by DXCore `IsIntegrated` matched to DXGI by LUID), validated by a self-test against a CPU reference, and falls back to the CPU; see the section "Inference device selection" below.

Status: the Windows code is in the repository and covered by automated tests. Windows is no longer deferred, but its readiness is not established. Only the hardware checklist rows marked Pass are verified (see the README Windows section). any accelerator energy benefit, clean-profile install, signed distribution and the remaining checklist rows are open, and the Windows App SDK runtime is redistributed under Microsoft Software License Terms.

**Architectural decisions**

- [ADR-0016](adr/0016-windows-shell-winui3-unpackaged-platform-split.md): Windows shell: C++/WinRT WinUI 3, unpackaged and self-contained, with a UI-free platform library
- [ADR-0017](adr/0017-windows-two-rust-cdylibs.md): Windows Rust core and inference as two cdylibs with a C ABI
- [ADR-0018](adr/0018-windows-monitor-identity-from-edid.md): Windows monitor identity from EDID without the connector path
- [ADR-0019](adr/0019-windows-inference-cpu-default-directml-allowlist.md): Windows inference: CPU by default, DirectML only for measured device profiles with a failure marker
- [ADR-0020](adr/0020-windows-efficiency-ecoqos-user-managed-mode.md): Windows efficiency: EcoQoS set once, UI exemption only while a window is visible, user-forced Efficiency mode respected
- [ADR-0021](adr/0021-windows-volume-key-hook-and-route.md): Windows volume and mute keys through a low-level hook, a lock-free route and container-id ownership
- [ADR-0022](adr/0022-windows-presence-camera-shared-mediacapture.md): Windows presence camera: shared read-only MediaCapture, infrared sources excluded
- [ADR-0023](adr/0023-windows-laptop-naming-and-notebook.md): Windows UI says "Laptop" and "Notebook"; core identifiers unchanged
- [ADR-0024](adr/0024-windows-zip-distribution-and-hkcu-run.md): Windows distribution: unpackaged zip with a per-user PowerShell installer and HKCU Run for launch at login
- [ADR-0025](adr/0025-windows-app-sdk-license-versus-open-source-rule.md): Windows App SDK license versus the open-source-only rule (Proposed; owner decision open)
- [ADR-0026](adr/0026-about-screen-and-github-update-check.md): About screen and an informational update check against GitHub releases (Accepted)
- [ADR-0027](adr/0027-selectable-inference-device-and-igpu-offload.md): Selectable inference device and iGPU offload (Accepted)
- [ADR-0028](adr/0028-align-windows-toggles-right.md): Windows Settings toggles right-aligned by resetting the ToggleSwitch `MinWidth` in the shared row builder (Accepted)
- [ADR-0029](adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md): Core-owned manual-control queue, key policy and DDC dispatch behind a clock-injected handle (Accepted for the core and both shells; macOS local test scripts pass, macOS CI and hardware checks pending)
- [ADR-0030](adr/0030-short-confirmation-wait-after-first-no-face-check.md): Short confirmation wait after the first no-face check, so dimming takes about one sampling interval plus about a second (Accepted)
- [ADR-0031](adr/0031-publish-each-snapshot-revision-once.md): Publish each snapshot revision once: a revision accessor in the core, no duplicate macOS refresh, and no republish of an already published revision on macOS and Windows
- [ADR-0032](adr/0032-reopen-ddc-ports-after-topology-change.md): Reopen DDC ports after a topology change or a failed attempt instead of keeping the first transport, without reporting the monitor to the core again (Accepted)
- [ADR-0033](adr/0033-wait-for-ddc-readiness-after-wake.md): Wait up to 30 s for DDC to answer after a display wakes, retrying transient failures every 2 s without using up the per-primitive attempts, in the shared controls dispatcher (Accepted)
- [ADR-0034](adr/0034-local-diagnostic-logs.md): Keep always-on local diagnostic logs of brightness decisions and DDC attempts, with monitors as hashed ids, through a bounded core ring drained by `tv_diagnostics_drain`, the macOS unified log and a rotating Windows file (Accepted)

## About screen and update check — 2026-10-17

The About page is implemented on both shells: a macOS tab in the Settings window and a Windows NavigationView page. On macOS, About is reached through Settings > About. On Windows, it is reached from Settings and the tray right-click menu. The page shows the app icon, name and installed version, the project's GitHub link, the two credits, and an update status (checking, up to date, update available with the remote version, or could not check). Strings are localized per shell in en-US and pt-BR.

The Rust core exports `tv_core_version` and `tv_update_evaluate`. Both are pure and additive to ABI version 1: `tv_core_version` returns the core's version, and `tv_update_evaluate` compares the installed version with a GitHub `releases/latest` answer and returns a status. The core builds the release URL itself and never opens a URL from the server's answer.

The platform HTTP stacks perform the single GET (`URLSession` on macOS, WinHTTP on Windows), off the UI thread, with bounded timeouts. The check runs only when the About page opens or "Check now" is pressed. There is no setting, no persistence and no background polling. The result is cached in memory only: successful outcomes for 6 hours, failures for 30 seconds.

The version has a single source, `crates/telaviva-core/Cargo.toml`. The other copies are checked by `crates/telaviva-core/tests/version_consistency.rs`, and the release rules are listed in [AGENTS.md](AGENTS.md#release-version). The update check is the first outbound request the app makes; the privacy wording in [README.md](README.md#privacy) describes it. Rendered and hardware acceptance of the page is not claimed here.


## Inference device selection — 2026-10-18

A new `inference_device` setting lets the user choose where face detection runs, on the Advanced page of Settings. The requirements, design and tasks are in [the archived feature](specs/archived/inference-device-selection/).

- **Setting:** `inference_device` (string, default `auto`) in settings schema 1, validated by shape in the Rust core: `auto`, `cpu`, `ane`, `gpu`, or `gpu:<vendor>:<device>:<ordinal>` (4 lowercase hex digits each, ordinal 1 to 3 digits, at most 32 characters). The core only stores it; the device list, resolution and status live in each native shell, so the core ABI stays at version 1 and the snapshot is unchanged.
- **Windows (implemented):** `Platform/AdapterCatalog` lists DXGI adapters with their LUID and classifies each as integrated or discrete through DXCore (`IsIntegrated`, matched by LUID, loaded at run time; it never creates a D3D12 or DirectML device, so a sleeping discrete GPU is not woken). `Platform/DeviceSelection` turns the stored id and the catalog into a choice. `InferenceHost` owns the session: `setPreference` releases it and re-decides without a restart, `report()` feeds the page. Automatic picks the first integrated GPU, then the CPU; a discrete GPU is only an explicit choice. Persisted GPU ids use vendor, device and ordinal, never an index or a LUID.
- **Validation:** an accelerated candidate must create a session, attach the requested provider, return `present` for the bundled self-test frame as the CPU reference does, and answer within 250 ms on macOS and Windows, where the budget applies to the first detection of a preference session. It does not have to be faster than the CPU. A failure falls back to the CPU; on Windows under Automatic it writes a failure marker `(device id, driver, app version)` in `inference-failures.json`, and an explicit choice never writes or reads one.
- **Single runtime:** the Windows package ships the DirectML build of ONNX Runtime 1.20.1 as the only `onnxruntime.dll`. The acceleration profile allowlist of ADR-0019 is retired (the file, its parser and its tests are removed).
- **macOS (implemented):** the Advanced page offers Automatic, Neural Engine preference, GPU preference and CPU. `PresenceCamera` owns a testable detector host with a retained session decision and observable report. CoreML uses `MLComputeUnits` string options (`ALL`, `CPUAndNeuralEngine`, `CPUAndGPU`), with CPU reference/250 ms validation, same-frame CPU retry and sticky fallback across idle recreation. Changing the preference resets validation. macOS bundles official ONNX Runtime 1.23.0 with unchanged `ort` rc.9/API 20 bindings; the original 1.22.0 spike exposed an upstream exit-time mutex abort even with CPU sessions, and 1.23.0 passes clean shutdown. Windows keeps its existing runtime.
- **Hardware evidence:** records are in `specs/archived/inference-device-selection/validation/`. On the Windows development desktop (Radeon integrated graphics and an RTX 4080 SUPER, no camera, no NPU) Automatic selected the Radeon, per-LUID utilisation showed only the Radeon busy, and every device returned the same result on the self-test frame. Energy, a real camera, other GPUs and hybrid laptops are not checked.
- **Out of scope:** NPUs (they need vendor runtimes that are not open source), Windows on Arm, Intel Macs, and any energy claim.


## inference-device-selection finalized — 2026-10-18

[ADR 0027](adr/0027-selectable-inference-device-and-igpu-offload.md) amends the inference policy of ADR 0001 and ADR 0019. Both shells now offer a native Advanced page and persist the same device preference while owning their own runtime reports. macOS uses ONNX Runtime 1.23.0 through the existing Rust bindings and CoreML compute-unit string options; it retains validated decisions and CPU fallbacks across the existing idle detector release, with same-frame CPU retry after accelerator errors. Windows keeps its existing 1.20.1 runtime and iGPU policy.

CoreML Automatic was enabled after the Apple M4 Pro host validation: the first self-test detections for ALL, Neural Engine and GPU returned the same presence outcome as CPU within the 125 ms budget then in effect. The budget is 250 ms on macOS and Windows, raised from 125 ms, because the first CoreML inference compiles lazily and an isolated test measured 242 ms for the GPU option; the larger budget leaves headroom for a cold compile. Every session still validates locally. The [archived feature](specs/archived/inference-device-selection/) preserves the specification, runtime-spike diagnosis, provider and host measurements, and remaining acceptance limits. Physical per-operator placement, energy, live-camera timing and idle-resource improvements are not established. Existing Windows UI acceptance remains pending and was outside this macOS completion. That validation used a temporary harness and did not cover the production validator wiring, which until 0.1.2 turned every passing self-test into `session_failed`, so Automatic, Neural Engine and GPU all used the CPU. This is fixed in 0.1.2 (see the 2026-10-19 amendment in ADR 0027), and a hardware check of the corrected build is pending.


## macOS Settings and popup layout — 2026-10-18

The macOS Settings window opens at 760 points wide, with the same width in both the SwiftUI Settings scene and the window opened from the menu-bar popup. It was resizable until 2026-10-23; the window now has a fixed 760 by 720 point size, and tab content that does not fit scrolls. The popup footer exposes Settings and Quit; the About page is reached through Settings > About. This entry-point change amends [ADR 0026](adr/0026-about-screen-and-github-update-check.md). The width is fixed rather than adapted to the available screen area; these dimensions do not establish compatibility with every display scaling setting.

## core-command-queue — 2026-10-18

The feature design is archived in [specs/archived/core-command-queue/](specs/archived/core-command-queue/). This section records what is implemented.

### Controls handle

The core crate exposes an opaque, clock-injected controls handle, `TvControls`, through `tv_controls_create`, `tv_controls_step` and `tv_controls_destroy`; the change is additive to ABI version 1, which stays `1`. Its code is in `crates/telaviva-core/src/controls/` (`model.rs`, `manual.rs`, `keys.rs`, `dispatch.rs` and `retry.rs`), with the strict JSON parser shared with the camera lifecycle in `crates/telaviva-core/src/strict_json.rs`. The handle owns the manual-control draft and coalescing queue for brightness and volume, the key step, intent, rebase and mute policy, dispatch of the core's pending actions, and the DDC retry and pacing policy: three attempts per primitive with 0.2 s and then 1.0 s backoff, a 60 ms settle, a duplicate write only for the GSM manufacturer profile, a 2 s wake grace, and at most three resubmissions of a rejected result. After a display wakes, a transient DDC failure is retried every 2 s for up to 30 s instead of using up the three attempts; permanent errors and readback mismatches are unchanged ([ADR 0033](adr/0033-wait-for-ddc-readiness-after-wake.md)).

The handle takes one JSON event and returns one JSON result, and every event carries the shell's monotonic `now`. The result lists effects (`submit_command`, `run_attempt`, `submit_result` and `draft_changed`) that the shell executes, plus one `next_deadline` for the shell's single timer; the shell keeps the key hook or event tap and its swallow decision, device discovery, running one DDC primitive at a time on its worker, and that timer. The contract is in [api.md](api.md).

On Windows, `ControlsDriver` in `native/windows/Platform` drives the handle and `DdcController` runs one primitive per effect. The core's dispatcher keeps one DDC primitive executing at a time across all monitors. The earlier `ManualControls`, `KeyIntentRouter` and `VolumeActionTracker` are no longer used by `AppController`; their files and their tests are removed, and the ported cases live in the Rust contract and unit tests. `DdcTransaction` and `DdcTimingProfile` remain for discovery and the `--probe-ddc` diagnostic. Two Windows behaviors change: DDC writes are now retried (three attempts), and a rejected result submission is resubmitted at most three times instead of re-running the write on every snapshot. `observe` carries each monitor's normalized percentage. Windows native tests pass (614 tests, 0 failed, 2026-10-18); hardware verification is pending, and no Windows hardware support is claimed here.

The macOS shell runs on the handle too. `ControlsBridge.swift` (the handle wrapper) and `ControlsDriver.swift` (the effect executor and the single timer armed from `next_deadline`) replace the Swift manual-control, key-policy and DDC-dispatch queues. `ManualMonitorControls` is now a UI binding, `HardwareKeyRouter` decodes events, owns the tap and permissions, and makes one synchronous `key` call per event, and `DDCTransaction` runs one attempt per primitive. The Swift queue, receipts and their tests are removed. The app sources type-check and link against the core library, and all 16 local `tools/tests/test-*.sh` scripts pass on an Apple Silicon Mac, including the new `test-controls-bridge.sh`. The full app packaging, the macOS CI run and the macOS hardware checks are still pending, so no macOS hardware support is claimed here. Discovery keeps its own retrying DDC primitives on both platforms. The decision is recorded in [ADR 0029](adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md), which amends ADR 0001, supersedes the router decisions of ADR 0021 for Windows, and amends ADRs 0004, 0005 and 0008 for macOS.

## oled-protection-absence-confirmation-latency — 2026-10-18

[ADR 0030](adr/0030-short-confirmation-wait-after-first-no-face-check.md) changes when the confirming no-face check is taken, not how many are needed. Absence is still confirmed by two valid no-face checks, but after the first one the core reports the short return interval (`return_sampling_interval_s`, 1 second by default) as `camera.next_sample_in_ms`, so the monitor dims about one sampling interval plus about a second after the face leaves instead of about two intervals.

The change lives only in `crates/telaviva-core/src/core.rs` (`absence_confirmation_pending` and the face-mode cadence arm of `snapshot_value`) and is shared by both shells, which already schedule from `next_sample_in_ms`. It adds no setting and no ABI or snapshot-schema change. The low-light handling is unchanged: a dark confirming frame still keeps the camera on for 5 seconds and assesses one frame at the end. Hardware verification of the timing is pending.

## snapshot-publish-amplification — 2026-10-18

Every accepted core command repeated work that only needs to happen once. The change removes those repeats without changing the snapshot content, the callback payload or the C ABI:

- The core has `Core::revision()`, and `tv_submit` reads the revision for the snapshot-ready callback without building a snapshot. The callback payload and the ABI are unchanged.
- macOS `CoreBridge` calls `refresh()` after an accepted submit only while shutting down, and skips a snapshot whose revision is not newer than the last published one.
- The Windows bridge worker skips a revision already published, through the header-only `RevisionGate`. `GeneralPage` and `AutomaticBrightnessPage` keep their poll logic because they also refresh state that does not come from the snapshot.

The decision is recorded in [ADR 0031](adr/0031-publish-each-snapshot-revision-once.md), and the archived plan is at [specs/archived/snapshot-publish-amplification/](specs/archived/snapshot-publish-amplification/). The saving was not measured, and the Windows part is not yet built or run.

## ddc-stale-handle-after-monitor-connect — 2026-10-18

On Windows, connecting a second monitor left the first monitor at "Sem resposta" until the app restarted, because a monitor that stayed connected kept the DDC port it opened at discovery and nothing replaced it. [ADR 0032](adr/0032-reopen-ddc-ports-after-topology-change.md) records the fix: the shells reopen the transport of a monitor that is still present, and never report it to the core again, since a new `monitor_discovered` would reset its generation, pending actions and baseline.

- **Windows:** `DdcController::refresh()` reopens the port of every known, active, stable external monitor (topology change, resume, the refresh button and startup), keeping the old port if the open fails. `DdcController::run()` reopens the port once and repeats the same primitive when it fails with anything other than `unsupported`. `run()` is therefore one primitive, or two after a failure; the core still owns retries and backoff.
- **macOS:** a failed attempt marks the display's cached IOAV service as stale and schedules one discovery pass that replaces it. This path was reviewed by reading only; it was not compiled or reproduced.
- **No core, ABI or snapshot change.** The archived plan is at [specs/archived/ddc-stale-handle-after-monitor-connect/](specs/archived/ddc-stale-handle-after-monitor-connect/). Windows native tests pass (625 tests, 0 failed, 2026-10-18). The owner's manual validation was reported as passed, with no per-step record, so macOS hardware support is not claimed here. Two machines sharing one monitor still compete for the DDC bus.

## brightness-stays-at-zero-after-slow-monitor-wake — 2026-10-19

An external monitor dimmed by presence protection could stay at 0 after a display sleep and wake: the core's wake readback gave up after about 7 s (the 2 s grace plus four failed reads of three attempts each), dropped ownership of the brightness, and nothing restored it. The change has two parts and records what is still open:

- **Wake readiness window.** After `display_power(on)` the controls dispatcher retries a transient DDC failure every 2 s for up to 30 s without using the three attempts of a primitive; permanent errors and readback mismatches are unchanged. The rule lives in the shared core, so macOS and Windows get it together ([ADR 0033](adr/0033-wait-for-ddc-readiness-after-wake.md)).
- **Diagnostic logs.** The core keeps a bounded ring of 256 decision lines, drained through `tv_diagnostics_drain` (additive to ABI version 1), and the shells add display sleep and wake, discovery and DDC attempt lines. A monitor appears only as a hashed id. macOS writes to the unified log (subsystem `com.telaviva.oled`) and Windows to `telaviva.log` of at most about 2 MiB ([ADR 0034](adr/0034-local-diagnostic-logs.md)).
- **Still open.** A monitor rediscovered while dimmed is restored to its baseline only when it returns under the same id at the dimmed level (see the [2026-10-21 section](#external-monitor-reconnect-restore-and-whole-unit-manual-values--2026-10-21)). At another level or under another id it still adopts the reading, and a display sleep during an in-flight dim write is still ignored by the core. The remaining cases are owner decisions in the archived plan, and the new log shows which path happens in the field.

The archived plan is at [specs/archived/brightness-stays-at-zero-after-slow-monitor-wake/](specs/archived/brightness-stays-at-zero-after-slow-monitor-wake/). The Windows changes were not compiled on the development Mac and no hardware check of the wake behavior has been run.

## macos-inference-validator-wiring — 2026-10-19

On macOS, the validator wiring in `PresenceCamera.swift` reported a passing accelerator self-test as `session_failed`, so Automatic, Neural Engine and GPU always used the CPU in 0.1.1. The fix keeps a pass, a failure and a missing owner distinct, with regression tests for the helper and for the detector host. The 250 ms self-test budget is a separate headroom change and was not the cause. The fix was checked by log analysis, an isolated Swift snippet and the regression tests, not on hardware: a hardware check of the corrected build is pending, and where CoreML places the work is still not established. See [ADR 0027](adr/0027-selectable-inference-device-and-igpu-offload.md) for the amendment.

## Laptop display protection and automatic brightness — 2026-10-21

The laptop's built-in screen becomes a protectable, controllable display next to the external monitors. The shared Rust core decides: it evaluates the presence and screensaver reasons for the reserved id `"builtin"` and publishes them in a level-triggered `builtin_protection` snapshot object. The macOS shell applies that decision and holds the screen dimmed until the core reports no reasons and the restore is verified. The built-in screen is not an entry in the core's `monitors` map, because the DDC path does not fit it. [ADR 0035](adr/0035-laptop-display-protection-and-auto-brightness.md) records the decision and [api.md](api.md#laptop-display-protection) the contract. The feature is implemented for macOS only. Windows is documented in [windows-handoff.md](specs/archived/laptop-display-protection-and-auto-brightness/windows-handoff.md) and is not implemented.

### Components

- **Core policy.** `crates/telaviva-core/src/builtin.rs` holds the runtime state of the built-in screen (`protectable`, `holding`, the absence count and the presence dim; never persisted) and the pure reason and target helpers. `crates/telaviva-core/src/core.rs` adds the `builtin_display` handler and freeze rule, the built-in branch of the presence and clear paths, camera gating through the gate display (the monitored display, or the next awake candidate when it sleeps), the follow pause, the monitored display resolver and the snapshot objects. `crates/telaviva-core/src/settings.rs` adds the settings with their validation and the clamped protection level.
- **macOS shell.** `BuiltInProtectionController.swift` reads the baseline, dims, restores with readback and bounded retries, keeps the protection record, and runs quit and launch recovery. `BuiltInAutoBrightnessOwner.swift` is the single owner of the system's automatic brightness setting, shared with calibration. `AmbientCalibrationController.swift` takes that owner and refuses to begin while protection holds the screen. `BuiltInCardView.swift`, `MonitorPopup.swift` and `SettingsView.swift` show the built-in card (protection switch, protection level, brightness slider, no volume) and the Settings controls. `BuiltInBrightnessMonitor.swift` polls for protection as well as for follow, and `BuiltInDisplayBackend.swift` serializes DisplayServices calls with a lock.
- **Camera and lid.** `LidStateMonitor.swift` reports the clamshell state. `PresenceCamera.swift` and `CameraSelection.swift` apply the lid-closed camera rule.

### Data flow

1. Two consecutive absent samples set the built-in presence dim, under the same confirmation as for monitors. The snapshot reports `reasons: ["presence"]`, the protection level as `target_percent` and, with the option on, `hold_auto_brightness`.
2. The shell reads the baseline, writes its record, reports `holding=true`, acquires the automatic-brightness owner when the option is on, and writes `min(baseline, target)`.
3. When a face returns or the screensaver stops, the reasons are empty. The shell writes the baseline, verifies it, releases the owner, deletes both records and reports `holding=false`. The core then accepts reports from the screen again.
4. Quit: `applicationShouldTerminate` ends calibration, calls `restoreNow()` and only then starts `core.beginShutdown`. Once the core is closed it reports no reasons.
5. Launch order: owner recovery, then protection recovery (which starts in holding with the stored baseline), then the built-in monitor start, then calibration recovery.

### Settings

- `builtin_auto_brightness_off_on_protection`: default `false`. Protection touches automatic brightness only when it is `true`.
- `monitored_display_id` (Automatic when `null`) and the read-only `monitored_display_history` (at most 8 display ids): the display whose brightness decides the camera. The camera gate uses the first awake display in the same order, so a sleeping monitored display falls through to the next one.
- `presence_min_brightness_by_display`: a threshold of 0 to 90 per display id, kept for displays that are disconnected. The gate display's own entry applies, or `presence_min_brightness_percent` when it has none. That global value stays the default and is now valid from 0 to 90. The Settings slider edits the monitored display's entry.
- `screensaver_targets` holds the protection level for each screen (applied clamped to 1 to 50 percent, default 1, used for absence and for the screensaver). `protection_disabled` accepts `"builtin"`.

All new keys are optional in schema 1, and the C ABI stays at revision 1.

### Recovery

Two shell records live in `~/Library/Application Support/TelaVivaOLED/`. Each is written atomically before the change it guards and deleted after a verified restore. `builtin-protection-session.json` keeps the unrounded baseline read before the first dim. `auto-brightness-hold.json` exists only while the app has turned automatic brightness off. A restore that fails three times keeps the restore-failed notice and the record; the next snapshot, availability change or launch tries again. If an interrupted calibration's monitor is missing at launch, its restore waits for the monitor, for 90 seconds at most, and its session record stays until then. External monitors have no on-disk record: a dim the core holds on one is kept in memory only, so a quit before the monitor returns loses it, and the next launch adopts the dimmed reading.

### Testing

- Core: `crates/telaviva-core/tests/builtin_protection_contract.rs` and `monitored_display_contract.rs`, plus the settings tests in `crates/telaviva-core/tests/core_contract.rs`.
- macOS: `native/macos/Tests/BuiltInProtectionTests.swift`, `BuiltInAutoBrightnessOwnerTests.swift` and `AmbientCalibrationTests.swift`, run by `tools/tests/test-builtin-protection.sh`, `test-builtin-auto-brightness-owner.sh` and `test-ambient-calibration.sh`. `CameraSelectionTests.swift` covers the lid rule and runs through `test-camera-lifecycle.sh`. The tests use fake backends.
- Hardware: the macOS rows of the feature hardware checklist are pending.

### Verification status

The macOS implementation is in the working tree, and the tests above exist. This section does not record test results. Hardware-verified: nothing. The DisplayServices writes, the automatic-brightness setter, force-quit recovery, lid transitions and the per-display thresholds have not been run on a physical Mac.

### Windows status

Not implemented and not verified. Windows sends neither `protectable` nor `holding`, so the built-in policy stays inert for it. It receives the shared core changes: the snapshot and settings keys its codec ignores, presence absence dimming to the protection level (1 percent by default) instead of 0, and the core accepting presence thresholds from 0 to 90 percent (the Windows slider still uses its own range). The per-display threshold map is not sent by Windows, so the core keeps it. The owner runs the ordered tasks and hardware rows in [windows-handoff.md](specs/archived/laptop-display-protection-and-auto-brightness/windows-handoff.md) on Windows 11. The shared reconnect restore of a protection dim (`parked_dims`) and the whole-unit manual values also apply to Windows DDC monitors once built; their hardware checks are pending.

## External monitor reconnect restore and whole-unit manual values — 2026-10-21

When the app owns the brightness of an external monitor and the device holds a value that differs from the baseline (a protection dim), the core remembers that baseline for the monitor id if the monitor disconnects, or is discovered again in place. When the same id is discovered again with stable identity and supported brightness, the entry is consumed. If the reading is within 0.5 raw units of the dimmed value, the previous baseline (rounded) is restored through the normal ownership path, which reads the device first and then writes, and the log line `dim_resumed monitor=id-xxxxxxxx baseline=N` is written. If the monitor returns at another level, nothing is restored and that level becomes the baseline. An ambiguous or unsupported rediscovery leaves the entry parked. Another id is never restored from it.

The entries are runtime only and are never persisted. External monitors have no on-disk record, so a quit before the monitor returns loses the dim, and the next launch adopts the dimmed reading. Shutdown discards every entry, so quitting never restores a parked dim. At most 16 entries are kept; when a new entry would exceed the cap, the first id in key order is dropped, not the oldest. An entry lasts until a stable, supported discovery of its id, shutdown, or eviction. A resumed monitor starts with no presence reason, so if the user is still away it dims again after two absent checks. No line is logged when parking, and `monitor_discovered` logs the device reading as both raw and baseline before the resume.

On macOS the monitor id includes the port's IORegistry location, which contains a `dispextN@...` segment, so a monitor moved to another port or dock has another id and is not restored. Whether the location also changes after sleep is not verified. Windows ids are EDID based and do not include a port.

Manual brightness and volume values are stored as whole raw units. The slider percent is mapped to the device raw range, rounded to a whole unit and clamped, so the desired value and the baseline are whole numbers. Before this, a fractional baseline such as 45.1513 never matched the whole number that a DDC monitor reports, and the core re-read and re-wrote the monitor about every second (4753 cycles in three hours in the owner's log).

The shared core changes apply to Windows DDC monitors once built. Hardware checks for the reconnect restore and the whole-unit values are pending on both platforms, and this section does not claim a physical restore. The decision is recorded in the [2026-10-21 amendment to ADR 0006](adr/0006-keep-brightness-restore-pending-after-failed-wake-readback.md#amendment-2026-10-21-restore-a-dim-after-reconnect).

## camera-failure-retry — 2026-10-23

A camera that reports `busy` or `unavailable` is no longer left suspended until an unrelated event. The core counts consecutive reports (`camera_failures`, runtime only) and publishes `camera.retry_in_ms`: 30 s after the first, 60 s after the second, then 300 s from the third on. The field is null in every other state, including `permission` and `error`, which need user action, and whenever presence protection is off.

Each shell arms one one-shot timer from `retry_in_ms` for the current camera generation. When it elapses, the shell sends the existing `camera_status` command with status `unknown`. The core then sets `resume_pending`, so the next sample comes after 1 s, and the normal sample path probes the camera again. Other snapshots do not postpone the timer. The counter resets when an accepted sample reports `ready` and on every resume event. The core writes `camera_failure` and `camera_retry` lines to its diagnostic ring, which both shells drain into the macOS unified log and `telaviva.log` on Windows, with no device identifier. The change adds no setting, dependency, network use or ABI change. It also relabels `busy` from "busy" and "ocupada" to "no image" and "sem imagem" on both platforms, because the status does not mean that another app holds the camera.

The decision is recorded in [ADR 0036](adr/0036-camera-failure-retry-in-the-core.md), and the bug plan is at [specs/archived/camera-stuck-busy-after-usb-stall/spec.md](specs/archived/camera-stuck-busy-after-usb-stall/spec.md). Hardware checks are pending on both platforms. The Windows changes are unverified until a Windows build or CI run passes, and the hardware stall itself is not prevented.

## lock-macos-settings-window-size — 2026-10-23

The macOS Settings window has a fixed size of 760 by 720 points and cannot be resized. `SettingsWindowLayout` holds the size, the style mask (title, close and minimize, without resize) and a view modifier that both the menu-bar-launched window and the SwiftUI `Settings` scene use, and the scene also sets `.windowResizability(.contentSize)`. Tab content that does not fit scrolls inside the window. The Windows Settings window is unchanged, because the owner scoped the request to macOS. The change adds no setting, string, dependency, network use or ABI change.

The decision is recorded in [ADR 0037](adr/0037-fixed-size-macos-settings-window.md), and the chore plan is at [specs/archived/lock-macos-settings-window-size/plan.md](specs/archived/lock-macos-settings-window-size/plan.md). On displays whose visible area is shorter than about 748 points the window may be clipped at the bottom. The manual window check (no resize cursor, zoom button disabled, all tabs usable) is pending.

## monitor-slider-icons-and-tooltips — 2026-10-25

Both native shells present brightness, volume and protection brightness with aligned sun, speaker and shield icons, followed by the slider and percentage. Built-in display cards use the brightness and shield rows and omit volume. Windows uses Segoe Fluent Icons; macOS uses SF Symbols, with a consistent 24-unit icon column within each platform. Protection brightness no longer has a separate visible heading.

Each shell owns the three explanatory help keys in en-US and pt-BR. Passive icons expose hover tooltips; sliders retain localized accessible names with display identity and gain equivalent accessibility help. Existing ranges, visibility, locking, control bindings and dispatch paths remain unchanged. No public interface, core policy, dependency or network behavior changed.

The decision is recorded in [ADR 0038](adr/0038-monitor-slider-icons-and-tooltips.md); the [archived plan](specs/archived/monitor-slider-icons-and-tooltips/plan.md) records implementation and validation. Windows Release built without warnings or errors and all 743 native tests passed. Native macOS execution and interactive visual, accessibility and physical monitor acceptance remain pending. The IDEA diagnostics helpers did not cover the changed Swift, C++, XAML or RESW files, and no IntelliJ inspections connection was available.


## apply-macos-monitor-slider-icons — 2026-10-25

The macOS slider implementation from ADR 0038 now uses SwiftUI `accessibilityHint` for the four explanatory help modifiers in the external and built-in display cards. The previous `accessibilityHelp` calls prevented compilation. Existing SF Symbols, localized icon tooltips, accessible slider names, numeric values, bindings and control behavior are retained. No core, public interface, settings, dependency or network behavior changes.

The standard Apple Silicon build, packaged smoke test, popup layout and camera/localization tests passed. The packaged and installed signatures verified, the installed executable matched the build byte for byte, and the app was reopened from `/Applications/TelaViva OLED.app` after a normal quit and bundle backup. This supplements the earlier macOS validation status in the ADR 0038 section above. IntelliJ inspections returned no findings for the two changed Swift files; SonarLint skipped Swift and TypeScript had no applicable files. The preexisting `Sendable` warning in `MenuBarController.swift` remains, and interactive visual, tooltip hover, VoiceOver and physical monitor checks are still pending.

The compatibility decision is recorded in [ADR 0039](adr/0039-apply-macos-monitor-slider-icons.md), with the execution record in the [archived plan](specs/archived/apply-macos-monitor-slider-icons/plan.md). The local installation does not constitute a release or change the application version.

## keep-awake-with-presence — 2026-10-26

The default-off `keep_awake_with_presence` setting adds an independent shared-core monitoring consumer with native macOS and Windows integration. Camera acquisition continues despite battery/brightness/DDC eligibility gates while the option is enabled and the native session is usable. Retained capture follows the existing bounded inference cadence. Brightness protection keeps its own battery and display eligibility and its two-miss rule; first absence releases only the new inactivity inhibition.

The shared core owns current-generation positive evidence and a monotonic lease of the normal sampling interval plus 15 seconds, capped at 315 seconds. Each native shell owns temporary idle-display/system requests, guarded activity reporting and an independent expiry deadline; repeated snapshots cannot renew evidence. On Windows, the shell holds `SetThreadExecutionState` (display and system required, no away mode) on a dedicated thread while the lease is valid and sends one zero-displacement `SendInput` mouse move per second after local vetoes (locked or non-active session, secure desktop, display off, suspend, active screen saver) pass; `SetThreadExecutionState` does not suppress screen savers or explicit sleep, and a Windows block on synthetic input resets is reported instead of pulsing. Unknown detection, manual session transitions, disabling and shutdown release effects. No automatic unlock/wake, global security preference edits, elevated helper or new network use is introduced.

The owner approved manual-action precedence, release on unavailable detection, and release on the first valid absent observation. Native request success does not establish suppression of every screensaver/security policy. macOS posting permissions, session/saver guard limitations, manual-saver preservation, actual OS idle deadlines and Windows build/hardware results are tracked explicitly in the [validation report](specs/archived/keep-awake-with-presence/validation.md). The application version is 0.1.4. Release publication is separate from the version metadata update.

See [ADR 0040](adr/0040-presence-driven-idle-inhibition.md), the [specification](specs/archived/keep-awake-with-presence/spec.md), [architecture](specs/archived/keep-awake-with-presence/architecture.md), [contracts](specs/archived/keep-awake-with-presence/contracts.md) and [implementation plan](specs/archived/keep-awake-with-presence/plan.md).
