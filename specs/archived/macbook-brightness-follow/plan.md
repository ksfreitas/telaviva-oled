# Tasks: MacBook Brightness Follow, Popup Sizing and Larger Settings

**Branch**: feat/macbook-brightness-follow **Specs**: [spec.md](spec.md), [contracts.md](contracts.md) **Architecture**: [architecture.md](architecture.md) **Status**: Code and docs complete; hardware validation pending ⏰

Test tasks are included because SC-008 requires them. Each test task comes first and MUST be run to confirm it fails before its implementation tasks start. `[P]` means the task touches a different file than its neighbors with no ordering dependency. Story groups: `US1-3` popup sizing, `US4` larger Settings, `US10` per-monitor protection (also covers the protection half of `US9`), `US5/7/9` follow, `US6` calibration, `US8` popup in follow mode and keys.

## Phase 1: Setup and hardware spikes ⏳ (~2h)

**Notes**: Deferred by the coordinator during `--ff`: the setter spikes change the user's screen and settings and need the user present. Code in later phases is written spike-agnostic (polling, fail closed, popover sizing with a documented fallback).

These need the user at the MacBook with the external monitor connected. Probes are scratch code outside the repo; results are recorded in `specs/archived/macbook-brightness-follow/validation/spikes.md`. Setter spikes change the user's screen and settings, so run them only with the user present and restore state afterward.

- [ ] T001 [P] Spike: call `DisplayServicesSetBrightness` on the built-in display (change by 5 points, then restore) and record the return status, exact effect and read-back.
- [ ] T002 [P] Spike: toggle automatic brightness through `CBDisplayBrightnessClient setAutoBrightnessEnabled:error:` (off, then restore the original), and compare the result with System Settings and `/usr/libexec/corebrightnessdiag status-info`.
- [ ] T003 [P] Spike: register `DisplayServicesRegisterForBrightnessChangeNotifications`, find the callback signature and whether it fires for key presses and automatic ramps, then decide polling only or push plus safety poll (architecture Decision 6).
- [ ] T004 [P] Spike: run `TelaViva --diagnostics` with the same external monitor on two ports (and a dock if available) and record which ID components change (architecture Decision 4).
- [ ] T005 [P] Spike: in a scratch SwiftUI app, check that `NSHostingController.sizingOptions = .preferredContentSize` resizes an `NSPopover` to its content on macOS 14+; otherwise note the `contentSize` fallback.
- [ ] T006 Write `specs/archived/macbook-brightness-follow/validation/spikes.md` with each result and, for any failed spike, amend the matching Decision in `architecture.md` (for example drop push notification, or accept calibration without automatic-brightness control).

**Checkpoint**: ✋ Spike results recorded; architecture decisions still hold or are amended

## Phase 2: Foundational ✅ (~4h)

**Notes**: Done. `cargo test` (60 core + 4 ffi), clippy `-D warnings`, fmt and all `tools/tests/test-*.sh` pass. A test written with insertion-order expectations for `protection_disabled` was corrected, because the contract says it is a sorted set.

⚠️ CRITICAL: blocks every phase below. Settings, snapshot and bridge plumbing that all stories use.

