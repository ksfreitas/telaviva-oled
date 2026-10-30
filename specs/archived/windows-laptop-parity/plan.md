# Tasks: Windows parity for the laptop display protection feature

**Source**: `specs/archived/laptop-display-protection-and-auto-brightness/windows-handoff.md` (sections 3 and 7 are the detailed design; this file only tracks execution).
**Scope**: every item of the handoff that is not implemented on Windows. Not in scope: `lock-macos-settings-window-size` (scoped to macOS by the owner), 7.6(d) wake re-read and 7.6(e) in-place rediscovery (the handoff marks both as undecided).
**Decisions taken for the handoff's open questions** (the proposed option in each case): protection slider and threshold use the handoff's proposals; `primary` is sent at discovery only; the camera picker hides built-in cameras while the lid is closed; the adaptive give-up (handoff 7.5 item 6) was NOT decided and is NOT implemented; the 90 s monitor-recovery give-up (monitor part only) is implemented.
**Conventions**: en-US comments and docs; UI strings in en-US and pt-BR; follow `native/windows` style; Windows wording only ("Laptop" en-US, "Notebook" pt-BR). Tests first.

## Phase A: Small settings changes (7.2, 7.3, 7.6c)

- [x] A1 Presence threshold minimum 0 (`kMinBrightnessMin` in `SettingsModel.cpp` and `GeneralPage.xaml.cpp`), tests, help text.
- [x] A2 Protection level fixed 1..50 (`ProtectionRules.h`, `AppController.cpp`, `FlyoutState.h`, `MonitorCard.xaml.cpp`, `GeneralPage.xaml.cpp`), tests in `protection_rules.cpp`.
- [x] A3 No settings save before the first snapshot.

Notes: the A3 guard blocks all submits before the first snapshot, not only settings saves.

## Phase B: Codec, settings, follower (T-W1, T-W2, T-W3)

- [x] B1 `JsonCodec`: `protectable`, `holding`, `builtin_protection`, `monitored_display*`, `camera_history`, `primary`, `builtin_auto_brightness_off_on_protection`.
- [x] B2 `SettingsModel`: new option, `presence_min_brightness_by_display`, `buildSelectMonitoredDisplayPayload`.
- [x] B3 `LaptopPanelFollower`: flags and protection polling; `ILaptopPanel` supported levels.

Notes: none beyond the decisions above.

## Phase C: Protection controller, shared owner, calibration recovery (T-W4, T-W5, 7.5)

- [x] C1 `AutoBrightnessOwner` and `AdaptiveBrightness` read helper.
- [x] C2 `AmbientCalibrationController` on the owner, plus the 7.5 monitor recovery (same file, one worker).
- [x] C3 `BuiltInProtection` controller and visibility helper.

Notes: the adaptive give-up (7.5 item 6) is not done, although the decisions above name it. 7.6 (d) and (e) are not done.

## Phase D: Wiring and UI (T-W6, T-W7)

- [x] D1 `AppController` wiring (publish, launch, quit, `canCalibrate`, `setHolding`).
- [x] D2 Popup card, General page rows, option switch, strings.

Notes: follow-ups D3 and N1 are the notices and a non-blocking slider write, and the owner's `notChanged` outcome. They are not tracked as tasks above.

## Phase E: Monitored display and per-display threshold (7.1, 7.8)

- [x] E1 `primary` flag through the catalog to `monitor_discovered`.
- [x] E2 Picker, `MonitoredDisplayRules.h`, per-display threshold, strings.

Notes: the monitored picker sits below the threshold row, as on macOS.

## Phase F: Lid closed camera (7.4)

- [x] F1 `CameraSelection`, `CameraLifecycleDriver`, lid forward in `AppController`.

Notes: none beyond the decisions above.

## Phase G: Verification and docs (T-W8, T-W9)

- [x] G1 Full build, `tests.exe`, `cargo test --workspace`.
- [x] G2 Hardware rows L4-L6, W-L8, W-L9 as "Not run"; handoff status; `docs/user-guide.md`; `CHANGELOG.md` Unreleased.

Notes: `scripts\build-windows.ps1 -NoZip` passes with 742 tests and 0 failures; `cargo test --workspace --all-targets --locked` passes. No Windows hardware row was run. The Windows rows already existed as W-L4 to W-L10 and were extended instead of duplicated.
