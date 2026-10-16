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