- [x] T007 Add failing tests in `crates/telaviva-core/tests/core_contract.rs` for the new settings: defaults, `AmbientCurve` rules (range 0–100, non-decreasing ignoring nulls, nulls allowed), the 128-entry caps, key length 1–512 bytes, and a save and load round trip.
- [x] T008 Add `AmbientCurve`, `ambient_follow_enabled`, `ambient_curves` and `protection_disabled` with `#[serde(default)]`, validation and caps in `crates/telaviva-core/src/settings.rs` (schema stays 1).
- [x] T009 Add failing tests in `crates/telaviva-core/tests/core_contract.rs` for `update_settings` on the three fields (whole replacement, omitted keys untouched, invalid curve rejected with state unchanged) and for the static snapshot fields (`ambient` object, per-monitor `follow` and `protected`).
- [x] T010 Add the `update_settings` arms in `crates/telaviva-core/src/core.rs` and the static snapshot output (`ambient` with `enabled` from settings and `available`, `calibrating`, `builtin_percent` defaulting to false, false and null; per-monitor `follow` eligibility and `protected`); run `cargo test -p telaviva-core` until T007 and T009 pass.
- [x] T011 [P] Add failing tests in `native/macos/Tests/CoreBridgeTests.swift` for the new `CoreSettings` keys (optional, so old snapshots still decode), their presence in `CoreSettings.json`, and decoding of `CoreMonitor.follow`/`protected` and `CoreAmbient`.
- [x] T012 Implement the new fields in `native/macos/TelaViva/CoreBridge.swift` (`CoreSettings` properties and `json`, `CoreMonitor.follow`/`protected`, a `CoreAmbient` struct, `CoreSnapshot.ambient`) and run `tools/tests/test-core-bridge.sh`.
- [x] T013 [P] Add a failing en-US and pt-BR key parity test in `native/macos/Tests/CameraLocalizationTests.swift`, then add the missing pt-BR key `cameraStatus.suspended_display_sleep` in `native/macos/TelaViva/Localization.swift`; run `tools/tests/test-camera-lifecycle.sh`.
- [x] T014 [P] Create `native/macos/TelaViva/BuiltInDisplayBackend.swift` with the `BuiltInDisplayBackend` protocol (availability with reason, read level percent, write level percent, read and set automatic brightness) and the real `dlsym`/Objective-C implementation per contracts.md; register it in `native/macos/TelaViva.xcodeproj/project.pbxproj` (four entries, then `plutil -lint`) and in the `SOURCES` array of `packaging/macos/build-app.sh`.

**Checkpoint**: ✋ Foundation ready: settings persist, snapshot carries the new fields, Swift decodes them, backend protocol exists

## Phase 3: Popup sizing (US1-3, P1) ⏰ 🎯 Quick win (~3h)

**Goal**: The popup fits 1, 2 or 3 monitors exactly and scrolls beyond 3.
**Independent Test**: With 1, 2, 3 and 4 external monitors the popup shows every card with no empty band, and scrolls only with 4 or more.

### Tasks

- [x] T015 [P] [US1-3] Add `native/macos/Tests/PopupLayoutTests.swift` and `tools/tests/test-popup-layout.sh` (executable, modeled on `test-hardware-key-router.sh`) with failing cases: 0 to 5 cards, 3 cards cap, differing card heights, screen cap smaller than the 3-card height, spacing.
- [x] T016 [US1-3] Create `native/macos/TelaViva/PopupLayout.swift` with `PopupLayout.listHeight(cardHeights:maxVisible:spacing:screenCap:)` and the card-height `PreferenceKey`; register it in `project.pbxproj` and `build-app.sh`; make T015 pass.
- [x] T017 [P] [US1-3] Add the popup layout test to the `native-arm64` job in `.github/workflows/macos.yml` and to the test list in `README.md` (lines near 49–55).
- [x] T018 [US1-3] Update `native/macos/TelaViva/MonitorPopup.swift`: measure each card's height, remove `.frame(width: 360, height: 420)` in favor of width 360 and content height, set the `ScrollView` height from `PopupLayout`, keep the empty-state minimum.
- [x] T019 [US1-3] Update `native/macos/TelaViva/MenuBarController.swift`: remove the duplicate `contentSize` (line ~44) and set `sizingOptions = .preferredContentSize` (or the measured-height fallback from T006), applying the visible-screen cap.
- [ ] T020 [US1-3] Manually validate 0, 1, 2, 3 and 4 monitors (use fewer if only one is available and say so) and record in `specs/archived/macbook-brightness-follow/validation/popup-and-settings.md`.

