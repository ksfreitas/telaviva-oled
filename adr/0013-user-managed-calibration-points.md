# ADR-0013: User-managed calibration points

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** ambient-calibration-add-remove-points
**Spec:** [Archived chore plan](../specs/archived/ambient-calibration-add-remove-points/spec.md)

## Context

[ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) lets an external monitor follow the MacBook's built-in brightness through a per-monitor curve that the user calibrates. [ADR 0012](0012-invert-macbook-calibration-direction.md) inverted the meaning of each saved point: the point is an external monitor brightness, and the saved value is the MacBook level at which the two screens look alike. The stored shape stayed fixed, as `ambient_curves[<monitor-id>] = { p25, p50, p75, p100 }`, with four required keys and the optional low-end keys `p1`, `p5`, `p10`, `p15` and `p20` added by an amendment to ADR 0012.

The fixed keys meant the user could not choose which points exist. Each monitor had to be measured at the same percentages, and a point at any other brightness could not be saved. The request was to let the user add and remove points at any whole monitor brightness, so that calibration can match the monitor's own range and the user's dim levels.

## Decision

- **Variable point list replaces the fixed keys.** `ambient_curves[<monitor-id>] = { points: [ { monitor, builtin }, ... ] }`. `monitor` is the monitor's brightness percent, a finite number greater than 0 and at most 100. `builtin` is the MacBook level percent, a finite number from 0 to 100, at which the monitor looks equal to the monitor at `monitor`%. The meaning of each point is the one from ADR 0012.
- **Two-point minimum.** A curve is complete with at least two points. A monitor with fewer than two saved points is uncalibrated and does not follow the MacBook. Each calibration dialog saves one point, so **Done** is not gated by the point count; it is enabled once the monitor has reached that point's brightness. Removing points is not blocked, so the monitor simply stops following when fewer than two remain.
- **Calibration dialog.** The calibration slider lives in a dialog opened by Add or Calibrate.
- **Strict ascending order, rejected rather than reordered.** `monitor` must be strictly ascending and `builtin` must be non-decreasing across the list. The shell inserts each new point at its sorted position. The core rejects settings that are not sorted, so a buggy client fails loudly and the stored order cannot be wrong.
- **Existing `pN` points are read as points.** The deserializer accepts the fixed-key object, reads each non-null `pN` as `{ monitor: N, builtin: pN }`, and so the saved points appear in the list. There is no migration step, no version flag and no recalibration. The next save writes the `points` form, and the legacy keys are never written. A legacy curve with fewer than two non-null points loads as incomplete, as it did before.
- **Above the last point, the monitor holds.** The mapping interpolates from the origin below the first point, as before. Above the last point it stays at that point's `monitor` percent instead of jumping to 100%. A calibration whose last point is below 100% therefore caps the monitor there, and a 100% point is needed to reach full brightness.
- **Limit of 32 points per monitor.** The limit bounds the size of the settings. The 128-entry map limit and the 64 KiB `tv_submit` limit are unchanged.
- **No schema or ABI change.** `schema_version` stays 1, and the C ABI is unchanged because curves travel only inside the JSON settings.
- **Partial supersession of the stored shape.** This ADR partially supersedes the stored shape defined in [ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) and [ADR 0012](0012-invert-macbook-calibration-direction.md): the fixed `p25`, `p50`, `p75` and `p100` keys and the four-required-points rule. Both records remain as permanent history. The meaning of each point, the save-waits-for-the-anchor rule and the screen-off refusal from ADR 0012 are unchanged.

## Alternatives Considered

- **Keep the fixed keys and add a disabled flag per point.** Each of the nine keys would gain an enabled state. Rejected: a fixed set of keys still cannot express an arbitrary whole percentage from 1 to 100 without one key per value, and the core, the Swift model and the tests would keep coupling to the same nine slots.
- **Normalize unsorted input in the core.** The core would sort the points instead of rejecting them. Rejected: the shell is the only writer of this data, and silent reordering would hide a client bug. Strict validation keeps the stored order exact and reports the mistake at the point where it happens.

## Consequences

Older builds cannot read a settings file that contains `points`. The old `AmbientCurve` denies unknown fields, so the file is moved aside as corrupt and the app starts from defaults. This is acceptable because the feature is Unreleased.

Calibrations saved with the fixed keys keep working without recalibration, because their points are read as they are. Calibrations saved before [ADR 0012](0012-invert-macbook-calibration-direction.md) still need recalibration, as that record states.

Hardware behavior is not verified. The DDC writes made while adding points and the MacBook slider behavior are checked only by section F of the [hardware checklist](../specs/archived/macbook-brightness-follow/validation/hardware-checklist.md), which is not run and stays pending acceptance.

## Non-Obvious Constraints

The calibration panel in `AmbientBrightnessView.swift` must observe `CoreBridge` (`@ObservedObject`). It reads the saved points from the core snapshot, and with a plain reference SwiftUI skips re-rendering it when only the points change, so a removal or a save looks as if nothing happened even though the core accepted it.
