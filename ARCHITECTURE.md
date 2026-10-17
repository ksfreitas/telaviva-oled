# TelaViva OLED Architecture

## Overview

TelaViva OLED is a planned desktop application for macOS and Windows that controls the brightness and volume of external monitors through DDC. It adds configurable OLED-oriented brightness protections using webcam presence and screensaver state, with minimal computer resource usage as a central requirement.

This document records agreed intent with implementation in progress. The repository now contains shared Rust policy and inference crates plus a native macOS shell. Windows implementation is deferred by the user. Current evidence is recorded under `specs/oled-monitor-protection/validation/`; source code and successful tests do not imply verified device support. Reducing brightness to zero is a requested monitor brightness setting; its physical effect and suitability for OLED protection must be evaluated for supported hardware.

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
- Provide a tray/menu-bar popup with single-click brightness and volume controls for multiple monitors, plus settings and quit. Initial UI/settings locales are pt-BR and en-US; documentation remains en-US.
- Use low-energy hardware acceleration, such as an integrated GPU, when feasible, with software fallback.
- Minimize CPU, memory, camera, and energy usage.

## Stack

- Repository architecture: modular monolith, delivered as a desktop application.
- Initial target operating systems: macOS on Apple Silicon and Windows x64. Intel Mac support is deferred; affected M1 monitor/connection limitations are accepted and surfaced explicitly.
- Implementation: Rust for shared application policy and inference, with Swift/macOS and future C++/Windows presentation and device adapters.
- Build tooling: Cargo for the shared core, Xcode for the macOS shell, MSBuild for the Windows shell; one repository and modular application.
- UI: SwiftUI/AppKit on macOS and C++/WinRT with WinUI 3 on Windows 11; shared Rust core through a versioned C ABI. No web frontend runtime.
- Face detection: bundled YuNet model with ONNX Runtime via ort; CPU fallback, OS-supplied CoreML on macOS and optional OpenVINO on supported Intel Windows hardware, subject to measured energy benefit.
- DDC: Win32 physical monitor/VCP APIs on Windows; isolated IOAVService bridge based on the MIT-licensed m1ddc approach on Apple Silicon.
- Project license: MIT; see `LICENSE`. Third-party libraries and assets retain their own licenses and notices.
- Dependency policy: redistributed libraries, runtimes, and assets must be open source with retained licenses and provenance. Native operating-system APIs are platform facilities.

See [the feature architecture](specs/oled-monitor-protection/architecture.md) and [runtime contracts](specs/oled-monitor-protection/contracts.md) for decisions, alternatives, planned structure, acceptance budgets, and compatibility gates. These selections are design intent; hardware behavior is not yet verified.

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
- Validate brightness ownership across manual changes, application exit, suspend/resume, and monitor reconnection against the policies in specs/oled-monitor-protection/spec.md.
- Measure CPU, memory, energy, and camera use against the engineering budgets in the feature architecture; no benchmark results are claimed.
- Validate packaging, launch-at-login, native library loading, macOS screensaver observation, and the selected dependency/model compatibility.

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
- The camera snapshot separates operational status from localized presentation status, including awaiting-first-image, functioning, low-light, failure and suspension reasons. Successful fresh observations update operational readiness.
- The native popup aligns monitor controls, caps the protection-brightness slider at current brightness, and opens settings centered in the invoking monitor's visible area. Settings errors are localized and failed login registration is not persisted as successful.
- MIT applies to project code; bundled third-party artifacts retain their notices. Packaging verifies asset digests and includes both project and dependency licenses.

## Camera lifecycle and display suspension — 2026-10-16

The camera lifecycle extension adds an opt-in `always_on_camera` preference to retain capture between scheduled presence checks. Capture eligibility remains controlled by the shared Rust policy; battery suspension, disabled protection, no eligible external monitor, display sleep, camera errors, and shutdown take priority. Retaining a capture session does not increase inference frequency. Return checks retain their existing continuous capture behavior.

A retained camera session (always-on, or the dimmed return check) is set once, right after it starts, to a low frame rate (target 2 fps, snapped to the nearest rate the camera supports) and the rate is never changed while it runs, because changing it restarts the stream and blinks the camera LED. Single-frame sessions are not capped. The original durations are restored when it stops. Sampling cadence is unchanged, and hardware measurements are pending; see the 2026-10-16 amendment in [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md).

The native macOS adapter reports per-monitor OS sleep state with connection generations. Camera monitoring suspends when all eligible external displays are asleep; mixed awake/asleep configurations continue monitoring, while sleeping targets receive no DDC writes. Wake reconciliation uses fresh brightness readback and existing ownership rules. A failed wake readback or restore write keeps ownership and is retried up to three times, and a restore write still requires a readback that matches the value the app last set. Brightness zero and screensaver state do not imply physical display sleep. A monitor manually powered off can remain logically awake depending on its connection.

Low-light recovery uses the existing 10-second retry delay, then a five-second monotonic exposure-settling window before assessing a fresh frame. Initial acquisition and the post-settling frame each have a bounded timeout. Warmup frames do not enter inference or absence counting; two usable-light assessments remain required. The implementation plan and separate hardware checklist live under [camera lifecycle and display suspension](specs/camera-lifecycle-and-display-suspension.md). Source and automated checks do not establish physical camera or monitor compatibility.

## camera-lifecycle-and-display-suspension — 2026-10-16

This macOS extension adds opt-in continuous camera capture, per-display sleep policy, and exposure-settling behavior for low-light recovery, while retaining existing eligibility and brightness-ownership rules. The accepted decision is in [ADR 0003](adr/0003-camera-lifecycle-and-display-suspension.md); the standalone plan and unrun physical acceptance checklist are preserved in the [feature archive](specs/archived/camera-lifecycle-and-display-suspension/). Hardware behavior remains unverified, and Windows remains deferred.

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
