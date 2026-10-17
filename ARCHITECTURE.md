# TelaViva OLED Architecture

## Overview

TelaViva OLED is a planned desktop application for macOS and Windows that controls the brightness and volume of external monitors through DDC. It adds configurable OLED-oriented brightness protections using webcam presence and screensaver state, with minimal computer resource usage as a central requirement.

This document records agreed intent with implementation in progress. The repository now contains shared Rust policy and inference crates, a native macOS shell and a Windows 11 x64 shell in progress (see the windows-version section below). Current evidence is recorded under `specs/archived/external-monitor-control-and-oled-presence-protection/validation/`; source code and successful tests do not imply verified device support. Reducing brightness to zero is a requested monitor brightness setting; its physical effect and suitability for OLED protection must be evaluated for supported hardware.

### Agreed requirements

- Control external monitor brightness and volume through DDC where the monitor and connection support the requested capability.
- Offer optional webcam presence detection with a configurable 10-second default interval while the user is present; confirm absence after two valid missed checks.
- Detect any face; do not require identifying the user or enrolling a face.
- When no face is detected, reduce the external monitor brightness to zero and preserve the previous brightness for restoration.
- While presence-dimmed, check for return at a configurable 1-second default interval and restore the prior brightness, subject to other active protections.
- Offer an opt-in screensaver brightness reduction with a configurable target defaulting to zero. Apply the strongest active reduction and restore only after applicable protection reasons clear.
- Offer a configurable option to disable presence checking on battery, ON by default; stop camera acquisition and face detection while suspended.
- Process camera data locally without storing images or video; use camera sharing where supported and validated.
- Suspend face inference when lighting is insufficient rather than treating it as absence. The implemented core clears presence-only dimming, checks light every 10 seconds, and requires two usable-light observations before resuming inference; screensaver protection remains independent. Live-camera behavior still requires hardware validation.
- Provide a tray/menu-bar popup with single-click brightness and volume controls for multiple monitors, plus settings and quit. Initial UI locales are pt-BR and en-US. Each native shell owns its UI strings; the core keeps the `locale` setting and returns status codes rather than translated text ([ADR 0015](adr/0015-native-shells-own-ui-strings.md)). Documentation remains en-US.
- Use low-energy hardware acceleration, such as an integrated GPU, when feasible, with software fallback.
- Minimize CPU, memory, camera, and energy usage.

## Stack

- Repository architecture: modular monolith, delivered as a desktop application.
- Initial target operating systems: macOS on Apple Silicon and Windows 11 x64 (ARM64 later; Windows 10 out of scope). Intel Mac support is deferred; affected M1 monitor/connection limitations are accepted and surfaced explicitly.
- Implementation: Rust for shared application policy and inference, with Swift/macOS and future C++/Windows presentation and device adapters.
- Build tooling: Cargo for the shared core, Xcode for the macOS shell, MSBuild for the Windows shell; one repository and modular application.
- UI: SwiftUI/AppKit on macOS and C++/WinRT with WinUI 3 on Windows 11; shared Rust core through a versioned C ABI. No web frontend runtime.
- Face detection: bundled YuNet model with ONNX Runtime via ort; CPU by default and fallback, OS-supplied CoreML on macOS, and DirectML on Windows only for device profiles with a recorded energy benefit (none recorded yet). NPU routes are deferred.
- DDC: Win32 physical monitor/VCP APIs on Windows; isolated IOAVService bridge based on the MIT-licensed m1ddc approach on Apple Silicon.
- Project license: MIT; see `LICENSE`. Third-party libraries and assets retain their own licenses and notices.
- Dependency policy: redistributed libraries, runtimes, and assets must be open source with retained licenses and provenance. Native operating-system APIs are platform facilities.

See [the archived feature architecture](specs/archived/external-monitor-control-and-oled-presence-protection/architecture.md) and [archived runtime contracts](specs/archived/external-monitor-control-and-oled-presence-protection/contracts.md) for decisions, alternatives, planned structure, acceptance budgets, and compatibility gates, and [api.md](api.md) for the implemented native consumer interfaces. These selections are design intent; hardware behavior is not yet verified.

A Cargo workspace now contains `telaviva-core` and `telaviva-inference`. The current Rust implementation groups policy in `crates/telaviva-core/src/core.rs`, settings in `settings.rs`, the ABI in `ffi.rs`, and image inference in `crates/telaviva-inference/src/lib.rs`. The planned module table in the feature architecture describes responsibility boundaries; further file splitting can follow complexity without changing the contracts.

