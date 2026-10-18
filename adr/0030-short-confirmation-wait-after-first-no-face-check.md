# ADR-0030: Short confirmation wait after the first no-face check

**Status:** Accepted
**Date:** 2026-10-18
**Feature:** oled-protection-absence-confirmation-latency
**Spec:** [Archived bug plan](../specs/archived/oled-protection-absence-confirmation-latency/spec.md)

## Context

OLED protection dims a monitor after two valid no-face checks ([ADR 0002](0002-external-monitor-control-and-oled-presence-protection.md)): one missed detection must not dim an OLED screen. The core counts the checks in a per-monitor `misses` field and dims at `misses >= 2`.

The cadence the core publishes to the native shells (`camera.next_sample_in_ms`) stayed at `presence_sampling_interval_s` after the first miss. The confirming check was therefore a whole presence interval after the first one, and the time from the face leaving to the dim was about twice the configured interval: 5 s became about 10 s, 2 minutes about 4 minutes, and the 300 s maximum about 10 minutes. The owner expected a delay close to the configured interval.

The shells only obey `next_sample_in_ms`: they arm one sample timer from it, and only when no sample is in progress (`CameraLifecycleDriver.cpp` on Windows, `PresenceCamera.swift` on macOS). Neither the camera lifecycle nor the dark-frame settle reads the sample delay.

## Decision

- **Keep the two-miss rule, shorten only the wait for the confirming check.** While at least one monitor has exactly one no-face check on record, is not yet dimmed and drives the camera (`monitor_is_camera_active`: protected, eligible, awake and above the brightness threshold), face mode reports `return_sampling_interval_s * 1000` as `next_sample_in_ms`. That setting defaults to 1 s and ranges from 0.25 to 5 s. It is the same short cadence the core already uses for return checks and for the first sample after a resume. The change is `Core::absence_confirmation_pending` and one extra condition in the face-mode cadence arm of `snapshot_value` in `crates/telaviva-core/src/core.rs`.
- **Dimming now takes about one sampling interval plus about one second** after the face leaves, instead of two intervals.
- **No new setting, no ABI, snapshot-schema or shell change.** The change is shared by macOS and Windows through the core. `capture_mode` is unchanged: the confirming sample is an ordinary single-frame face sample.
- **Low light is untouched.** A single-frame face sample that is dark keeps the camera on for 5 s, discards frames while the exposure settles and assesses one frame at the end ([ADR 0003](0003-camera-lifecycle-and-display-suspension.md), [ADR 0014](0014-camera-lifecycle-in-the-core.md)). If the settled confirming frame is still dark, the core reports `insufficient_light`, clears the first miss and enters `light_only` with its existing cadence. The 5 s settle stays at 5 s.

## Alternatives Considered

- **Dim on a single no-face check.** Rejected: it removes the guard against one bad frame (a turned head, glare, a missed detection) and contradicts the documented design.
- **Sample at half the interval so two misses take one interval.** Rejected by the owner: it makes the configured value the real time to dim and spaces the two checks well, but it doubles camera captures for as long as a face is present, against the project's camera and battery priority, and it would force a review of the 5 to 300 s range.
- **A confirmation wait of `max(return_sampling_interval_s, 2 s)`.** Not adopted; kept as a fallback if hardware shows false dims (see Non-Obvious Constraints).
- **Shorten the 5 s dark-frame settle to 3 s.** Rejected: samples never overlap, since a new sample is armed only after the previous one has reported, so there is nothing to protect. A shorter settle would risk a still-dark final frame and a false `insufficient_light`.

## Consequences

- The monitor dims about one interval plus about a second after the user leaves, on both platforms.
- One extra camera frame is taken shortly after each first miss. It leads either to the dim or, if a face is back, to a return to the presence interval. Sustained camera use while a face is present is unchanged.
- The confirmation cadence reuses `return_sampling_interval_s`, so it is user-adjustable from 0.25 to 5 s without a new setting.
- Five core contract tests cover it (`first_miss_shortens_the_wait_for_the_confirming_check` and four others in `crates/telaviva-core/tests/core_contract.rs`); they fail without the change. The docs (`docs/user-guide.md`, `ARCHITECTURE.md`, `api.md`) and the CHANGELOG describe it.
- Hardware behavior is not verified: the expected 11 to 12 s dim with a 10 s interval, and the dark-room case, are checked only by the core tests and by reading the shells.

## Non-Obvious Constraints

- Enabling protection starts a resume state in which the core reports 1000 ms until the first accepted presence sample, so the presence-interval cadence is only seen after one accepted sample.
- The two checks are only about one second apart, so they are weakly independent: a head turned for a moment can fail both. If hardware shows false dims, lengthen the wait to `max(return_sampling_interval_s, 2 s)` with no other change.
- `misses` also counts for monitors with protection disabled, which never dim. The confirmation condition therefore requires `monitor_is_camera_active`, or their count would keep the cadence at the short interval. In face mode this exclusion cannot be told apart from a protected monitor holding the same miss, so it is defensive and pinned only by the observable behavior in the tests.
- An insufficient-light, unavailable or present result clears the first miss, so a miss is never carried across a dark or failed sample.
