# ADR-0011: Camera brightness activation threshold

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** camera-brightness-activation-threshold
**Spec:** [Archived chore plan](../specs/archived/camera-brightness-activation-threshold/spec.md)

## Context

[ADR 0003](0003-camera-lifecycle-and-display-suspension.md) suspends the camera when no eligible external monitor exists (stable identity, supported brightness, known baseline, protection on) or when every eligible monitor is asleep. A monitor kept at a very low brightness has little left to protect, yet the camera kept running for it.

The request was to keep presence checking off unless a compatible monitor is active, and to add a configurable brightness level below which the camera stays off. The first part already held in the core, so only regression coverage and documentation were added for it. The request text said "só houver um monitor"; it was read as "at least one compatible active monitor", and one awake eligible monitor is enough, as before.

## Decision

> Note (2026-10-29): the 10 to 90 range below was already superseded by the 0 to 90 range of [ADR 0035](0035-laptop-display-protection-and-auto-brightness.md), and is now 1 to 90. A stored 0 is raised to 1 on load. Recorded in [ADR 0044](0044-presence-brightness-threshold-floor-of-one-percent.md). The text below is kept as recorded on 2026-10-17.

A new core setting, `presence_min_brightness_percent`, gates the camera. It is additive to settings schema 1, defaults to 20, and is limited to 10 to 90 in both the core validation and the Settings slider (the range was chosen by the user before implementation). A missing key loads as 20. There is no ABI change.

- The camera runs only while at least one eligible monitor is awake and above the threshold. The comparison is strict: a monitor exactly at the threshold does not count.
- The level compared is the normalized baseline (the user's intended brightness) while the app owns the monitor's value or the monitor has protection reasons; otherwise it is the observed value. A monitor that is presence-dimmed always counts as above, because it was dimmed only after being above and its return check must keep running. A monitor without a baseline is not camera-eligible, as before.
- `Core::sync_brightness_gate` runs after every accepted command and tracks the set of eligible monitors at or below the threshold. A monitor entering the set restarts its miss count; a still-connected monitor leaving it (brightness rises, or the threshold is lowered) starts a clean resume through `begin_camera_resume`. Either transition bumps `camera.generation` and clears the latest presence sample, so in-flight samples are dropped.
- `camera.display_status` gains `suspended_low_brightness`. Precedence is battery states, then `suspended_display_sleep`, then `suspended_low_brightness` (an eligible monitor exists but none is awake and above the threshold), then `suspended_no_monitor`.
- The macOS shell adds the slider, the optional `presence_min_brightness_percent` field in `CoreSettings` (always sent, 20 when unset), and the en-US and pt-BR texts. It needs no lifecycle change, because it already follows `camera.mode` and `capture_mode` from the snapshot.

## Alternatives Considered

- Always comparing the observed brightness. Rejected: when the app dims a monitor for presence or the screensaver, or while a restore write is pending, the camera would stop and restart, which would tear down a retained capture session on each return.
- Polling monitor brightness over DDC so that changes made with the monitor's own buttons are noticed at once. Left out because it adds bus traffic and can disturb some monitors; it can be a follow-up if hardware use shows it matters.
- Allowing 0 to 100, where 100 would switch presence checking off. Not used; the user fixed the range at 10 to 90.

## Consequences

With the default 20%, a monitor kept at 20% or lower no longer gets presence protection. Screensaver protection is unaffected, since it does not use the camera. Lowering the slider to 10% keeps protection at lower levels. One existing test fixture used brightness 0 and now uses 30; brightness 0 has its own test and reports `suspended_low_brightness`, not display sleep.

A manual brightness change reaches the gate at once when the app already owns the monitor's value, and when the write result arrives otherwise.

Hardware behavior is not verified; the work passed automated tests and a local app build only.

## Non-Obvious Constraints

- The shell does not poll monitor brightness. Levels refresh on discovery, display change, wake, the popup refresh button and the app's own writes, so a change made with the monitor's own buttons is noticed only at the next refresh: raising it above the threshold does not start the camera until then, and lowering it does not stop the camera.
- A monitor with unknown (null) brightness has no baseline, so it is not camera-eligible: it neither activates the camera nor suspends it while another monitor is above the threshold.
- `tools/tests/test-core-bridge.sh` reads `target/aarch64-apple-darwin/release/libtelaviva_core.a`; build the core with `--target aarch64-apple-darwin` before running it, or it links a stale library and fails on the new setting.
