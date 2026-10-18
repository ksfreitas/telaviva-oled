# Bug: OLED protection waits two full sampling intervals before dimming

## Bug Description

With OLED protection on, the monitor only dims after two consecutive no-face checks, and the two checks are a full presence sampling interval apart. The time from the face leaving to the dim is therefore about twice the configured interval: 5 s becomes about 10 s, 2 minutes becomes about 4 minutes, and 300 s (the maximum) becomes about 10 minutes. The user expects the delay to be about the configured interval.

Expected: after the user leaves, the monitor dims about one sampling interval later (plus a short confirmation). Actual: it dims after two sampling intervals.

## Problem Statement

The core requires two valid no-face checks (`misses >= 2`) before dimming, which is the intended false-positive guard (a single missed detection must not dim the screen). But the cadence it publishes to the native shells (`camera.next_sample_in_ms`) stays at `presence_sampling_interval_s` after the first miss, so the confirming check is not taken until another full interval has passed. The confirmation costs a whole interval instead of a moment.

## Solution Statement

Keep the two-miss rule and shorten only the wait between the first miss and its confirming check. While at least one protected, awake, not yet dimmed monitor has exactly one miss (a pending absence confirmation), the core reports `return_sampling_interval_s` (default 1 s, range 0.25 to 5 s) as `next_sample_in_ms`, the same short cadence already used for return checks and for the first sample after a resume. Both shells already schedule from `next_sample_in_ms`, so no native code changes. Result: dim latency is about one interval plus about one second, and the false-positive guard is unchanged.

Preserved rule (owner): low light keeps the existing handling. A single-frame face sample that comes back `insufficient_light` keeps the camera on for 5 s, discards frames while exposure settles, and assesses one frame at the end ([ADR 0003](../../../adr/0003-camera-lifecycle-and-display-suspension.md), `CameraLifecycleDriver.cpp` `startDarkSettle`, `CameraLifecycle.swift` `beginDarkSettle`). The fix does not touch it: it lives in the shells, and the confirming sample is an ordinary single-frame face sample that goes through the same path. If the settled frame is still dark, the core reports `insufficient_light`, clears the miss count (so the first miss is forgotten rather than carried over) and enters `light_only` with its 10 s cadence and 5 s settle. The change only moves when the confirming sample starts, never what happens inside it.

The 5 s settle stays at 5 s (owner asked whether 3 s was needed to avoid overlapping loops; it is not). A new sample timer is armed only when no sample is in progress (`!sampling_` in `CameraLifecycleDriver.cpp:174`, `!self.sampling` in `PresenceCamera.swift:138`), and it is armed from the snapshot that follows the reported result. A dark settle therefore finishes and reports before the next sample is scheduled, so samples never overlap, even at the 5 s minimum interval or with the 1 s confirmation. In the dark the effective period is the interval plus the 5 s settle, as today. The 5 s is the shared `SETTLE_DURATION` in `camera_lifecycle.rs`, chosen for webcam auto-exposure in ADR 0003; shortening it would risk a still-dark final frame and a false `insufficient_light`.

Decision (owner): this approach was chosen over sampling at half the interval, which would make the configured value the real time to dim but doubles camera captures for as long as a face is present, against the project's camera and battery priority, and would also force a review of the 5 to 300 s range.

Known trade-off: the two checks are only about one second apart, so they are weakly independent (a head turned for a moment can fail both). If that proves a problem on hardware, the confirmation wait can become `max(return_sampling_interval_s, 2 s)` with no other change.

Rejected alternative: dim on a single no-face check. It halves the latency but removes the guard, so one bad frame (head turned, glare, a blink of the detector) dims an OLED screen. It also contradicts the documented design ("confirm absence after two valid missed checks"). If the owner prefers it, the change is `m.misses >= 1` on `core.rs:699` and no cadence change, but the docs and ADR statement would have to change with it.

## Steps to Reproduce

1. Enable OLED protection (camera mode `face`, AC power, a monitor with supported brightness, a baseline and stable identity), and set the presence sampling interval to 60 s.
2. Make the face leave the camera view.
3. Observe `camera.next_sample_in_ms` in the snapshot after the first absent observation: it is still 60000.
4. The monitor dims only after the second absent observation, about 120 s after the first.

In the core this is reproduced by the new regression test below: after one `absent` observation, `next_sample_in_ms` equals `presence_sampling_interval_s * 1000.0` instead of the short confirmation cadence.

## Root Cause Analysis

`crates/telaviva-core/src/core.rs`:

