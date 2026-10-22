# ADR-0003: Retain eligible camera sessions and suspend on display sleep

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** camera-lifecycle-and-display-suspension
**Spec:** [Archived implementation plan](../specs/archived/camera-lifecycle-and-display-suspension/plan.md)

## Context

The macOS presence adapter previously used one-frame capture for ordinary checks and retained a camera session only for return checks. Low-light recovery also needed an exposure-settling period, while camera acquisition needed to stop when every eligible external display was asleep. These changes had to preserve existing eligibility, power, error, shutdown, privacy, and brightness-ownership rules. macOS-reported display sleep may not detect a monitor manually powered off while its connection remains logically active.

## Decision

Add an opt-in `always_on_camera` setting, defaulting to false. When enabled, retain a healthy camera session between scheduled checks without increasing inference frequency. Keep capture subordinate to shared core eligibility and suspend it when all eligible external displays are confirmed asleep. Track per-monitor display state with connection generations, suppress DDC writes to sleeping targets, and reconcile after wake using fresh brightness readback and existing ownership checks.

Superseded for the default by the 2026-10-22 amendment.

For low-light recovery, retain the 10-second retry delay, then ignore frames during a five-second monotonic exposure-settling window before assessing a fresh frame. Keep the two usable-light threshold and independent bounded acquisition watchdog. Warmup frames do not enter inference or absence counting. The feature remains macOS-scoped; Windows work is deferred.

## Alternatives Considered

- Keep one-frame sessions for all ordinary checks: this was the original default and is selected when the setting is off (it is on by default since the 2026-10-22 amendment).
- Suspend whenever any external display sleeps: this would stop monitoring even when another eligible display remains awake; the selected policy suspends only when all eligible displays are confirmed asleep.
- Treat brightness zero as display sleep: brightness is not a reliable display-power signal, so the implementation uses OS-reported sleep state.
- Count warmup frames toward presence or absence: this could make exposure transitions produce invalid observations, so warmup frames are ignored.

## Consequences

The setting and display-state contract are additive to settings schema 1 and ABI version 1. Keeping the session open does not change inference cadence. Automated Rust and native fake-session checks can verify policy and scheduling without physical devices; actual camera behavior, display sleep reporting, DDC suppression and restoration, and energy use remain hardware acceptance work. The physical checklist is archived with status not run. Setting files without the key default the option to on since the 2026-10-22 amendment (it was off when the option was introduced); a saved value is always kept. Older binaries may reject the newly serialized field.

## Non-Obvious Constraints

- Only confirmed asleep state for every eligible external display suspends monitoring; unknown state does not count as asleep, and one awake eligible display permits monitoring.
- Brightness zero does not establish physical display sleep or guarantee OLED protection.
- A manually powered-off monitor may remain logically awake according to macOS and its connection.
- Five seconds of exposure settling follows the existing 10-second low-light retry delay; it is distinct from the five-second acquisition timeout.
- Camera frames remain local and outside the core ABI. Windows implementation and validation remain deferred.

## Amendment 2026-10-16: Camera selection history and resume sampling

**Feature:** camera-selection-history-and-wake-resume
**Plan:** [Camera selection history and faster presence resume after wake](../specs/archived/camera-selection-history-and-wake-resume/spec.md)

This dated amendment adds decisions to the camera lifecycle contract above. The accepted decisions in this record remain in force.

### Decision

- **Fallback is resolved at runtime and never persisted.** The core owns `camera_history`, a bounded most-recent-first list whose first entry is `selected_camera_id`. The native shell resolves the effective camera against connected devices when capture starts and when a camera connects or disconnects, and uses the system default only when no history entry is connected. Only an explicit user selection changes stored settings, so a missing camera never rewrites the history.
- **Resume sampling.** A resume is triggered by an event rather than by a camera-mode transition: an eligible display going from asleep to awake, power returning from battery to AC while battery suspension is on, or presence protection being re-enabled. The trigger clears stale low-light state and resets sticky `busy` or `unavailable` camera status to `unknown`. `permission` and `error` keep their existing handling. Until the first accepted camera report, face mode reports `next_sample_in_ms` as 1 s instead of the configured interval. Later samples keep the configured interval, and light-only cadence is unchanged.
- **Dark-frame settle in face mode.** When a single-frame face-mode frame is assessed as `insufficient_light`, the camera stays on for 5 s and frames are discarded while exposure settles. One frame is assessed at the end of the window, and only that final result is reported. This applies only to single-frame face-mode samples; continuous capture modes report an insufficient-light frame immediately. It reuses the light-only settle mechanism, with single-frame face-mode results as its trigger.

### Consequences

- The setting, the snapshot behavior and the 1 s first sample are additive to settings schema 1 and ABI version 1; no C signature changes. As with `always_on_camera`, an older binary that rejects unknown fields may reset a settings file that contains `camera_history`.
- Benefit: a dark frame taken during auto-exposure after wake no longer starts the light-only cycle, which the earlier 1 s first sample made more likely.
- Cost: in a genuinely dark room each single-frame face sample keeps the camera on about 5 s longer before it reports, which raises camera-on time per sample. This works against the project's minimal camera and battery goal and should be measured on hardware before the trade-off is treated as settled.
- Hardware behavior is not verified. The wake acceptance row remains "Not run" in the archived macOS checklist, and the manual checks for this change are pending.

### Non-Obvious Constraints