**Checkpoint**: ✋ Popup sizing complete and independently testable

## Phase 4: Larger Settings with tabs (US4, P1) ⏰ 🎯 Quick win (~3h)

**Goal**: A larger, resizable Settings window with General and Keyboard tabs; the Automatic brightness tab arrives in Phase 6.
**Independent Test**: Each tab shows its full content at the default size with no clipped control, and a setting changed in any tab persists.

### Tasks

- [x] T021 [US4] Restructure `native/macos/TelaViva/SettingsView.swift` into a `TabView` with General (general, OLED protection, availability sections) and Keyboard (hardware keys section) as `@ViewBuilder` sections, keeping one owner of every `@State` draft and of `save()`.
- [x] T022 [US4] Add the tab title keys to `native/macos/TelaViva/Localization.swift` in en-US and pt-BR.
- [x] T023 [US4] Update `native/macos/TelaViva/MenuBarController.swift` (window 640x720, add `.resizable`, minimum about 560x560) and align the minimum frame in `native/macos/TelaViva/App.swift`, keeping that scene's `SettingsView` initializer compiling through defaulted parameters.
- [ ] T024 [US4] Run `tools/tests/test-core-bridge.sh`, check each tab by hand (including a save round trip and a restart), and record in `specs/archived/macbook-brightness-follow/validation/popup-and-settings.md`.

**Checkpoint**: ✋ Settings window complete and independently testable

## Phase 5: Per-monitor protection (US10, US9 part, P2) ⏰ (~5h)

**Notes**: Code, tests and strings done and verified (65 core tests, clippy, fmt, all Swift scripts, whole-app typecheck). Misses are already tracked per monitor in the core, so re-protecting resets only that monitor's counter. A known short race: a Settings `save()` right after a protection toggle can resend the previous `protection_disabled` until the snapshot refreshes, the same exposure `screensaver_targets` already has. T031 (hardware validation) is pending.

**Goal**: Each monitor can opt in or out of OLED protection from the popup and from Settings, remembered by monitor ID.
**Independent Test**: With protection off for one of two monitors, presence loss and the screensaver dim only the other; toggling off while dimmed restores it; the setting survives restart and reconnection.

### Tasks

- [x] T025 [US10] Add failing tests in `crates/telaviva-core/tests/core_contract.rs`: presence dim skipped for an unprotected monitor, screensaver dim skipped, toggle off while dimmed restores through the existing read-before-write path, toggle on does not dim immediately, the camera does not run when no protected monitor is connected, default on for unknown IDs, `protected` snapshot flag.
- [x] T026 [US10] Add `is_protected(id)` in `crates/telaviva-core/src/core.rs` and gate the reasons in `reconcile_all` (`saver_dimmed` assignment, the dim reasons, the `min()` target) and the presence-absent path.
- [x] T027 [US10] In `crates/telaviva-core/src/core.rs`, clear a monitor's `presence_dimmed` and miss counter when it becomes unprotected and make `monitor_is_camera_eligible` and `all_eligible_displays_asleep` ignore unprotected monitors; run `cargo test -p telaviva-core` and `cargo clippy -- -D warnings` until T025 passes.
- [x] T028 [US10] Add the per-monitor protection toggle row to the card in `native/macos/TelaViva/MonitorPopup.swift` (shown when presence or screensaver protection is on, writes the full `protection_disabled` set through `core.updateSettings`) and hide the protection brightness row when unprotected.
- [x] T029 [US10] Add a connected-monitors list with the same toggle to the OLED protection section in `native/macos/TelaViva/SettingsView.swift`.
- [x] T030 [US10] Add the toggle and list strings to `native/macos/TelaViva/Localization.swift` in en-US and pt-BR.
- [ ] T031 [US10] Validate on hardware (toggle off during the screensaver, restart, reconnect) and record as "pending acceptance" in `specs/archived/macbook-brightness-follow/validation/protection.md`.

