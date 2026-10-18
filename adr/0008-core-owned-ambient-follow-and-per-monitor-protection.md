# ADR-0008: Core-owned ambient follow and per-monitor OLED protection

**Status:** Accepted; the per-write DDC retry described in Consequences is now the core's dispatch policy under [ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md)
**Date:** 2026-10-16
**Feature:** macbook-brightness-follow
**Spec:** [spec](../specs/archived/macbook-brightness-follow/spec.md)

## Context

External monitors should follow the brightness of the built-in MacBook display. Each monitor responds differently to the built-in level, so the mapping uses a per-monitor four-point curve that the user calibrates. OLED protection should also be selectable per monitor, so that a monitor the user does not want dimmed can opt out while the others stay protected.

The baseline, ownership by raw value, read-before-write on wake and the restore retries already live in the Rust core (`crates/telaviva-core/src/core.rs`, `reconcile_all`). Follow has to use that same path instead of a second one.

## Decision

The core owns the follow policy.

- The shell reports the built-in display with one command, `builtin_display {available, brightness, calibrating}`. The core keeps this as runtime state and does not persist it.
- For each eligible monitor, the core maps the built-in level through the monitor's curve to an integer raw target, `Monitor.follow_raw`. The curve is piecewise linear through (0, 0), (25, p25), (50, p50), (75, p75) and (100, p100). Input below 0 or above 100 is clamped. The result is rounded and clamped to the monitor's raw range.
- A follow target sets the baseline to `follow_raw`. It does not change `manual_override`, so follow is not treated as a manual change and protection is not suspended.
- While the monitor is dimmed by a protection reason or asleep, follow updates the baseline only and defers the write. Otherwise the write goes through the existing reconcile path, which reads before it writes.
- Follow is authoritative. If a readback differs from `follow_raw`, the core overwrites the monitor; it does not adopt the readback. The loop is prevented by integer targets and a dead band: a new target within one raw unit of the desired value is ignored.
- Curves and the follow switch are stored in `Settings` schema 1 as additive fields: `ambient_follow_enabled` (bool, default false), `ambient_curves` (monitor ID to curve) and `protection_disabled` (a sorted set of monitor IDs). `protection_disabled` stores exceptions, so an absent ID means protection is on for that monitor.
- All per-monitor data is keyed by the full monitor ID and is never pruned. An entry for a disconnected monitor applies again when that ID reappears. A curve never applies to a different ID.
- Per-monitor protection gates the presence and screensaver reasons in `reconcile_all`, and it gates camera eligibility. Turning protection off for a monitor clears its presence dim and miss counter, so turning it on again does not dim it at once.
- Calibration is a shell-owned session. Its state is kept in a crash-recovery file, `calibration-session.json`. Ending a session by any path restores in a fixed order: the built-in level, then automatic brightness (only if it was on), then the monitor's brightness, and finally `calibrating: false`.
- Brightness keys pass through to macOS only for monitors that follow. This deviates from the literal "all brightness keys" wording of FR-016, so that uncalibrated monitors keep hardware key control.
- In the shell, the built-in level is quantized to whole percent and a change is submitted only when it moves by at least 1. Ramps are debounced with a 250 ms trailing delay. Availability and calibration changes are sent without the delay. Reports are delivered to the core in the order they were produced.

## Alternatives Considered

- Shell-computed per-monitor targets, through a `set_follow_target` command per monitor. The shell would then need the curves and the eligibility rules, and the core's baseline, ownership and protection guarantees would be bypassed.
- Reusing `set_brightness`. It sets `manual_override`, which suspends protection, so follow would be treated as a manual change.
- Bumping the schema to 2. Older binaries would refuse the file. The data would be preserved, but the app would be unavailable to those builds, and migration code would be needed.
- A separate profiles file read only by new builds. It adds a second store, a second atomic write path and split validation.
- An EDID-only key, so that settings survive a port or dock change. The code cannot prove that the vendor, product, serial, EDID UUID or CoreDisplay UUID survive a port move, and serial "0" is not special-cased. A wrong match could apply a curve to the wrong panel.
- Passing every brightness key through while follow is on. Uncalibrated monitors would then be controllable only through the popup slider. Revisit this if the user prefers the literal rule; it is a one-line change in the set computation.

## Consequences

- Downgrading to a build without the new fields makes that build rename `settings.json` to `settings.json.corrupt` and start from defaults, which loses the curves. The app is unreleased, so this is accepted. The same caveat is to be added to `api.md`.
- The same monitor on another port or dock gets a new ID. Its curve does not apply, and it needs recalibration. A Phase 0 check will record which ID components change between two ports. A follow-up ADR may add a fallback match if the stable part is sufficient.
- A failed follow write is retried on the next reconcile, which the next built-in change or wake triggers. Each DDC write gets up to three attempts in the native layer, as `contracts.md` requires.
- A change made on the monitor's own buttons while following is overwritten at the next built-in change. The shell does not send `monitor_observed`, so the core does not detect such a change. This is intended.
- Validation caps both maps at 128 entries, with keys of 1 to 512 bytes. This also closes the unbounded-map gap found for `screensaver_targets`.
- `update_settings` sends the whole settings document, and `tv_submit` accepts at most 64 KiB. Validation allows up to 128 entries of up to 512 bytes in each of `ambient_curves` and `protection_disabled`, so a worst-case file could exceed the submit limit. Real monitor IDs (a few hundred bytes) are not expected to approach it. Revisit the limits if monitor IDs grow.
- A hard power loss during calibration is restored only at the next launch.
- The core gains one command and one runtime monitor field. Each accepted `builtin_display` raises the snapshot revision.

## Non-Obvious Constraints

- Ownership compares raw values, so follow targets must be integers. A fractional target such as 37.5 against an integer readback of 38 would cause reads and writes to repeat without end.
- Every accepted command bumps the snapshot revision and fires the snapshot callback. This is why the shell quantizes and debounces before it submits.
- Follow writes use the same actions and read-back verification as the existing DDC path. `follow_raw` changes only the target.
- Follow and protection are independent. A monitor with protection off still follows the built-in display.
- `update_settings` replaces `ambient_curves` and `protection_disabled` as whole values, the same way it replaces `screensaver_targets`. The shell must send the full value.
- Follow is paused (`calibrating: true`) while calibration uses `set_brightness` on the monitor.
- The private display APIs used for the built-in display (`DisplayServices`, `CoreBrightness`) are recorded in [ADR 0007](0007-private-display-apis-for-built-in-brightness.md).
- Hardware behavior is pending acceptance on a MacBook with a DDC monitor. The `DisplayServicesSetBrightness` write, the automatic-brightness setter and ID stability across ports are Phase 0 checks, and none of them is verified yet.

## Amendment 2026-10-18: Controls handle

[ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md) moves the DDC retry policy into the core. The "up to three attempts in the native layer" in Consequences is now three attempts per DDC primitive (write, duplicate write and read), counted by the core's dispatch, and the macOS shell runs one attempt at a time. Follow, protection, the built-in display path and the pass-through of brightness keys for followed monitors are unchanged.