- The runtime fallback must never be written into `selected_camera_id` or `camera_history`.
- The 1 s first sample applies only to a resume trigger. It does not change `presence_sampling_interval_s` or its 5–300 second range.
- The single-frame face-mode settle is bounded by its 5 s deadline. A stalled stream during the settle remains bounded by the existing frame timeout and is not extended.

## Amendment 2026-10-16: capture frame rate and queue priority

This dated amendment adds decisions to the camera lifecycle contract above. The accepted decisions in this record remain in force.

**Feature:** always-on-camera-resource-optimization
**Plan:** [Archived chore plan](../specs/archived/always-on-camera-resource-optimization/plan.md)

### Decision

- A retained capture session (always-on, or a dimmed return session) is set once, right after the session starts, to a low frame rate: the target is 2 fps, snapped to the nearest rate the camera supports (the built-in camera has a 15 fps floor, the external Lenovo camera 5 fps). The rate is never changed while the session runs, because changing it restarts the camera stream and makes the LED blink. Single-frame sessions are not capped: they last about one frame and a cap would only flicker the LED at start.
- The target rate is matched to the frame-rate ranges of the format that is active after the session starts, because the preset only selects that format at `startRunning`. Cameras expose continuous ranges (15...30) or discrete rates (15.000015), and AVFoundation raises an uncatchable Objective-C exception for any duration outside them. At a range edge the app therefore uses the device's own duration value, and it never writes a duration it has not checked against the range. Cameras with a floor above the target (the built-in camera starts at 15 fps) simply run at that floor.
- The durations the device reported before the first cap are restored, by value, before the session stops. Setting `disableCaptureRateControl` to true in the app's user defaults turns frame-rate control off.
- Sampling cadence and the modes that keep a session open are unchanged.
- The camera queue (`com.telaviva.camera`) runs at background QoS. The 5 s acquisition watchdog and the settle deadlines run on `com.telaviva.camera.deadlines` at utility QoS and hop back to the camera queue.
- `cameraQueueQoS` is the single constant to change back to utility if hardware runs show deferred watchdogs.
- Resolution stays VGA because the detector input is a fixed 640x640; cameras without VGA use fallback presets.

### Consequences

- Status: implemented. Hardware CPU, energy, and frame-rate measurements are pending, so this amendment does not change the acceptance matrix.
- The C ABI is unchanged.

## Amendment 2026-10-17: Camera session timing in the core

Session timing now lives in the core behind `TvCameraLifecycle` (see [ADR 0014](0014-camera-lifecycle-in-the-core.md)), and the decisions in this record remain in force.

## Amendment 2026-10-18: Partial hardware acceptance

The decisions in this record remain in force. The earlier statements that the checklist is entirely unrun are superseded by this amendment.

- The physical checklist was run in part on a MacBook Pro (Apple M4 Pro, macOS 27.0.1) with one Dell S3225QC, using the installed app, which is byte-identical to the build of this commit. Ten of sixteen scenarios passed: existing settings, always-on enable and disable, return and restore, suspension when all eligible displays sleep, resume on wake, brightness zero, screensaver overlapping display sleep, app exit, and the exact Portuguese status text. One passed in part (low light), and five were not run: one awake and one sleeping monitor, light recovery, interruption during settling, camera errors, and an external brightness change while asleep.
- The brightness-zero row was corrected: since [ADR 0011](0011-camera-brightness-activation-threshold.md) the camera suspends as low brightness at or below the threshold. Brightness zero still does not mean display sleep.
- A low-light report with a bright window behind the user was not explained and stays open.
- Results are owner-observed, not instrumented, and cover one monitor and one connection. Energy use is still unmeasured.

## Amendment 2026-10-22: on by default

**Feature:** keep-camera-on-by-default-in-advanced
**Spec:** [Archived plan](../specs/archived/keep-camera-on-by-default-in-advanced/plan.md)

This dated amendment changes the default of the `always_on_camera` setting that the Decision above made opt-in. The accepted decisions in this record remain in force.

### Decision

- **The default is true** for new settings and for settings files without the key. Existing saved values are kept, so an install that saved `false` stays off.
- **No migration.** A one-time migration would need a schema bump or a marker field, and older builds reject unknown fields (`deny_unknown_fields`) and reset settings when they back up the file as corrupt on downgrade. The change therefore needs no schema or ABI change. The CHANGELOG upgrade note tells existing installs where to turn the option on.
- **The setting moved to the Advanced page** on macOS and Windows, in a Camera section above the inference device, with a warning under the toggle: turning it off can cause detection problems, depending on the camera, and can produce "insufficient light" events.

### Consequences

- Energy trade-off: the default keeps the camera session and its LED open between checks, at the capped low frame rate from the 2026-10-16 capture frame rate amendment. Inference cadence does not change, and battery suspension still applies by default. This works against the project's minimal CPU, memory, camera and battery priority, and it is not measured.
- The owner observed that one-frame sessions can, depending on the camera, hurt detection and produce insufficient-light results. This is an owner observation, not a measured result. The five-second dark settle for single-frame face samples (2026-10-16 amendment) was meant to prevent false low-light events; this amendment does not claim that it resolves the camera-dependent problems.
- Hardware is not measured. The default, the Advanced page and the warning have no physical acceptance run on either platform, and the manual checks are pending.

### Non-Obvious Constraints

- A saved `false` is never rewritten to `true`. Only a missing key takes the new default.
- Battery suspension, display sleep, camera errors and shutdown still take priority over the retained session.