**Checkpoint**: ✋ Per-monitor protection complete and independently testable

## Phase 6: Follow the MacBook brightness (US5, US7, US9, P1) ⏰ 🎯 MVP part 1 (~8h)

**Notes**: Code, tests and strings done and verified: 82 core + 5 ffi tests, clippy, fmt, 9 Swift test scripts (the core-bridge script links `target/aarch64-apple-darwin/release/libtelaviva_core.a`, which must be rebuilt with `cargo build --release --target aarch64-apple-darwin -p telaviva-core` before running it), whole-app typecheck. Follow is authoritative: a readback that differs from the last desired value is overwritten, not adopted as baseline; a failed follow write is re-applied on the next built-in change or wake (the shell already retries each write 3 times). The monitor serializes reports with sequence numbers and checks availability on every tick. T046 (hardware validation) is pending.

**Goal**: With the feature on and a MacBook that supports it, calibrated monitors follow the built-in brightness; on other Macs the tab is disabled with a reason.
**Independent Test**: Using hand-entered curves (calibration UI comes in Phase 7), changing the MacBook brightness moves each calibrated monitor to the interpolated value within 2 s with no oscillation; on a Mac without adjustable built-in brightness the switch cannot be enabled.

### Tasks

- [x] T032 [US5] Add failing tests in `crates/telaviva-core/tests/core_contract.rs` for the pure curve function: anchors, between points, below 25% (from (0,0)), clamping, integer rounding, raw range clamp.
- [x] T033 [US5] Add failing tests in `crates/telaviva-core/tests/core_contract.rs` for the `builtin_display` command validation (bounds, non-finite, null brightness, unknown key, wrong types, state unchanged on error).
- [x] T034 [US5] Add failing tests in `crates/telaviva-core/tests/core_contract.rs` for follow behavior: eligible monitors get a write, ineligible ones none; baseline equals the mapped value and `manual_override` is untouched; dimmed or asleep monitors defer; protection restore returns the mapped value; one-unit dead band; queue-full retry; `calibrating:true` pauses; `available:false` stops; reconnect of the same ID reuses its curve and a different ID does not.
- [x] T035 [US5] Implement the `builtin_display` command, runtime state and the pure curve function in `crates/telaviva-core/src/core.rs` (allowed keys, dispatch, validation, `snapshot_value` `ambient` values).
- [x] T036 [US5] Add `Monitor.follow_raw` in `crates/telaviva-core/src/core.rs`, recompute it after `builtin_display`, `update_settings`, `monitor_discovered` and wake, set the baseline, and make the no-reason branch of `reconcile_all` target it (including for never-owned monitors); run `cargo test`, `cargo fmt --check` and `cargo clippy -- -D warnings` until T032–T034 pass.
- [x] T037 [P] [US5] Add a C ABI test for `builtin_display` in `crates/telaviva-core/tests/ffi_contract.rs`.
- [x] T038 [P] [US5] Add `native/macos/Tests/BuiltInBrightnessMonitorTests.swift` and `tools/tests/test-builtin-brightness-monitor.sh` with failing cases against a fake backend: whole-percent quantization, forward only on change of 1 or more, 250 ms trailing debounce, failed read keeps the last value, no polling while asleep, calibrating or disabled, availability transitions, a missing built-in display or `CanChange` false reports unavailable with a reason.
- [x] T039 [US5] Create `native/macos/TelaViva/BuiltInBrightnessMonitor.swift` (1 s timer with 0.5 s leeway, quantize, debounce, availability, built-in selection by `CGDisplayIsBuiltin` plus online and active, push upgrade only if T003 allowed it); register it in `project.pbxproj` and `build-app.sh`; make T038 pass.
- [x] T040 [US5] Add `submitBuiltInDisplay(available:brightness:calibrating:)` to `native/macos/TelaViva/CoreBridge.swift` using the raw `core.submit` path, with a test in `native/macos/Tests/CoreBridgeTests.swift`.
- [x] T041 [US5] Wire the monitor in `native/macos/TelaViva/MenuBarController.swift`: lazy creation, start after `ddc.start`, stop on both termination paths, re-evaluate on screen parameter change and wake, follow the enabled flag from the snapshot.
- [x] T042 [US7] Create `native/macos/TelaViva/AmbientBrightnessView.swift` (the Automatic brightness tab: enable switch, disabled state with the explanation from the backend reason, connected-monitor list showing `follow` eligibility and why a monitor is excluded); register it in `project.pbxproj` and `build-app.sh`.
- [x] T043 [US5] Add the tab to `native/macos/TelaViva/SettingsView.swift`, disabled when `ambient.available` is false.
- [x] T044 [US7] Add the tab, availability and eligibility strings to `native/macos/TelaViva/Localization.swift` in en-US and pt-BR.
- [x] T045 [P] [US5] Add the new test script to `.github/workflows/macos.yml` and `README.md`.
- [ ] T046 [US5] Validate on a MacBook with a DDC monitor and a hand-edited `ambient_curves` entry (keys, Control Center, automatic ramp, lid closed, idle CPU with the feature on and off) and record as "pending acceptance" in `specs/archived/macbook-brightness-follow/validation/follow.md`; list the Mac mini case as unverified.

