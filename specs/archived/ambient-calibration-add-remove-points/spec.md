# Chore: Add and remove ambient calibration points

## Chore Description

In Settings > Automatic brightness, each external monitor is calibrated against the MacBook screen through nine **fixed** monitor-brightness points (1, 5, 10, 15, 20, 25, 50, 75 and 100%; the first five are optional and the last four are required). The user cannot choose which points exist. The chore makes the points user-managed:

- The user can **add** a calibration point at any whole monitor brightness from 1 to 100% that is not already used, and **remove** any saved point.
- A calibration needs **at least two points**. A monitor with fewer than two saved points is "not calibrated" and does not follow the MacBook. Each Add or Calibrate opens a dialog that saves one point with Done (enabled once the monitor reaches the point's brightness) or discards it with Cancel; the two-point minimum only decides whether the monitor follows.
- Points are always kept and shown in **ascending order** of monitor brightness. The shell inserts a new point at its sorted position; the core rejects settings whose points are not strictly ascending, so the stored order can never be wrong.

What does not change (ADR 0012): a point is still an external-monitor brightness; choosing it moves the monitor there and a slider sets the MacBook level, and the stored value is the MacBook level at which the two screens look alike. The save-waits-for-the-anchor rule, the neighbor-bounded slider range, the session file, crash recovery, the screen-off/lid-closed refusal and the restore order are untouched.

### Data model

Today `ambient_curves[<monitor-id>]` is the fixed-key object `{ p1, p5, p10, p15, p20, p25, p50, p75, p100 }`. It becomes a variable list:

```json
"ambient_curves": { "<monitor-id>": { "points": [ { "monitor": 25, "builtin": 18 }, { "monitor": 100, "builtin": 90 } ] } }
```

- `monitor`: finite number, `0 < monitor <= 100`, the monitor's brightness percent. Strictly ascending across the list (so no duplicates).
- `builtin`: finite number in `0..=100`, the MacBook level percent. Non-decreasing across the list (equal values stay allowed, as today).
- At most `MAX_CURVE_POINTS = 32` points per monitor; a curve is **complete** (`is_complete`) when it has at least `MIN_CURVE_POINTS = 2` points.
- `schema_version` stays 1 (an existing test rejects 2) and the C ABI is unchanged, because curves travel only inside the JSON settings.
- **Existing points are imported as they are.** The calibrations already saved are not converted by a separate step and do not need recalibration: when a saved or submitted curve still has the old `pN` keys, the deserializer reads each non-null `pN` as the point `{ monitor: N, builtin: pN }` (same meaning since ADR 0012), so those points simply appear in the new list. There is no migration step, no version flag and no user action; the next save writes the `points` form and legacy keys are never written. A legacy curve with fewer than two non-null points loads as incomplete (same as today, where the four required points were missing).

### Runtime mapping (core)

`curve_percent(curve, level)` is piecewise linear through the knots (MacBook level, monitor percent): the implicit origin `(0, 0)` followed by each point `(builtin, monitor)` in order.

- Below the first point it interpolates from `(0, 0)` (unchanged at the low end).
- **Above the last point it stays at the last point's monitor percent** (it was a fixed 100 because `p100` was required). A calibration whose last point is below 100% therefore caps the monitor there; add a 100% point to let it reach full brightness.
- A zero-width segment (equal consecutive MacBook levels, including a first point with `builtin == 0`) returns the segment's upper monitor percent, as today.
- Returns `None` while the curve is incomplete.

### Calibration flow (shell)

- `CalibrationPoint` (the nine-case enum) is removed. `ActiveCalibration.point` becomes the monitor percent as an `Int` (1...100).
- The panel lists the monitor's saved points in ascending order as rows "Monitor N% -> MacBook M%" with a **Calibrate** (re-measure, replaces M) action and a **Remove** action, and an **Add point** row: a stepper for the monitor percent (1...100, default the first unused of 25, 50, 75, 100, else the first unused integer) and an **Add** button. Add is disabled when the percent is already saved (the user edits that row instead) or when 32 points exist. **Add** and **Calibrate** both call `begin(monitorID:point:)`, which opens the calibration dialog; nothing is stored until **Done** (`confirm`), which inserts or replaces that one point and keeps the list sorted. **Cancel** or Escape closes the dialog without saving.
- **Remove** writes the curve without that point through the existing `saveCurve` hook. It is disabled for the point that is currently being calibrated. Removing down to fewer than two points is allowed; the monitor then reports "not calibrated" and stops following.
- `allowedRange(for:monitorID:)` keeps its meaning (bounded by the nearest saved neighbors on each side of the point, 0 below and 100 above when none exist) but derives the neighbors from the sorted list instead of the fixed enum.
- A hint under the list says that at least two points are needed whenever the monitor has fewer than two.

## Relevant Files

- `crates/telaviva-core/src/settings.rs`: `AmbientCurve` (fixed `p1..p100` fields, `is_complete`, `validate`) becomes the `points` list with legacy `pN` deserialization, `MIN_CURVE_POINTS`, `MAX_CURVE_POINTS` and the new validation.
- `crates/telaviva-core/src/core.rs`: `curve_percent` (knots and above-last behavior), `follow_eligibility` (uses `is_complete`), and the `ambient_curves` branch of `update_settings` (~line 571, deserialization stays generic).
- `crates/telaviva-core/tests/core_contract.rs`: about 40 ambient-curve tests (lines ~2700-3270 and ~3620-3990) use the `p25/p50/p75/p100` shape; they are rewritten for `points`, plus new legacy-migration, ordering, bounds, limit and mapping tests.
- `crates/telaviva-core/tests/ffi_contract.rs`: only references calibration through snapshot fields; must stay green and is extended only if it builds a curve.
- `native/macos/TelaViva/CoreBridge.swift`: `AmbientCurve` Swift model (line ~317) and the `ambient_curves` JSON emission in `CoreSettings.json` (~line 382).
- `native/macos/TelaViva/AmbientCalibrationController.swift`: remove `CalibrationPoint` and its `AmbientCurve` subscript; `ActiveCalibration.point`, `begin`, `allowedRange`, `anchorReached`, `confirm`, plus new `removePoint` and sorted insert helpers.
- `native/macos/TelaViva/AmbientBrightnessView.swift`: `CalibrationPanel` (point grid -> sorted point list, add row, remove, and the calibration dialog) and the `notice`/hint text.
- `native/macos/TelaViva/MenuBarController.swift`: `calibration` hooks (`curve`, `saveCurve`) keep their signatures; verify `AmbientCurve()` still compiles with the new model and that nothing else reads `pN`.
- `native/macos/TelaViva/Localization.swift`: en-US and pt-BR strings for the new controls and the two-point hint; `calibration.points.title`, `calibration.help`, `calibration.point.*` get reworded or removed.
- `native/macos/Tests/AmbientCalibrationTests.swift` and `native/macos/Tests/CoreBridgeTests.swift`: rewritten for the point list (controller state machine, range bounds, sorted insert, remove, JSON encode/decode, legacy decode is core-only).
- `tools/tests/test-ambient-calibration.sh`, `tools/tests/test-core-bridge.sh`: compile the files above; no change expected beyond staying green.
- `api.md` (lines ~61 and ~75-77): follow mapping and `AmbientCurve` contract.
- `README.md` (the "Follow the MacBook brightness" paragraph and the Usage text): "four required points and five optional points" becomes "at least two points you add".
- `CHANGELOG.md`: entry under Unreleased.
- `specs/archived/macbook-brightness-follow/validation/hardware-checklist.md`: section F steps that name the fixed 25/50/75/100 and optional low-end points.
- `adr/0012-invert-macbook-calibration-direction.md`: left as is (permanent record); referenced from the new ADR.

### New Files

- `adr/0013-user-managed-calibration-points.md`: records the variable point list, the 2-point minimum, strict ordering, the import of existing `pN` points, the above-last-point behavior and the alternatives (keep fixed keys with a "disabled" flag; normalize unsorted input in the core instead of rejecting). Follows the structure of ADR 0012 and marks 0008/0012 as partially superseded for the stored shape.

## Step by Step Tasks

### 1. Core: curve model, import of existing points and validation (`settings.rs`)

- Replace the nine `Option<f64>` fields with `points: Vec<AmbientPoint>`, `AmbientPoint { monitor: f64, builtin: f64 }` (both `Serialize`/`Deserialize`, `deny_unknown_fields`, `Clone`, `Debug`, `PartialEq`). Keep `AmbientCurve` `Default`, `Serialize`.
- Implement deserialization through a private untagged representation (`#[serde(from = "AmbientCurveRepr")]`): `Points { points }` first, then `Legacy { p1..p100 }` (every field optional, `deny_unknown_fields`). Convert legacy values to points in ascending `N` order, skipping nulls.
- Add `MIN_CURVE_POINTS = 2`, `MAX_CURVE_POINTS = 32`; `is_complete()` is `points.len() >= MIN_CURVE_POINTS`.
- `validate()` rejects: more than 32 points; a non-finite `monitor`/`builtin`; `monitor` outside `(0, 100]`; `builtin` outside `[0, 100]`; `monitor` not strictly greater than the previous; `builtin` lower than the previous. Keep the existing error-string style (`"invalid ambient curve point"`, `"ambient curve points must be strictly ascending"`, `"ambient curve points must not decrease"`, `"too many ambient curve points"`).
- Update the comment above the struct to describe the new meaning. `Settings::validate` and `load` need no other change (a file with `pN` keys is read as points and rewritten in the new form on the next store).

### 2. Core: mapping and eligibility (`core.rs`)

- Rewrite `curve_percent` to build knots as `(0, 0)` plus `(builtin, monitor)` per point, return `None` when `!curve.is_complete()`, interpolate as today, and return the last point's `monitor` (not 100) above the last knot. Keep the zero-width-segment rule and the 0..=100 input clamp.
- `follow_eligibility` keeps calling `AmbientCurve::is_complete`; update the "p25, p50, p75 and p100 are required" comment.
- Keep `update_settings`' `ambient_curves` branch unchanged (generic `serde_json::from_value`), which now also migrates legacy payloads.

### 3. Core tests (`core_contract.rs`, `ffi_contract.rs`)

- Port every curve fixture (`json!({"p25":...})`) to `{"points":[{"monitor":N,"builtin":M},...]}`, including `follow_curve()` and the 128-entry cap test; keep the same numeric scenarios so behavior is compared one to one.
- Add tests for: legacy `pN` deserialization from `update_settings` and from a settings file (migrated to ascending points, round-trips as `points`, no `pN` keys written, curve with one legacy point is incomplete); fewer than two points is `uncalibrated` and writes nothing; exactly two points is `eligible`; unsorted, duplicate-`monitor`, `monitor` 0, `monitor` above 100, negative or above-100 `builtin`, decreasing `builtin`, NaN/Infinity and 33 points are rejected without changing stored settings; the mapping below the first point, between points, and above the last point (stays at its monitor percent, for example 80); a zero-width first segment.
- Run `ffi_contract.rs` unchanged; extend it only if it builds a curve.

### 4. Swift model and JSON bridge (`CoreBridge.swift`)

- Replace `struct AmbientCurve` with `struct AmbientCurvePoint: Codable, Equatable { var monitor: Double; var builtin: Double }` and `struct AmbientCurve: Codable, Equatable { var points: [AmbientCurvePoint] = [] }`. Add `var isComplete: Bool { points.count >= 2 }`, a `func point(at monitor: Int) -> AmbientCurvePoint?`, a mutating `set(monitor: Int, builtin: Double)` that replaces or inserts and re-sorts ascending, and a mutating `remove(monitor: Int)`.
- `CoreSettings.json`: emit `{"points": [{"monitor": ..., "builtin": ...}]}` per monitor (still only when `ambientCurves` is set, so unrelated saves do not touch it). Remove the `p25/p50/...` emission. Decoding reads `points` only (the core always emits the new form).

### 5. Calibration controller (`AmbientCalibrationController.swift`)

- Delete `CalibrationPoint` and the `AmbientCurve` subscript. `ActiveCalibration.point` becomes `Int`; `begin(monitorID:point:)` takes an `Int` and ignores values outside 1...100 (no notice, no write).
- `begin` moves the monitor to `Double(point)`, then initial slider = the saved point's `builtin`, else the captured built-in level, clamped to `allowedRange`.
- `allowedRange` takes the nearest saved neighbor below (`monitor < point`) and above (`monitor > point`) from the sorted list, 0 and 100 when absent, same `min(lower, upper)...upper` result as today.
- `anchorReached` compares to `Double(current.point)`.
- `confirm()` calls `curve.set(monitor: point, builtin: sliderValue)` and saves through `hooks.saveCurve`.
- Add `removePoint(monitorID:point:)`: no-op when that point is the active one; otherwise `curve.remove` and `saveCurve`.
- Keep `end`, `restore`, the session file, recovery and notices as they are.

### 6. Calibration panel UI (`AmbientBrightnessView.swift`)

- In `CalibrationPanel`, replace the `LazyVGrid` of fixed buttons with: the sorted list of saved points (monitor percent, MacBook percent, **Calibrate** and **Remove** buttons, the active row highlighted, **Remove** disabled for the active row), then the add row (`Stepper` 1...100 with the default described above, **Add** button disabled when the percent is saved or 32 points exist). Show the calibration dialog while a point is active (help, waiting text, slider, **Done**, **Cancel**), opened by **Add** or **Calibrate**, and enable **Done** once the monitor has reached the point's brightness.
- Show the two-point hint while the monitor has fewer than two points (also outside a session).
- Accessibility labels for each row and button include the percent and the saved/pending state (reuse `calibration.point.*` or replace them with the new keys).

### 7. Localization (`Localization.swift`)

- Add en-US and pt-BR strings: add-point label, add button, calibrate/re-measure, remove, "at least two points are needed", row format (monitor N%, MacBook M%), accessibility labels. Reword `calibration.points.title` and `calibration.help` so they no longer promise a fixed set. Keep both locale dictionaries complete (the localization check in the build must still pass).

### 8. Swift tests

- `AmbientCalibrationTests.swift`: replace `.p25`-style calls with integer points and `AmbientCurve(points:)`; keep every state-machine scenario (anchor wait, failed write, switch monitor, close, recovery, screen off) and add: add keeps ascending order, editing an existing point replaces its value, remove deletes and is refused for the active point, removing below two points makes `isComplete` false, `allowedRange` with neighbors/with none/with equal neighbors, `begin` ignores 0 and 101, `canFinish` is false with one point and true with two.
- `CoreBridgeTests.swift`: update the curve decode/encode cases (`points` shape, sorted output, curves absent when unset) and drop the optional-low-end-key cases.

### 9. Documentation

- `api.md`: rewrite the mapping paragraph (knots, above-last behavior, eligibility with two points) and the `AmbientCurve` definition (`points`, ranges, ordering, 32-point limit, legacy `pN` accepted on input and never emitted, 128-entry map limit unchanged).
- `README.md`: update the Follow paragraph and Usage item 3 (at least two user-added points, sorted automatically, Done needs two, removing points below two stops following, existing points are kept and shown in the list). Do not add a "recalibrate" instruction.
- `CHANGELOG.md`: under Unreleased > Changed, "Ambient calibration points are now added and removed by the user (at least two, kept in ascending order); existing points are kept". Hardware acceptance pending.
- `adr/0013-user-managed-calibration-points.md`: write per the New Files entry.
- Hardware checklist section F: replace the fixed-point steps with add, re-measure, remove, the two-point gate and the above-last-point cap, marked not run.

### 10. Run Validation Commands

Run every command below and fix failures before reporting. Review the diff for any remaining `p25`, `p50`, `p75`, `p100` or `CalibrationPoint` reference outside the legacy-migration code, its tests, and archived specs/ADRs.

## Validation Commands

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
tools/tests/test-ambient-calibration.sh
tools/tests/test-core-bridge.sh
tools/tests/test-builtin-brightness-monitor.sh
tools/tests/test-popup-layout.sh
scripts/build-macos.sh
grep -rnE "CalibrationPoint|\b(p1|p5|p10|p15|p20|p25|p50|p75|p100)\b" native crates/telaviva-core/src api.md README.md
```

The last command is a review aid, not a pass/fail gate: every hit must be the legacy migration code, its tests, or an intentional mention of the legacy format in `api.md`.

## Notes

- **Decisions made without asking** (each easy to change): the two-point minimum only decides whether the monitor follows (it is not enforced on removal or by the dialog) (the user may drop below two and the monitor simply stops following); the maximum is 32 points per monitor (an arbitrary cap that only bounds the settings size); monitor percents are whole numbers 1-100 in the UI (the core accepts any finite value in `(0, 100]`); above the last point the monitor holds that point's percent instead of jumping to 100.
- **Ordering is by monitor brightness.** Because the MacBook level must not decrease as the monitor level rises, this is also the ascending MacBook order. The core rejects, rather than reorders, unsorted input so a buggy client fails loudly.
- **Payload size.** One monitor at 32 points is about 1.5 KB of JSON, well under the 64 KiB `tv_submit` limit. Because `update_settings` replaces `ambient_curves` as a whole, a few dozen monitors that each hold many points could exceed 64 KiB; that is a pathological case, and the update is rejected cleanly rather than truncated. No change to the limit.
- **Older builds** cannot read a settings file that contains `points` (the old `AmbientCurve` denies unknown fields and the file would be moved aside as corrupt). Acceptable because the feature is Unreleased; state it in the ADR.
- The working tree already has unrelated uncommitted edits in `PresenceCamera.swift`, `SettingsView.swift`, `CHANGELOG.md` and `specs/camera-selection-history-and-wake-resume.md`; do not touch or revert them, and keep the CHANGELOG edit to a single added line.
- Hardware acceptance (DDC writes while adding points, MacBook slider behavior) is not part of this chore and stays pending in the checklist.
- UI follow-up (user request): the slider moved into a dialog; Save point removed; `canFinish` removed; the panel must observe `CoreBridge` or its list goes stale.

## Implementation Status

- [x] 1. Core: curve model, import of existing points and validation
- [x] 2. Core: mapping and eligibility
- [x] 3. Core tests
- [x] 4. Swift model and JSON bridge
- [x] 5. Calibration controller
- [x] 6. Calibration panel UI
- [x] 7. Localization
- [x] 8. Swift tests
- [x] 9. Documentation (also fixed the stale "four-point"/"low-end" CHANGELOG lines, the api.md compatibility sentence and the checklist section E JSON after the docs worker)
- [x] 10. Validation Commands: `cargo fmt --check`, `cargo clippy -D warnings`, `cargo test --workspace` (119 core tests plus the rest), the four Swift test scripts and `scripts/build-macos.sh` all passed. The popup itself was not opened, so the new panel is not visually checked.
- [x] 11. Follow-up: dialog and stale-list fix
