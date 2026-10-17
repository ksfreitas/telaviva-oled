# ADR-0012: Invert the MacBook calibration direction

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** invert-macbook-calibration-direction
**Spec:** [Archived chore plan](../specs/archived/invert-macbook-calibration-direction/spec.md)

## Context

[ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) lets an external monitor follow the MacBook's built-in brightness through a per-monitor four-point curve that the user calibrates. The first calibration flow fixed the **MacBook** level at 25, 50, 75 and 100%, set the MacBook screen to that level, and let the user move a slider for the **external monitor** until the two screens looked alike. The saved value of each point was the monitor brightness.

The request was to invert it: the points are external monitor brightnesses, choosing a point sets the monitor to that brightness, and the slider sets the MacBook, "because the calibration makes more sense for generating the curve". The monitor is then a stable anchor (a DDC write that is read back) and the MacBook level is the free variable (a synchronous `DisplayServices` write), so the four points are spread over the monitor's own range.

The request also asked that calibration cannot be performed while the MacBook screen is off or the lid is closed. [ADR 0010](0010-pause-follow-when-built-in-display-is-off.md) already reports those states as built-in unavailable (`displayOffline`), and the calibration panel was already hidden then, but the controller itself did not check.

The runtime goal did not change: while following, the monitor brightness is still a function of the MacBook level. The stored data is now the inverse of that function.

## Decision

- **Same settings shape, new meaning.** `ambient_curves[<monitor-id>] = { p25, p50, p75, p100 }` is unchanged (each `Option<f64>` in 0 to 100, non-decreasing across present points, complete when all four are present). `pN` is now the MacBook level (percent) at which the monitor looks equal to the monitor at N% of its range. No C ABI change, no `schema_version` change, no new Swift model.
- **The core inverts the stored points.** `curve_percent(curve, level)` maps a MacBook level to a monitor percent with a piecewise linear function through the knots (MacBook level, monitor percent) `(0, 0)`, `(p25, 25)`, `(p50, 50)`, `(p75, 75)`, `(p100, 100)`. A level above `p100` maps to 100%. A zero-width segment (equal consecutive points, which validation allows, including `p25 == 0`) returns the segment's upper monitor percent instead of dividing by zero.
- **Controller order.** `AmbientCalibrationController.begin` moves the monitor to `point.rawValue` first, then writes the MacBook slider (initial value: the stored point, else the MacBook level captured at the start, clamped to the neighbor-bounded range). `setSlider` now writes the MacBook through the built-in backend and never the monitor. A failed MacBook write ends the session and always restores the monitor. The session file, crash recovery and the restore order (built-in level, automatic brightness, monitor, then `calibrating: false`) are unchanged.
- **Save waits for the anchor.** `anchorReached(monitorID:)` is true once the monitor's reported brightness is within 2 points of the active point; the Save button is disabled until then and a hint is shown. The DDC write can take over a second with retries and the controller gets no completion signal, so without this a dropped or failed write would store a point measured against the wrong level.
- **No calibration while the MacBook screen is off or closed.** `begin` checks `backend.status().available` and `!backend.isAsleep()` live, before any capture or write, and refuses with the notice `builtInUnavailable`. Ending a running session with `.builtInUnavailable` also sets that notice. The panel stays hidden while the core reports the built-in display unavailable.

## Alternatives Considered

- **Convert on save and keep the runtime** (invert the measured pairs onto the fixed MacBook grid 25/50/75/100). Rejected: it needs all four points before any conversion, so partial progress would need a new shell-side field, and the resulting curve no longer passes exactly through the measured pairs.
- **Invert at runtime with a new schema** (rename the field or bump `schema_version`). Rejected for this change: an existing contract test rejects `schema_version: 2`, and the rename would touch the ABI-adjacent JSON, tests and docs for a feature that is Unreleased with hardware acceptance pending. The chosen approach keeps the names and documents the new meaning.
- **Migrate saved calibrations.** Not done: the old and new meanings cannot be told apart in the file, and an exact conversion needs all four points. Recalibration is documented instead.
- **Rely only on the hidden panel for the screen-off case.** Rejected: the core snapshot can lag a display sleep or a closed lid, and the controller would then fall through to the misleading "brightness could not be read" notice.