**Checkpoint**: ✋ Follow works end to end with stored curves and degrades safely

## Phase 7: Calibration (US6, P1) ⏰ 🎯 MVP part 2 (~7h)

**Notes**: Code, tests and strings done and verified (10 Swift test scripts, whole-app typecheck, core lib rebuilt). Restore order is built-in level, automatic brightness (only if it was on), monitor brightness, then `calibrating:false`; the session file is recovered at launch and ended on every trigger (window close, display sleep, built-in unavailable, quit). Deviations from the first draft: end reasons are `done/cancelled/leftTab/windowClosed/appQuit/displaySleep/builtInUnavailable`; a failed first write of the built-in level does not touch the monitor; a session file that cannot be saved aborts the start with the `recoveryFailed` notice; the controller notice is rendered once under the monitor list. T053 (hardware validation) is pending.

**Goal**: The user calibrates each monitor at 25, 50, 75 and 100% by comparing it to the MacBook screen, and the MacBook state is always restored.
**Independent Test**: Calibrating one point sets the MacBook to that level, shows the monitor slider, saves a monotonic point, and afterwards the MacBook brightness, automatic-brightness setting and monitor brightness are as before, including after a forced quit.

### Tasks

- [x] T047 [P] [US6] Add `native/macos/Tests/AmbientCalibrationTests.swift` and `tools/tests/test-ambient-calibration.sh` with failing cases against a fake backend and a temp directory: state capture, restore order (built-in level, automatic brightness only if it was on, monitor brightness, then `calibrating:false`), cancel, every end trigger (done, tab left, window closed, display sleep, built-in unavailable, quit), recovery from a leftover session file, an unreadable file, automatic brightness not toggleable (calibration still runs with a notice), slider range clamped between neighboring points.
- [x] T048 [US6] Create `native/macos/TelaViva/AmbientCalibrationController.swift` (main-thread `ObservableObject` session state machine, atomic `calibration-session.json` per contracts.md, `recoverIfNeeded()`); register it in `project.pbxproj` and `build-app.sh`; make T047 pass.
- [x] T049 [US6] Add the calibration UI to `native/macos/TelaViva/AmbientBrightnessView.swift`: four point buttons per monitor with calibrated state, the comparison slider driven through `ManualMonitorControls.setValue` with the clamped range, confirm and cancel, and the automatic-brightness notice; confirmation sends only the changed curve through `core.updateSettings`.
- [x] T050 [US6] In `native/macos/TelaViva/MenuBarController.swift`, run `recoverIfNeeded()` at launch, end the session when the Settings window closes and on both termination paths and on display sleep.
- [x] T051 [US6] Add the calibration strings to `native/macos/TelaViva/Localization.swift` in en-US and pt-BR.
- [x] T052 [P] [US6] Add the calibration test script to `.github/workflows/macos.yml` and `README.md`.
- [ ] T053 [US6] Validate on hardware (all four points, cancel, force-quit mid-calibration then relaunch, automatic brightness on before and after) and record as "pending acceptance" in `specs/archived/macbook-brightness-follow/validation/calibration.md`.

