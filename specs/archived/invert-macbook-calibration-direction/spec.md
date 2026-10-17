# Chore: Invert the MacBook calibration direction

## Chore Description

Today, calibrating a monitor against the MacBook (Settings > Automatic brightness) works like this: each of the four points is a **MacBook** level (25%, 50%, 75%, 100%). Choosing a point sets the MacBook screen to that level and shows a slider that sets the **external monitor** brightness, so the user matches the monitor to the MacBook by eye. The saved value of each point is the monitor brightness.

The calibration must be inverted, because it produces a better curve:

- Each of the four points is an **external monitor** brightness (25%, 50%, 75%, 100%).
- Choosing a point sets the **external monitor** to that brightness (through the existing `ManualMonitorControls` -> core -> DDC path) and shows a slider that sets the **MacBook** brightness, so the user matches the MacBook to the monitor by eye.
- Confirming stores the slider value, now a **MacBook** level, for that point.

The monitor is the fixed anchor (a DDC write that is read back) and the MacBook level is the free variable (a synchronous `DisplayServices` write), so the user compares against a stable external level and the four points are evenly spread over the monitor's own range.

What does not change: the runtime goal. While following, the monitor brightness is still a function of the MacBook level. The stored data is now the inverse of that function, so the core must invert it.

### Calibration is blocked while the MacBook screen is off or closed

Calibration needs a usable built-in screen: the MacBook is now the screen the user adjusts, and its brightness is written on every slider tick. When the MacBook screen is off (display sleep) or the lid is closed, calibration cannot be performed:

- It cannot start. The calibration panel is already hidden when the core reports `ambient.available == false` (`AmbientBrightnessView.isAvailable`), and the unavailable reason (`displayOffline`, ADR 0010) is shown instead. That snapshot can lag the real state, though (availability refreshes on display sleep/wake notifications and screen-parameter changes), so `AmbientCalibrationController.begin` also checks the live backend (`backend.status().available` and `!backend.isAsleep()`) and refuses to start with a dedicated notice, instead of falling through to the misleading "MacBook brightness could not be read" notice.
- It stops if the screen turns off or the lid closes during calibration. This already happens through `MenuBarController` (`calibration.end(.builtInUnavailable)` when the monitor reports unavailable) and restores the captured state; the chore adds a visible notice so the user learns why the session ended.

### Data model decision

Keep the settings shape unchanged: `ambient_curves[<monitor-id>] = { p25, p50, p75, p100 }`, each `Option<f64>` in 0..=100, non-decreasing across present points (equal values stay allowed), complete only when all four are present. Only the **meaning** changes: `pN` is the MacBook level (percent) at which the monitor looks equal to the monitor at N% of its range. Reasons: no C ABI change (the header has no calibration symbols, everything goes through the JSON `tv_submit`), no `schema_version` bump (an existing test asserts that `schema_version: 2` is rejected), no new Swift model, and the existing neighbor-bounded slider range logic applies unchanged to the MacBook axis.

### Runtime mapping (core)

`curve_percent(curve, level)` maps a MacBook level to a monitor percent. It becomes piecewise linear through the knots (MacBook level, monitor percent) `(0, 0)`, `(p25, 25)`, `(p50, 50)`, `(p75, 75)`, `(p100, 100)`:

- A MacBook level below `p25` interpolates from `(0, 0)` (unchanged behavior at the low end).
- A MacBook level above `p100` clamps to 100% (the monitor cannot go higher; `p100` may be below 100 when the MacBook looks as bright as the monitor at full brightness before reaching its own maximum).
- A zero-width segment (equal consecutive knots, which validation allows, including `p25 == 0`) must not divide by zero: it returns the segment's upper monitor percent.
- The input is still clamped to 0..=100. Callers (`follow_target`) and the raw-range mapping stay as they are.

### Existing saved calibrations