## Modules

The following are intended responsibility boundaries within one application, not existing crates or directories:

- Monitor control: discover external displays, inspect supported controls, and read/write brightness and volume through platform-specific DDC integrations.
- Presence detection: acquire camera samples and detect faces according to the active sampling policy.
- Protection coordination: combine presence and screensaver conditions, preserve previous brightness, and coordinate restoration.
- Operating-system integration: observe screensaver and battery/power state on macOS and Windows.
- Settings and interface: expose per-monitor controls, configurable intervals, protection options, language selection, and application status; persist preferences locally. Native per-OS presentation consumes shared core commands and snapshots.

## Main Flows

### Manual monitor controls

The user selects an external monitor and adjusts its brightness or volume. The application uses the corresponding DDC capability when available. Discovery, unsupported-capability reporting, retry limits, and connection behavior require design and hardware validation.

### Presence protection

1. When presence detection is enabled and permitted by the power policy, sample the webcam at the configured N-second interval.
2. After two valid no-face checks, preserve the monitor's previous brightness once and request brightness zero.
3. Distinguish absent, insufficient-light, and camera-error observations; suspend face inference for insufficient light rather than count it as absence.
4. While presence-dimmed, check for return at a configurable 1-second default interval. On return, restore the previous brightness subject to any other active protection condition.

Two valid missed checks confirm absence. Camera errors and insufficient illumination do not count as valid no-face observations. Detailed recovery latency, camera errors, and permission handling remain technical design and validation matters.

### Screensaver protection

When screensaver protection is enabled and the screensaver becomes active, reduce external monitor brightness to its configurable target (default zero). Apply the strongest active reduction and restore only after applicable protection reasons clear. Screensaver protection starts disabled; lock and display sleep are outside the initial trigger scope.

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

Keep permanent decision records in `adr/`. When a feature is finalized, preserve its reasoning in an ADR and archive its specification rather than discarding it. No project-specific skills directory is configured. SonarQube connectivity is not configured and can be determined when analysis is first requested.

Documentation and commit messages default to en-US; change the `language` convention here if the team chooses another language. No existing Git history or branch pattern was available during initialization.

## external-monitor-control-and-oled-presence-protection — 2026-10-16

The current macOS application consists of a native SwiftUI/AppKit shell, a shared Rust policy/settings core, and a separate Rust inference crate. The shell communicates with the core through ABI version 1, using bounded UTF-8 JSON commands, revisioned JSON snapshots, and explicit buffer ownership. The core decides protection state and action generations; native adapters handle operating-system observations and device calls. Camera frames stay within capture/inference and do not enter the core ABI.

The policy combines presence and screensaver reasons per external monitor, preserves raw brightness baselines, and checks current hardware state before restoring values the app owns. It requires two valid absences before presence dimming; low light and camera errors suspend presence. Battery suspension stops camera acquisition. The native macOS implementation and source/test evidence are recorded in the feature validation reports. A single Dell on one Apple Silicon host has read-only brightness and volume evidence; no physical write or restoration is verified.

The design selects YuNet through ONNX Runtime with CPU fallback. CoreML is a candidate, and no acceleration energy benefit has been established. Windows native implementation and validation are deferred. Remaining gates include camera capture/sharing and permissions, screensaver startup behavior, monitor writes/restoration and broader connection coverage, active resource/energy measurements, UI acceptance, signing/notarization, and clean-account installation. Therefore this architecture record describes an implementation in progress and local testing bundle, not release readiness.

### Permanent documentation and archive

The original [feature specification](specs/archived/external-monitor-control-and-oled-presence-protection/spec.md), [design](specs/archived/external-monitor-control-and-oled-presence-protection/architecture.md), [contracts](specs/archived/external-monitor-control-and-oled-presence-protection/contracts.md), [plan](specs/archived/external-monitor-control-and-oled-presence-protection/plan.md) and [validation records](specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md) are archived as historical evidence. Earlier sections above are retained; their proposed module tree and validation wording must be read alongside this implementation update. The former architecture/contract paths contain navigation pointers. [ADR 0002](adr/0002-external-monitor-control-and-oled-presence-protection.md) records the finalized rationale, and [api.md](api.md) documents the implemented native consumer interfaces. Archival does not complete outstanding Windows or hardware acceptance work.

### Current implementation details