## Consequences

Calibrations saved by earlier development builds are silently reinterpreted with the opposite meaning and give wrong monitor levels until each monitor is recalibrated. This is stated in the README, the CHANGELOG and the hardware checklist.

The calibration slider writes the MacBook brightness on every tick, which is more private-API writes than before; throttle in the controller if hardware shows lag or flicker. The core mapping tests were rewritten for the new semantics, and the Swift controller tests were updated and extended for the anchor, the failed-write paths and the screen-off refusal.

Hardware behavior is not verified. The MacBook setter (`DisplayServicesSetBrightness`) and the automatic-brightness setter were already unverified, and the new anchor tolerance (2 points) and the screen-off behavior are checked only by the rewritten section F of the [hardware checklist](../specs/archived/macbook-brightness-follow/validation/hardware-checklist.md), which is not run.

## Non-Obvious Constraints

- `pN` keys keep their names, but they are MacBook levels now. Anything that reads `ambient_curves` must treat them as (MacBook level at which the monitor equals N%).
- A MacBook level above `p100` maps to 100% on purpose: `p100` can be below 100 when the MacBook looks as bright as the monitor at full brightness before reaching its own maximum.
- When the screen turns off during a calibration, `restore` writes the earlier MacBook level while the display is off, and that write can fail silently while the session file is deleted anyway. This already held for the existing unavailable path and is not changed here; if hardware confirms it, the follow-up is to keep the session file and restore on the next availability.
- Two problems found while scoping, not fixed here and read from code only: quitting drops the monitor restore (`manualControls.shutdown()` clears the pending write queue before the drain runs, and `restore` has already deleted the session file), and launch recovery runs before monitors are discovered, so the monitor restore is likely skipped.
- The core returns 100% for a NaN level. This is unreachable because `builtin_display` validates brightness as finite and within 0 to 100; add an `is_nan` guard if that validation ever loosens.

## Amendment 2026-10-17: optional low-end points

**Feature:** calibration-low-end-points

This dated amendment adds decisions to the calibration contract above. The accepted decisions in this record remain in force.

### Decision

- **Five optional low-end points.** `ambient_curves[<monitor-id>]` gains optional `p1`, `p5`, `p10`, `p15` and `p20`. Each is the MacBook level (percent) at which the monitor looks equal to the monitor at 1, 5, 10, 15 or 20% of its range, with the same meaning as the existing points. Brightness perception changes fastest at the low end, so these points refine dim levels that the four original points cover coarsely. The 1% point was added the same day, for the very dim end of the range.
- **Optional, so the curve stays complete.** A curve is complete when `p25`, `p50`, `p75` and `p100` are present, as before. The low-end points are never required, so existing calibrations keep working and no monitor needs recalibration to keep following.
- **Ordering covers all nine keys.** Present values of `p1`, `p5`, `p10`, `p15`, `p20`, `p25`, `p50`, `p75` and `p100` must be non-decreasing in that order, with nulls ignored.
- **Additive shape.** The field names and the `deny_unknown_fields` shape grow by five keys. There is no C ABI change and no `schema_version` change. Unset keys are omitted from JSON.
- **Runtime curve.** The piecewise linear curve starts at (0, 0), then includes each present low-end point as (MacBook level, N), then (`p25`, 25) through (`p100`, 100). A MacBook level above `p100` still maps to 100%.
- **UI.** The calibration panel shows nine point buttons (1, 5, 10, 15, 20, 25, 50, 75 and 100%) in a wrapping grid.
- **Alternative rejected: make the low-end points required.** That would make every calibration saved with the four original points incomplete, so those monitors would stop following until they were recalibrated with all nine points.

### Consequences

- The change is additive to settings schema 1 and ABI version 1. A build from before this amendment rejects the new keys as unknown fields and treats a settings file that contains them as corrupt, as it does for the other additive fields.
- Hardware behavior is not verified. The low-end checks are in section F of the hardware checklist, which is not run.