**Checkpoint**: ✋ Calibration complete; with Phase 6 the headline feature is usable

## Phase 8: Popup in follow mode and keys (US8, P2) ⏰ (~4h)

**Notes**: Code and tests done and verified (10 Swift test scripts, whole-app typecheck). The router set is config-like, like the key steps: it survives disable and permission loss and a press already latched keeps its outcome. A followed monitor caps its protection brightness against the snapshot brightness. T058 (hardware validation) is pending.

**Goal**: Followed monitors show volume (and the protection row) only, and brightness keys control the MacBook for them.
**Independent Test**: With follow active, a calibrated monitor's card has no brightness slider while an uncalibrated one still does; pressing the brightness key over a followed monitor changes only the MacBook (the monitor then follows), and over an unfollowed monitor behaves as before.

### Tasks

- [x] T054 [US8] Add failing tests in `native/macos/Tests/HardwareKeyRouterTests.swift`: a brightness key whose target monitor is in the followed set passes through and its key-up and repeats pass through, volume and mute are unaffected, an unfollowed monitor is still handled, an empty set restores routing, `processForTesting` sets the set explicitly.
- [x] T055 [US8] Add `setFollowedBrightnessDisplays(_ ids: Set<String>)` to `native/macos/TelaViva/HardwareKeyRouter.swift` using the existing pass-through latch, with no `CoreSettings` or `CoreBridge` references; run `tools/tests/test-hardware-key-router.sh`.
- [x] T056 [US8] In `native/macos/TelaViva/MenuBarController.swift`, compute the followed IDs from the snapshot (`ambient.active` and `follow == "eligible"`) and push them to the router on every snapshot.
- [x] T057 [US8] Update `native/macos/TelaViva/MonitorPopup.swift` to hide the brightness slider for followed monitors only when `ambient.active && follow == "eligible"`, and make the protection brightness row and its cap work without the brightness control; run `tools/tests/test-manual-monitor-controls.sh`.
- [ ] T058 [US8] Validate the popup variants, the card height with the brightness row hidden, and the key behavior on hardware, and record as "pending acceptance" in `specs/archived/macbook-brightness-follow/validation/follow.md`.

**Checkpoint**: ✋ Follow mode UI and keys complete

## Final Phase: Polish ✅ (~4h)

**Notes**: Done. ADRs 0007 and 0008, `api.md`, `ARCHITECTURE.md` and `CHANGELOG.md` written; `cargo fmt --all --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace` (82 + 5 + 4 + 1 tests) and all ten `tools/tests/test-*.sh` pass; whole-app `swiftc -typecheck` is clean. `fix-idea-diagnostics`: SonarLint headless covers only the changed YAML (clean); the IntelliJ inspections returned no problems for the 26 changed Rust and Swift files, which does not prove the IDE analyzes those languages, so the compilers are the real check. The per-area validation files planned in T006, T020, T024, T031, T046, T053 and T058 were consolidated into `validation/hardware-checklist.md`, which is the single place to record hardware results. Success criteria status: SC-008 met; SC-001 to SC-007, SC-009 and SC-010 have their logic covered by tests and remain pending acceptance on hardware (SC-005 also needs a Mac without a built-in display).