- Policy/settings/ABI live in the core crate; native Swift device queues replace the originally proposed separate Rust adapter and worker modules. The inference crate contains YuNet preprocessing and execution.
- Presence sampling accepts 5–300 seconds (default 10); return sampling accepts 0.25–5 seconds (default 1). Older valid schema-1 interval values are clamped on load without discarding other preferences.
- Normal checks capture a single frame. Return checks retain the camera session across deadlines, including the 5-second return interval; unused frames are discarded and inference remains limited to the chosen cadence. Disabled, suspended and shutdown states stop acquisition. Low-light recovery samples light every 10 seconds.
- The camera snapshot separates operational status from presentation status: `display_status` carries language-neutral codes such as `checking`, `ready`, `insufficient_light`, `error` and the `suspended_*` reasons, and each native shell maps them to its own localized text ([ADR 0015](adr/0015-native-shells-own-ui-strings.md)). Successful fresh observations update operational readiness.
- The native popup aligns monitor controls, caps the protection-brightness slider at current brightness, and opens settings centered in the invoking monitor's visible area. Settings errors are localized and failed login registration is not persisted as successful.
- MIT applies to project code; bundled third-party artifacts retain their notices. Packaging verifies asset digests and includes both project and dependency licenses.

## Camera lifecycle and display suspension — 2026-10-16

The camera lifecycle extension adds an opt-in `always_on_camera` preference to retain capture between scheduled presence checks. Capture eligibility remains controlled by the shared Rust policy; battery suspension, disabled protection, no eligible external monitor, display sleep, camera errors, and shutdown take priority. Retaining a capture session does not increase inference frequency. Return checks retain their existing continuous capture behavior.

A retained camera session (always-on, or the dimmed return check) is set once, right after it starts, to a low frame rate (target 2 fps, snapped to the nearest rate the camera supports) and the rate is never changed while it runs, because changing it restarts the stream and blinks the camera LED. Single-frame sessions are not capped. The original durations are restored when it stops. Sampling cadence is unchanged, and hardware measurements are pending; see the 2026-10-16 amendment in [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md).

The native macOS adapter reports per-monitor OS sleep state with connection generations. Camera monitoring suspends when all eligible external displays are asleep; mixed awake/asleep configurations continue monitoring, while sleeping targets receive no DDC writes. Wake reconciliation uses fresh brightness readback and existing ownership rules. A failed wake readback or restore write keeps ownership and is retried up to three times, and a restore write still requires a readback that matches the value the app last set. Brightness zero and screensaver state do not imply physical display sleep. A monitor manually powered off can remain logically awake depending on its connection.

Low-light recovery uses the existing 10-second retry delay, then a five-second monotonic exposure-settling window before assessing a fresh frame. Initial acquisition and the post-settling frame each have a bounded timeout. Warmup frames do not enter inference or absence counting; two usable-light assessments remain required. The implementation plan and separate hardware checklist live under [camera lifecycle and display suspension](specs/archived/camera-lifecycle-and-display-suspension/README.md). Source and automated checks do not establish physical camera or monitor compatibility.

## camera-lifecycle-and-display-suspension — 2026-10-16

This macOS extension adds opt-in continuous camera capture, per-display sleep policy, and exposure-settling behavior for low-light recovery, while retaining existing eligibility and brightness-ownership rules. The accepted decision is in [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md); the standalone plan and unrun physical acceptance checklist are preserved in the [feature archive](specs/archived/camera-lifecycle-and-display-suspension/). Hardware behavior remains unverified, and Windows remains deferred.

## camera-brightness-activation-threshold — 2026-10-17

The shared core gates presence checking on a brightness threshold, `presence_min_brightness_percent` (default 20, range 10 to 90, strict "above" comparison). Presence checking runs only while at least one eligible monitor is awake and above the threshold; otherwise the camera is suspended with display status `suspended_low_brightness`, or `suspended_no_monitor` when no eligible monitor exists. The core compares the monitor's baseline while the app owns or dims its brightness, so the app's own presence or screensaver dimming does not stop the camera, and the observed level otherwise. The setting is additive to schema 1 and does not change ABI 1.

The macOS shell does not poll monitor brightness. Levels refresh on discovery, display change, wake, the popup refresh button and the app's own writes, so a change made with the monitor's own buttons is noticed only at the next refresh. Periodic DDC reads were not added because they add bus traffic and can disturb some monitors. Hardware behavior remains unverified, and hardware acceptance is pending.