Curves saved by earlier development builds use the opposite meaning and are silently reinterpreted; they give wrong monitor levels until the monitor is recalibrated. The feature is Unreleased and its hardware acceptance is still pending, and a schema bump would conflict with the contract that rejects `schema_version: 2`. The chore therefore documents "recalibrate each monitor after updating" (README, CHANGELOG, hardware checklist) instead of migrating. If the user prefers an explicit migration (rename the field and drop the legacy key), that is a separate, larger change.

### Controller behavior (Swift)

`AmbientCalibrationController` keeps its session, crash-recovery file, restore order (MacBook level, automatic brightness, monitor, then `calibrating: false`) and hooks. Behavior changes:

- New notice `CalibrationNotice.builtInUnavailable`.
- `begin(monitorID:point:)` refuses first: right after `notice = nil` (before ending another monitor's session and before any capture, file write or hardware call), `guard backend.status().available, !backend.isAsleep() else { notice = .builtInUnavailable; return }`. No session file is written, `setCalibrating` is not called and no monitor write is issued. If a session for another monitor was running, it is left to the existing unavailable path (`end(.builtInUnavailable)`), not started again.
- `end(_:)`: when `reason == .builtInUnavailable` and a session was running (`captured != nil`), set `notice = .builtInUnavailable` before `finish(restoreMonitor: true)`, so the stop is visible. Other reasons set no notice, as today. The display-sleep reason (`.displaySleep`, an external display) is unchanged.
- `begin(monitorID:point:)` otherwise: `point.rawValue` is now the monitor level. After capturing state and setting `calibrating`, call `hooks.setMonitorBrightness(monitorID, Double(point.rawValue))` first, then compute the slider (MacBook level) and write it with `backend.writeLevel(slider)`. Initial slider = `clamp(curve[point] ?? captured.builtinLevelPercent, to: range)` (the MacBook level the user had when the session started is the natural starting guess). If `backend.writeLevel` fails: `notice = .builtInWriteFailed` and `finish(restoreMonitor: true)` (the monitor was already moved by this call, so it is always restored; the old `active != nil` condition no longer applies).
- `setSlider(_:)`: clamps to `allowedRange` and writes the **MacBook** (`backend.writeLevel`), not the monitor. A failed write sets `.builtInWriteFailed` and ends the session with `finish(restoreMonitor: true)`, matching `begin`.
- `allowedRange(for:monitorID:)`: logic unchanged (bounded by the nearest stored neighbors, 0 and 100 by default); it now bounds MacBook levels.
- `confirm()`: unchanged code path; the stored value is the MacBook slider value.
- New `anchorReached(monitorID:)`: true when `active` is for that monitor and `hooks.monitorBrightness(monitorID)` is within 2 points of `Double(active.point.rawValue)`. The monitor write can take well over a second (DDC read-back with retries) and the controller gets no completion signal, so Save is enabled only after the monitor reports the point's level; otherwise a failed or dropped monitor write would store a point measured against the wrong level.
- Update doc comments: `CalibrationPoint` is a point on the monitor brightness axis; `AmbientCurve` points are MacBook levels.

## Relevant Files

- `crates/telaviva-core/src/core.rs`: `curve_percent` (around line 1576) holds the mapping to invert; `follow_target` (around line 1269) calls it and needs no change besides what `curve_percent` returns.
- `crates/telaviva-core/src/settings.rs`: `AmbientCurve` doc comment (lines 56-57) describes the old meaning; `validate` (lines 76-93) already enforces non-decreasing and range, kept as is.
- `crates/telaviva-core/tests/core_contract.rs`: mapping tests `follow_curve_*` (around lines 3539-3580) and the `follow_curve()` helper (around line 3460) pin the forward curve and must be rewritten; the other `follow_*` and `ambient_*` tests that use the helper must keep passing with the new fixture.
- `native/macos/TelaViva/AmbientCalibrationController.swift`: session logic (`begin`, `setSlider`, `confirm`, new `anchorReached`) and doc comments.
- `native/macos/TelaViva/AmbientBrightnessView.swift`: `CalibrationPanel` (point buttons, help text, slider row, Save/Done/Cancel). The Save button is disabled until `anchorReached`; a waiting hint is shown meanwhile.
- `native/macos/TelaViva/Localization.swift`: single-line en-US (line 5) and pt-BR (line 6) dictionaries with the `calibration.*` keys.
- `native/macos/TelaViva/BuiltInDisplayBackend.swift`: read-only reference. `BuiltInDisplayBackend.status()` and `isAsleep()` already report a closed lid or a sleeping built-in screen as unavailable (`displayOffline`); the controller's new guard uses them.
- `native/macos/TelaViva/MenuBarController.swift`: wires the hooks (lines 30-54); no change expected, listed to confirm `monitorBrightness` returns the observed value (see Notes).
- `native/macos/Tests/AmbientCalibrationTests.swift`: 18 tests built on a fake backend and hooks; several assume the old direction and need updating, new tests added.
- `README.md`: line 39 describes the old direction.
- `api.md`: line 61 (curve definition) and line 65 (the calibration slider "uses `set_brightness`").
- `CHANGELOG.md`: Unreleased section (line 3, and the ambient-follow entry at line 29).
- `specs/archived/macbook-brightness-follow/validation/hardware-checklist.md`: section F (lines 351-403), the active acceptance checklist linked from the README, written for the old direction.
- `specs/archived/macbook-brightness-follow/spec.md`, `contracts.md`, `architecture.md`: historical records of the old direction; each gets a one-line superseded note, not a rewrite.
- `adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md`: line 57 says calibration uses `set_brightness` on the monitor, which stays true; ADRs are permanent, so no edit.

## Step by Step Tasks

### 1. Invert the core mapping and its tests

- In `crates/telaviva-core/src/core.rs`, rewrite `curve_percent` with knots `(0.0, 0.0), (p25, 25.0), (p50, 50.0), (p75, 75.0), (p100, 100.0)` where x is the MacBook level and y the monitor percent. Clamp `level` to 0..=100; if no segment satisfies `x <= x1` (level above `p100`), return `Some(100.0)`; if a segment has `x1 <= x0`, return `Some(y1)`; otherwise interpolate linearly. Keep returning `None` for an incomplete curve. Update the comment above the function: "Monitor percent for a built-in level: piecewise linear through (0, 0) and the points (MacBook level, monitor percent). Clamps to 100 above the last point."
- In `crates/telaviva-core/src/settings.rs`, change the `AmbientCurve` comment to: "One monitor's calibration: the MacBook level (percent) at which the monitor looks equal to the monitor at 25, 50, 75 and 100 percent of its range. A curve is complete only when all four points are present." Leave the struct and `validate` untouched.
- In `crates/telaviva-core/tests/core_contract.rs`, change the `follow_curve()` fixture to values that are clearly MacBook levels and not the identity (for example `p25 = 20, p50 = 45, p75 = 70, p100 = 90`) and rewrite the five `follow_curve_*` mapping tests to the new semantics: MacBook levels 20/45/70/90 map to monitor 25/50/75/100; MacBook 10 maps to 12.5 (linear from the origin); above 90 clamps to 100; fractional targets round to whole raw units; the 0..255 raw range and the monitor-minimum-above-zero cases keep their intent with the new expected raw values. Add tests for: `p25 == 0` returns 25 at level 0 without NaN; equal consecutive points (for example `p50 == p75`) produce no NaN and a finite monotone result; level 100 with `p100 < 100` yields exactly 100%. Check every other test that calls `follow_curve()` (the `follow_*` tests around lines 3583-3890) and update expected values only where the fixture change requires it.

### 2. Invert the calibration controller

- In `native/macos/TelaViva/AmbientCalibrationController.swift`, apply the behavior described under "Controller behavior (Swift)": reorder `begin` (monitor write first, MacBook write second, initial slider from `curve[point] ?? session.builtinLevelPercent`), make `setSlider` write the MacBook with failure handling, make the failure path of `begin` call `finish(restoreMonitor: true)`, and add `anchorReached(monitorID:)` (tolerance 2 points, evaluated on demand, main-queue precondition like the other methods). Do not change `confirm`, `end`, `recoverIfNeeded`, `restore` or the session file format. Update the doc comments on `CalibrationPoint` and the `AmbientCurve` subscript extension.

### 3. Update the calibration UI and strings

- In `native/macos/TelaViva/AmbientBrightnessView.swift` (`CalibrationPanel`): disable the Save button (`calibration.confirm`) unless `calibration.anchorReached(monitorID: monitor.id)`; while it is false, show a caption `calibration.waiting` under the help text. The slider row, its `allowedRange` and the `"NN%"` readout stay; the slider label key stays `calibration.slider` but its text changes. Confirm the view re-evaluates when the monitor snapshot changes (the panel already reads `core.snapshot`); if it does not, observe the snapshot the same way the point buttons do.
- In `native/macos/TelaViva/Localization.swift`, in both locale lines (one line each, keep the existing style and the `// I18N` convention used there):
  - `calibration.points.title`: en-US "Monitor brightness points" / pt-BR "Pontos de brilho do monitor".
  - `calibration.help`: en-US "Choose a point. The monitor goes to that brightness; adjust the MacBook screen until both screens look the same." / pt-BR "Escolha um ponto. O monitor vai para esse brilho; ajuste a tela do MacBook até as duas telas ficarem iguais."
  - `calibration.slider`: en-US "MacBook brightness" / pt-BR "Brilho do MacBook".
  - new `calibration.waiting`: en-US "Waiting for the monitor to reach this brightness…" / pt-BR "Aguardando o monitor atingir este brilho…".
  - new `calibration.notice.builtInUnavailable`: en-US "Calibration is not possible while the MacBook screen is off or the lid is closed." / pt-BR "A calibração não pode ser feita com a tela do MacBook desligada ou fechada."
  - Leave the other `calibration.*` keys as they are (the notice texts remain accurate).
- In `AmbientBrightnessView.swift`, map the new `CalibrationNotice.builtInUnavailable` case to `calibration.notice.builtInUnavailable` in `CalibrationNoticeView.noticeKey` (the switch must stay exhaustive). Keep the existing rule that the panel is hidden while `isAvailable` is false; the notice view is outside the panel and stays visible. The point buttons need no extra disabling.

### 4. Update the Swift controller tests

- In `native/macos/Tests/AmbientCalibrationTests.swift`, update the fake setup and assertions to the new direction: `begin` writes the monitor level `point.rawValue` through `hooks.setMonitorBrightness` and the initial MacBook slider through `backend.writeLevel`; `setSlider` writes the backend and no longer the monitor; `sliderRangeIsBoundedByNeighbors` and `confirmSavesOnlyTheChangedPoint` use MacBook values; `everyEndReasonRestoresInOrder` keeps the order built-in level, automatic brightness, monitor, `calibrating: false`; `failedFirstWriteRestoresAndReports` now expects the monitor to be restored too. Keep the recovery and session-JSON tests unchanged. Make the test file's `FakeBackend` configurable first: `status()` and `isAsleep()` currently return fixed values (lines 33-37); give them settable properties (default available and awake) like the fake in `BuiltInBrightnessMonitorTests.swift`. Add tests: `beginIsRefusedWhileTheBuiltInScreenIsUnavailable` (status unavailable with reason `displayOffline`: notice `.builtInUnavailable`, no backend or monitor write, no session file, `setCalibrating` never called, `active` nil); `beginIsRefusedWhileTheBuiltInScreenIsAsleep` (status available but `isAsleep()` true: same expectations); `endBuiltInUnavailableReportsTheNoticeWhenASessionWasRunning` (session running, `end(.builtInUnavailable)`: state restored in order and notice `.builtInUnavailable`) and `endBuiltInUnavailableWithoutASessionSetsNoNotice`; also check that `end(.cancelled)` and the other reasons leave `notice` nil. Also add: `anchorReachedFollowsTheMonitorLevel` (false while the hook reports a different level, true within 2 points, false after `end`); `failedSliderWriteEndsTheSessionAndRestores`; `initialSliderFallsBackToTheCapturedBuiltInLevel` (no stored point). Register new tests in the main list at the top of the file.

### 5. Update documentation

- `README.md` line 39: rewrite the sentences about the points: each monitor has four calibration points (25%, 50%, 75% and 100% of the **monitor's** brightness); choosing a point sets the monitor to that brightness and shows a slider for the MacBook screen, so you can match the two screens by eye. Add: calibrations made by earlier development builds must be redone. Add: calibration cannot be performed while the MacBook screen is off or the lid is closed (the controls stay hidden, and a calibration in progress stops and restores the earlier brightness). Keep the rest of the paragraph (automatic brightness handling, restore, pause behavior, ADR link) intact.
- `api.md` line 61: define the curve as piecewise linear through (0, 0), (`p25`, 25), (`p50`, 50), (`p75`, 75) and (`p100`, 100) in (MacBook level, monitor percent), clamped to 100% above `p100`; line 65: the calibration uses `set_brightness` to set the monitor to each point (adjust the wording that implies the slider uses it).
- `CHANGELOG.md` under Unreleased: add an entry that the calibration now fixes the monitor brightness at each point and the slider sets the MacBook brightness, that the core inverts the saved points when following, and that monitors calibrated with earlier development builds need recalibration. Match the file's existing entry style.
- `specs/archived/macbook-brightness-follow/validation/hardware-checklist.md` section F (lines 351-403): rewrite the steps for the new direction (the monitor goes to 25/50/75/100%, the MacBook slider appears, move it until the MacBook looks the same as the monitor, check monotone order on the MacBook slider, Save enabled only after the monitor reaches the level, restore of the monitor level on Done/Cancel and after a force quit); add a first step to recalibrate if the monitor was calibrated by an earlier build; add checks that with the MacBook screen off (display sleep) or the lid closed the calibration controls are hidden and the unavailable message is shown, that a point cannot be started, and that turning the screen off or closing the lid during a calibration ends it with the "not possible while the MacBook screen is off or closed" notice and restores the earlier MacBook brightness, automatic brightness and monitor level once the screen is back.
- Add a one-line note at the top of `specs/archived/macbook-brightness-follow/spec.md`, `contracts.md` and `architecture.md`: "Superseded in part: the calibration direction was inverted (points are monitor levels, the slider sets the MacBook). See `specs/invert-macbook-calibration-direction.md`." Do not rewrite their bodies.

### 6. Run the validation commands

- Run every command under Validation Commands, fix failures, and report any that could not run (for example `test-core-bridge.sh` needs the Rust library built first).

## Validation Commands

Run from the repository root. These cover the repository's own checks and the touched areas; nothing here needs a camera, a monitor or the network.

- `cargo fmt --all -- --check`
- `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu`
- `cargo clippy --workspace --all-targets --locked -- -D warnings`
- `tools/tests/test-ambient-calibration.sh`
- `tools/tests/test-builtin-brightness-monitor.sh`
- `tools/tests/test-manual-monitor-controls.sh`
- `cargo build --workspace --release --target aarch64-apple-darwin --locked`, then `tools/tests/test-core-bridge.sh` (the bridge test needs `libtelaviva_core.a`, as in the macOS workflow)
- `./scripts/build-macos.sh` to confirm the native app (view and strings) still compiles
- `git diff --stat` and a read of the full diff: only the files listed above changed, no leftover references to the old direction in the active docs (`grep -rn "MacBook screen to that level\|sets the MacBook screen to that level" README.md api.md specs/archived/macbook-brightness-follow/validation`).

Hardware acceptance of the new flow stays pending and is covered by the updated checklist section F; the automated checks above do not prove it.

## Notes

- Hardware: the MacBook setter (`DisplayServicesSetBrightness`) and the automatic-brightness setter are still unverified on hardware (see `BuiltInDisplayBackend.swift`). The new flow writes the MacBook on every slider tick, which is more writes than before; if hardware shows lag or flicker, throttle in the controller as a follow-up.
- The 2-point tolerance in `anchorReached` is a guess that fits monitors with a 0..100 raw range and ranges up to 255 (a raw step is at most about 0.4 points). Confirm on the LG ULTRAFINE and the Dell S3225QC. Also confirm that the `monitorBrightness` hook (`MenuBarController.swift:34-37`, snapshot `brightness.value`) is the **observed** (read-back) value and not the desired one; if it is the desired value, `anchorReached` must read the observed field of the snapshot instead.
- Pre-existing problems found while researching, out of scope here, worth separate chores (read from code, not run on hardware):
  - Quit drops the monitor restore: `MenuBarController` calls `calibration.end(.appQuit)` (which queues a monitor write through `ManualMonitorControls.setValue`) and then `manualControls.shutdown()`, which clears the pending queue before the drain runs, and `restore` has already deleted the session file. The same applies to the display-sleep path when the display is asleep.
  - Launch recovery runs before monitors are discovered: `calibration.recoverIfNeeded()` follows `ddc.start()` immediately, so `hooks.monitorBrightness` is likely still nil and the monitor restore is skipped, then the session file is deleted.
  - Done does not save an unsaved slider change (only Save does), while the checklist says "Done ... the saved value is kept"; the checklist wording is clarified in step 5 only if it conflicts with the rewritten steps.
  - `App.swift` declares a SwiftUI `Settings` scene that builds `SettingsView` without `calibration`; its reachability was not verified.
- Screen-off restore: when the screen turns off during a calibration, `restore` writes the earlier MacBook level while the display is off, and that write can fail silently (the step is `_ = backend.writeLevel`, and the session file is deleted regardless). The level would then stay at the last calibration value until the user changes it. This already holds for the existing unavailable path and is not changed here; the hardware checklist addition above is meant to observe it. If it fails on hardware, the follow-up is to keep the session file and restore on the next availability.
- The availability guard in `begin` reads `backend.status()` and `backend.isAsleep()` live; `status()` already folds the lid state and the display sleep state into `displayOffline` (ADR 0010), so no new IOKit code is needed.
- The old `plan.md` task T053 references `validation/calibration.md`, which does not exist; ignore it.
- Archived specs and ADRs are historical records. They get only the superseded note, not a rewrite.
- Delegation per the project and global rules: the orchestrator verifies lint, formatting, tests and the diff; code edits go to Haiku agents with this plan's exact before/after briefs (the tasks above are independent per file group: core and Rust tests, Swift controller and its tests, view and strings, docs). The Swift controller, its tests and the view share the new `anchorReached` API, so run tasks 2 to 4 sequentially or give them to one agent.

## Implementation Result

All six steps are done (2026-10-17). Verified by the coordinator: `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo test --workspace --all-targets --locked` (103 core tests, 6 FFI tests, 10 inference tests passed; the runtime integration test was skipped as in CI), `tools/tests/test-ambient-calibration.sh`, `test-builtin-brightness-monitor.sh`, `test-manual-monitor-controls.sh`, `test-core-bridge.sh` and `./scripts/build-macos.sh`. Hardware acceptance remains pending.

Deviations from the plan:

- `cargo fmt --all` was run once after the Rust worker finished, because its new tests were not rustfmt-formatted.
- The follow dead band in the core is an exact match on the rounded target, not a one-unit band, so one planned test input (MacBook 50.5) was changed to 50.2 and the affected hardware-ack values in the `follow_*` tests were recomputed. The Rust tests also gained a helper, `follow_first_write_for`.
- `api.md` also had the old meaning in the AmbientCurve settings description (line ~75); it was reworded.
- Section F of the hardware checklist was renumbered 1 to 26.
- `curve_percent` returns 100% for a NaN level instead of `None`. This is unreachable today because `builtin_display` validates brightness as finite and within 0 to 100; add an `is_nan` guard if that validation ever loosens.
- `CHANGELOG.md` line 21 has a pre-existing missing line break ("pending.- Faster first presence"); left as is.