- `presence()` (`"absent"` branch, lines ~688 to 704) increments `m.misses` per monitor and sets `presence_dimmed` only when `m.misses >= 2`.
- `snapshot_value()` (lines ~1321 to 1333) chooses the cadence. It uses the short `return_sampling_interval_s` only when a monitor is already `presence_dimmed`, or `RESUME_FIRST_SAMPLE_MS` while `resume_pending`. A monitor holding one miss is neither, so it falls into the `presence_sampling_interval_s` branch and the confirming check waits a full interval.

The state needed to fix it (`misses == 1`) already exists; the cadence code simply never reads it.

Not the cause: the shells (`CameraLifecycleDriver.cpp:175`, `PresenceCamera.swift:142`) only obey `next_sample_in_ms`; settings validation; the inference. `misses` is also incremented for monitors with protection disabled (it never dims them), so the new condition must exclude them or the cadence would stay at 1 s forever.

## Relevant Files

- `crates/telaviva-core/src/core.rs`: the cadence selection in `snapshot_value` and the miss counting in `presence`. The only production change.
- `crates/telaviva-core/tests/core_contract.rs`: regression tests next to `protection_requires_two_fresh_misses_and_suspends_camera_on_battery` (line ~715).
- `docs/user-guide.md` (lines 50 to 52): behavior description to update.
- `ARCHITECTURE.md` (lines 12, 59, 63) and `api.md` (the `next_sample_in_ms` entries, lines ~34 and ~119 to 129): describe the cadence and the two-miss rule; add the confirmation cadence.
- `CHANGELOG.md`: add a Fixed entry under Unreleased.
- `native/windows/Platform/CameraLifecycleDriver.cpp`, `native/macos/TelaViva/PresenceCamera.swift`: read only, to confirm both shells follow `next_sample_in_ms` (no change expected).

## Step by Step Tasks

### 1. Add the confirmation cadence in the core

In `crates/telaviva-core/src/core.rs`:

- Add a small helper on the core (near `is_protected` / `monitor_is_camera_active`), for example:

  ```rust
  // One no-face check is on record and the confirming check is still due: protected, awake
  // and not yet dimmed. Unprotected monitors also count misses but never dim, so they are excluded.
  fn absence_confirmation_pending(&self) -> bool {
      self.monitors.values().any(|m| {
          m.misses == 1
              && !m.presence_dimmed
              && m.display_state != "asleep"
              && self.is_protected(&m.id)
      })
  }
  ```

- In `snapshot_value`, compute `let confirming = camera_mode == "face" && self.absence_confirmation_pending();` and add a branch that returns `self.settings.return_sampling_interval_s * 1000.0`. It goes after the `resume_pending` branch and with the existing `presence_dimmed` branch (merge the conditions in one branch rather than duplicating the arm). Leave `capture_mode` unchanged: the shells already reopen the camera per sample in `single_frame`, as they do for the 1 s resume sample.
- Keep the comments in English and match the surrounding style. No setting, ABI or schema change.

### 2. Regression tests

In `crates/telaviva-core/tests/core_contract.rs`, using the existing `send`, `capture_time` and monitor-discovery pattern from the test at line ~715:

- New test `first_miss_shortens_the_wait_for_the_confirming_check`: enable protection, send one `absent`, assert `camera.next_sample_in_ms == 1000.0` (default `return_sampling_interval_s`) and no pending dim; send a second `absent`, assert the monitor dims and the cadence is still the return cadence (existing behavior).
- Same test, a `present` after the first miss: cadence returns to `presence_sampling_interval_s * 1000` (10000 by default); also a custom `presence_sampling_interval_s` (for example 60) shows the confirmation cadence is independent of it.
- Protection disabled for the monitor (`protection_disabled`): after an `absent` the cadence stays at the presence interval (guards the `misses` leak noted above).
- Asleep monitor and the two-monitor case (one protected with a miss, one without) as short assertions.
- Low-light rule kept: after one `absent`, send `insufficient_light` for the confirming sample. Assert the miss count is cleared (a following `absent` is a first miss again and does not dim), the mode is `light_only`, and `next_sample_in_ms` is 10000. Also assert that `usable_light` twice returns to `face` with the presence interval cadence.
- Native lifecycle tests: confirm that the existing dark-settle tests (`native/windows/Tests/camera_lifecycle_driver.cpp`, and the macOS `CameraLifecycle` tests) cover a face sample armed with `nextSampleMs = 1000`; if they only use the default interval, add one case per platform with 1000 ms to show the 5 s camera-on settle and the single final frame still happen. This is a test-only addition.
- Check that existing assertions on `next_sample_in_ms` (lines 1066, 1081, 2011, 2542 to 2591) still pass unchanged.