The decision is recorded in [ADR 0011](adr/0011-camera-brightness-activation-threshold.md), and the archived plan is at [specs/archived/camera-brightness-activation-threshold/spec.md](specs/archived/camera-brightness-activation-threshold/spec.md).

## External monitor hardware keys and fluid controls — 2026-10-16

Opt-in macOS keyboard routing adjusts an external monitor's brightness by pointer (key codes 144 and 145), and its volume and mute only while that monitor is the Mac's default sound output, matched by CoreAudio device UID against the display EDID UUID; other targets pass through to macOS. Menu bar sliders use bounded draft coalescing, and DDC writes are serialized through a transaction that verifies readback. The accepted decision is in [ADR 0004](adr/0004-external-monitor-hardware-keys-and-fluid-controls.md), and the feature records are in the [archive](specs/archived/external-monitor-hardware-keys-and-fluid-controls/).

The key step is configurable separately for brightness and volume, from 1 to 15 percentage points per press (default 5), and is stored in the core settings. Shift+Option is a fixed 1-point fine step. The native router receives plain values through `setSteps(brightness:volume:)`, which clamps them and stores them under its state lock. Popup brightness and volume sliders are continuous and are unaffected by the key step.

Behavior is verified only on one LG ULTRAFINE over DisplayPort and one keyboard, and the individual macOS acceptance checklist rows remain not run. Other monitors, docks, HDMI, USB audio and keyboards are unverified, and Windows remains deferred.

## hardware-key-step-settings — 2026-10-16

The key-step settings are two additive schema-1 fields in the core `Settings`, `brightness_key_step` and `volume_key_step`: integers from 1 to 15 percentage points, defaulting to 5 and validated by `Settings::validate`. `update_settings` maps each key to its field, and the snapshot carries both values to the native shell. `MenuBarController` passes them to `HardwareKeyRouter.setSteps(brightness:volume:)` from the snapshot sink. The router stores them under its state lock, and `reserveStep` reads the step for the feature being pressed, so the router keeps no core types. The Shift+Option fine step stays fixed at 1 point.

The router also no longer rebases a key's intent while one of its own accepted hardware writes is outstanding, or within a 1 s settle window after its completion. Acceptance and completion come from `MenuBarController` through `hardwareActionAccepted` and `hardwareActionCompleted`, matched by action generation; an accepted write whose completion never arrives stops blocking after 10 s. Outside those windows, the existing rebase rule still reports external changes and monitor clamping. The decision and its limits are recorded in [ADR 0005](adr/0005-configurable-hardware-key-step.md), and the archived spec is at [specs/archived/hardware-key-step-settings/spec.md](specs/archived/hardware-key-step-settings/spec.md).

## restore-brightness-on-monitor-wake — 2026-10-16

The core's restore retry is bounded. When a wake verification readback or a restore write fails while no protection reason is active and the app still owns the dimmed value, `Monitor.restore_failures` counts the failure and ownership, baseline and error status are kept; `reconcile_all` then re-reads before any retry write. After `MAX_RESTORE_FAILURES` (3) failures, ownership is dropped as before. Dim, manual and volume writes keep their failure behavior. Once shutdown has started, retries are disabled: a failed read or restore write on the shutdown path drops ownership as before, so quitting never waits on retries.

The counter is internal and is not in the snapshot. It resets on a successful `set_brightness`, a display-state change, a manual brightness set and monitor discovery. A successful readback does not reset it, because a restore write that keeps failing while readbacks succeed would otherwise retry without limit. A restore write is still enqueued only after a readback matches the value the app last set. No native shell or ABI change is part of this feature. The decision is recorded in [ADR 0006](adr/0006-keep-brightness-restore-pending-after-failed-wake-readback.md), and the archived spec is at [specs/archived/restore-brightness-on-monitor-wake/spec.md](specs/archived/restore-brightness-on-monitor-wake/spec.md). Hardware behavior on the physical monitor is not verified.

## macbook-brightness-follow — 2026-10-16

The macbook-brightness-follow extension makes external monitors follow the brightness of a MacBook's built-in display. The core owns the follow policy and the mapping from built-in brightness to each monitor's target through a per-monitor four-point curve, exposed to the native shell as a `builtin_display` command. The macOS shell reads and writes built-in brightness through one protocol, implemented by a private DisplayServices/CoreBrightness backend kept behind that protocol. The shell polls the built-in brightness every second, and readings are quantized and debounced before they reach the core. A calibration session fits each monitor's curve and writes a crash-recovery file so that an interrupted session can be recovered. Per-monitor OLED protection participation is a separate setting, so each monitor can opt in or out. The popup height is measured from its content and scrolls beyond three monitors. Settings is a larger, resizable window with General, Keyboard and Automatic brightness tabs. Brightness keys pass through to macOS for monitors that follow the MacBook.

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