- [x] T059 [P] Write `adr/0007-private-display-apis-for-built-in-brightness.md` (format of ADR 0006: Status, Date, Feature, Spec, Context, Decision, Alternatives, Consequences, Non-Obvious Constraints).
- [x] T060 [P] Write `adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md` in the same format.
- [x] T061 [P] Update `api.md` (command list, monitor and `ambient` snapshot fields, settings fields, the downgrade caveat for the three additive fields).
- [x] T062 [P] Append a `## macbook-brightness-follow` section to `ARCHITECTURE.md` linking both ADRs and the spec, stating what is unverified, and update `CHANGELOG.md` (Unreleased: Added and Changed, hardware checks marked "pending acceptance").
- [x] T063 Run `cargo fmt --check`, `cargo test`, `cargo clippy -- -D warnings` and every `tools/tests/test-*.sh`, and fix failures.
- [x] T064 Run `/codeadvisor-dev:fix-idea-diagnostics` on the changed files, rerun the checks, and confirm `git diff` contains no unrelated changes.
- [x] T065 Re-check each success criterion SC-001 to SC-010 against the validation notes; anything not hardware-verified stays marked "pending acceptance".

## Dependencies & Execution Order

```
Phase 1 (spikes) ──┐
                   ├─> Phase 2 (Foundational, blocks all)
                   │        │
                   │        ├─> Phase 3 (popup)     ┐ independent of each other,
                   │        ├─> Phase 4 (Settings)  ┘ both are quick wins
                   │        │        │
                   │        │        ├─> Phase 5 (per-monitor protection: its Settings list needs Phase 4 tabs)
                   │        │        └─> Phase 6 (follow: its tab needs Phase 4) ─> Phase 7 (calibration) ─> Phase 8 (follow-mode UI and keys)
                   │        │
                   └────────┴──────────────────────────────> Final Phase (Polish)
```

- Phase 1 only feeds decisions; it can overlap with Phase 2 except T006, which gates Phase 6's polling and Phase 3's popover approach.
- Phases 3 and 4 do not depend on each other, but both edit `MenuBarController.swift` (T019, T023), so run them one after the other when one person does the work.
- Phase 5 core work (T025–T027) is independent of Phase 6 core work, but both edit `core.rs`; sequence them. Phase 5 UI tasks need Phase 4.
- Phase 7 needs Phase 6 (backend, monitor and tab). Phase 8 needs Phase 6 and benefits from Phase 7 (curves to make monitors eligible).
- Within a phase, test tasks precede the code they verify; `[P]` tasks can run together.

## Implementation Strategy

- **Quick wins first**: Phases 3 and 4 deliver items 1 and 2 of the request without touching the follow feature, and can ship separately.
- **MVP for the headline feature** (item 3): Phases 1, 2, 6, 7 and 8 (about 25h). Without Phase 7 no curve can be created; without Phase 8 the popup still shows brightness sliders for followed monitors.
- Phase 5 (per-monitor protection) is a separate request made during discovery and can follow after the MVP.
- Delegation: the orchestrator reads, supervises and verifies every phase's checks; edits go to Haiku subagents with the exact files, before and after, and project conventions. Independent files in the same phase (`[P]`) can go to separate agents in one message; edits to the same file go to one agent. Hardware tasks (T001–T005, T020, T031, T046, T053, T058) need the user and cannot be delegated.

## Progress Tracking

⏳ Not Started · ⏰ In Progress · ✅ Completed — update as you go, including the Notes sections per phase.

| Phase | Status | Notes |
|-------|--------|-------|
| 1 Setup and spikes | ⏳ | |
| 2 Foundational | ⏳ | |
| 3 Popup sizing | ⏳ | |
| 4 Settings tabs | ⏳ | |
| 5 Per-monitor protection | ⏳ | |
| 6 Follow | ⏳ | |
| 7 Calibration | ⏳ | |
| 8 Follow-mode UI and keys | ⏳ | |
| Polish | ⏳ | |