### 3. Documentation and changelog

- `docs/user-guide.md` line 51: say that checks run at the sampling interval, and that after the first no-face check the confirming check runs after the short return interval, so the monitor dims about one interval plus about a second after you leave.
- `ARCHITECTURE.md` and `api.md`: add the confirmation cadence next to the existing description of `next_sample_in_ms`, keeping "two valid missed checks".
- `CHANGELOG.md` under Unreleased, Fixed: the monitor now dims about one sampling interval after the face leaves, instead of two.
- Documentation language is en-US. No UI string changes, so no localization work.

### 4. Run the Validation Commands

Run everything below and confirm zero failures.

## Validation Commands

Reproduce before the fix (the new test must fail, with `next_sample_in_ms` equal to the presence interval):

```powershell
cargo test -p telaviva-core --test core_contract first_miss_shortens_the_wait_for_the_confirming_check
```

After the fix:

```powershell
cargo test -p telaviva-core --test core_contract first_miss_shortens_the_wait_for_the_confirming_check
cargo test -p telaviva-core
cargo test -p telaviva-core --test version_consistency
cargo clippy -p telaviva-core --all-targets -- -D warnings
cargo fmt --all -- --check
cargo test --workspace
```

Native shells are untouched, but the Windows platform tests exercise the snapshot codec and camera lifecycle driver, so run them too (use the project's Windows test script under `scripts/`). Manual check on hardware, per the repository rule that code presence is not hardware verification: set the interval to 10 s, leave the camera view, and the monitor dims after about 11 to 12 s instead of about 20 s. Repeat in a dim room: a dark confirming frame keeps the camera on for 5 s and assesses one frame at the end, as before.

## Notes

- Macs and Windows share this core path, so the fix applies to both platforms in one change, as the project requires. The shells need no edit.
- The two-miss guard stays: absence is still only confirmed by two valid no-face checks. Insufficient light and camera errors still reset the count.
- Cost: one extra camera frame shortly after each first miss, which then either returns to the normal interval (face back) or leads to the dim. It does not add sustained camera use.
- The confirmation cadence reuses `return_sampling_interval_s` rather than a new setting, so its user-visible range is the existing 0.25 to 5 s. If it should be independent, that is a settings schema change and out of scope here.
- Open question for the owner: if a single no-face check is acceptable, the alternative above is simpler but weaker against false positives.

## Implementation Status

Implemented on 2026-10-18 on `main`, not committed.

- Step 1 done: `absence_confirmation_pending` in `core.rs` and the face-mode cadence arm that uses it. The helper requires `monitor_is_camera_active(m)` (protected, eligible, awake, above the threshold) instead of only "protected and awake", so a monitor that does not drive the camera never speeds up the sampling. `capture_mode` is unchanged.
- Step 2 done: five tests at the end of `crates/telaviva-core/tests/core_contract.rs`. All five fail without the production change and pass with it. Enabling protection starts a resume state that reports 1000 ms until the first accepted sample, so the tests take the presence-interval baseline after a `present` sample. The "unprotected monitor" test cannot tell apart the excluded-monitor case in face mode (a protected monitor also holds the miss); it pins the observable behavior (an unprotected-only setup stays suspended) and the exclusion stays defensive.
- Native lifecycle tests: no change needed. The Windows dark-settle tests (`native/windows/Tests/camera_lifecycle_driver.cpp`) already run with `nextSampleMs = 1000`, and neither the lifecycle nor the drivers read the sample delay except to arm the timer. macOS has no dark-settle test at all, which is an existing gap and out of scope.
- Step 3 done: `docs/user-guide.md`, `ARCHITECTURE.md`, `api.md` and a `CHANGELOG.md` Fixed entry.
- Step 4: `cargo fmt --all -- --check`, `cargo clippy -p telaviva-core --all-targets --locked -- -D warnings` and `cargo test -p telaviva-core --locked` pass (core_contract 128 tests). The shells and the workspace crates were not rebuilt, since no native or inference code changed. The manual check on hardware (about 11 to 12 s with a 10 s interval, and in a dim room) is still pending.
- Finalized 2026-10-18: [ADR 0030](../../../adr/0030-short-confirmation-wait-after-first-no-face-check.md). A sweep of the other docs found nothing inaccurate: `ARCHITECTURE.md` step 1 and `api.md` (the general "inference follows the configured intervals" sentence) stay as they are because the updated cadence paragraphs cover the first-miss case.