The decision is recorded in the 2026-10-16 amendment of [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md), and the archived plan is at [specs/archived/always-on-camera-resource-optimization/plan.md](specs/archived/always-on-camera-resource-optimization/plan.md). No CPU, energy or wakeup measurement exists yet, and a real capture session with the new rate control has not been verified on hardware. Snapshot publish amplification was deferred to [specs/snapshot-publish-amplification.md](specs/snapshot-publish-amplification.md).

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

In single-frame face mode, a frame assessed as `insufficient_light` is not reported at once. The camera stays on for 5 s, frames are discarded while exposure settles, and one frame is assessed at the end of that window; only that final result is reported. This reuses the light-only settle mechanism, so a dark frame taken during auto-exposure after wake no longer starts the light-only cycle. A stalled stream during the settle remains bounded by the existing frame timeout. Continuous capture modes still report an insufficient-light frame immediately. In a dark room each single-frame face sample keeps the camera on about 5 s longer, and this should be measured on hardware before the trade-off is treated as settled. The detector is pre-warmed when face mode becomes active, so ONNX session creation overlaps with camera start. Debug-level logs in the `wake` category record timings from `screensDidWake` through each applied sample delay, camera start, the first frame, detector readiness and the accepted presence report; device identifiers are not logged.

The decision is recorded in the 2026-10-16 amendment of [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md), and the archived plan is at [specs/archived/camera-selection-history-and-wake-resume/spec.md](specs/archived/camera-selection-history-and-wake-resume/spec.md). The manual two-webcam, dark-frame and wake-timing checks are not run; hardware acceptance is pending.

## ambient-calibration-add-remove-points — 2026-10-17

The fixed calibration points (1, 5, 10, 15, 20, 25, 50, 75 and 100%, with only the last four required) are replaced by a list the user manages. `ambient_curves[<monitor-id>]` is now `{ "points": [ { "monitor", "builtin" } ] }`: `monitor` is the monitor brightness percent (above 0, at most 100, strictly ascending), `builtin` is the MacBook level at which the monitor looks equal to it (0 to 100, non-decreasing), with at most 32 points per monitor. A curve is complete, and the monitor eligible to follow, with at least two points. The core rejects unsorted lists instead of reordering them, reads the old `pN` keys as points (so saved calibrations are kept and need no recalibration) and writes only the `points` form. `curve_percent` interpolates through (0, 0) and each `(builtin, monitor)` point and now holds the last point's monitor percent above it, instead of jumping to 100%. `schema_version` and the C ABI do not change.

In Settings > Automatic brightness, each monitor lists its saved points with Calibrate and Remove, and an add row with a stepper for the monitor brightness. Add and Calibrate open a dialog with the MacBook slider: Done saves the point (enabled once the monitor reaches that brightness) and Cancel or Escape discards it; the session capture, anchor check and restore order from [ADR 0012](adr/0012-invert-macbook-calibration-direction.md) are unchanged. The optional low-end points of the previous section are now ordinary points. The panel observes `CoreBridge`, otherwise the list goes stale after a save or a removal.

The decision is recorded in [ADR 0013](adr/0013-user-managed-calibration-points.md), and the archived plan is at [specs/archived/ambient-calibration-add-remove-points/spec.md](specs/archived/ambient-calibration-add-remove-points/spec.md). The contract is in [api.md](api.md). Hardware behavior is not verified; section F of the [hardware checklist](specs/archived/macbook-brightness-follow/validation/hardware-checklist.md) is not run.

## core-camera-lifecycle-abi-contract-and-cleanup — 2026-10-17

Camera session timing moved from the macOS shell into `telaviva-core`, as `crates/telaviva-core/src/camera_lifecycle.rs`, and is exposed through the opaque `TvCameraLifecycle` handle of the C ABI: `tv_camera_lifecycle_create`, `tv_camera_lifecycle_step` and `tv_camera_lifecycle_destroy`. Each step takes one JSON event and returns one JSON result with a `value` and a `state` object. The core owns the 5 s acquisition deadline, the 5 s warm-up and settle window, the once-per-sample dark settle for single-frame face mode, the stale-callback check and the assessment token. The shell passes `now` in seconds from its monotonic clock on every time-dependent event, and the core reads no clock. The shell keeps the `DispatchSource` and `asyncAfter` timers, the monotonic clock and AVFoundation, and the Swift `CameraLifecycle` is now a thin wrapper with the same public API. The core's `camera_mode` and `capture_mode` remain the only policy for whether the camera runs, and the lifecycle owns only session timing for the current mode. The change is additive to ABI version 1, so `tv_abi_version()` still returns `1`, and camera frames still never cross the ABI. Frame-rate selection (`chooseFrameRate` and `frameDuration`) stays in Swift in `native/macos/TelaViva/CameraFrameRate.swift`, because it configures the capture device from AVFoundation frame-rate ranges.

The hand-written headers `include/telaviva_core.h` and `include/telaviva_inference.h` are now checked against the Rust ABI by `header_contract` tests in each crate. The tests use only std, so they add no dependency and need no CI change: they pin every exported function with a typed function-pointer cast and compare prototypes, struct fields and constants with the header text. The core snapshot no longer carries `messages`, and the `locales/en-US.ftl` and `locales/pt-BR.ftl` catalogs were removed. Native shells own their UI strings, so on macOS `native/macos/TelaViva/Localization.swift` is the only catalog; the core keeps the `locale` setting and returns status codes rather than display text. The Xcode Release configuration now links the `aarch64-apple-darwin` release libraries instead of the `target/debug/` ones, and live documentation points to the permanent documents instead of the archived stub `specs/oled-monitor-protection/architecture.md`. Two follow-ups stay deferred, each blocked by an existing record: the calibration session machine (`AmbientCalibrationController`) stays in Swift because [ADR 0008](adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md) makes calibration a shell-owned session and needs a superseding record first, and the `HardwareKeyRouter` keystroke latch stays in Swift because [ADR 0004](adr/0004-external-monitor-hardware-keys-and-fluid-controls.md) says its event tap never calls the core and [ADR 0005](adr/0005-configurable-hardware-key-step.md) keeps the router standalone.

The decisions are recorded in [ADR 0014](adr/0014-camera-lifecycle-in-the-core.md) and [ADR 0015](adr/0015-native-shells-own-ui-strings.md), the archived plan is at [specs/archived/core-camera-lifecycle-abi-contract-and-cleanup/spec.md](specs/archived/core-camera-lifecycle-abi-contract-and-cleanup/spec.md), and the contract is in [api.md](api.md); camera hardware behavior is not verified, so acceptance is pending.

## windows-version — 2026-10-17

The Windows version adds a second native shell over the same Rust policy core and C ABI. The requirements and design are in [specs/archived/windows-version/spec.md](specs/archived/windows-version/spec.md) and [specs/archived/windows-version/architecture.md](specs/archived/windows-version/architecture.md); this section records only the repository shape and status.

- Shell split: `native/windows/Platform` is a static library without XAML that holds the platform layer and is tested with fake backends. `native/windows/TelaViva` is the WinUI 3 app with the tray host, flyout and Settings. Both are Windows 11 x64 only; ARM64 is a later feature.
- Rust DLLs: `telaviva_core.dll` and `telaviva_inference.dll`, each built as a cdylib and linked by the shell through its import library, rather than two static libraries in one binary.
- Inference ABI: additive only. Provider value `2` is the DirectML candidate, with `tv_inference_create_with_provider_device` and `tv_inference_directml_available`. Value `3` is reserved for an NPU provider. The core ABI stays at version 1, and CoreML code is compiled only for macOS. See [api.md](api.md).
- Key hook (`KeyHook`, `KeyIntentRouter`): volume and mute keys only, with the route logic ported from macOS; see the architecture, Decision 9.
- Audio owner (`AudioOwner`): default output matched to a monitor by container ID, with no name fallback; see the architecture, Decision 9.
- Efficiency (`EfficiencyController`, `Scheduler`): EcoQoS while idle, with a user-forced Efficiency mode left in place; see the architecture, Decision 11.
- Provider selection (`InferenceHost`): adapter matching against the allowlist, a startup self-test and CPU fallback; see the architecture, Decisions 3 and 4.

Status: the Windows code is in the repository and covered by automated tests. Windows is no longer deferred, but its readiness is not established. Only the hardware checklist rows marked Pass are verified (see the README Windows section). DirectML energy benefit, NPU, clean-profile install, signed distribution and the remaining checklist rows are open, and the Windows App SDK runtime is redistributed under Microsoft Software License Terms.

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
