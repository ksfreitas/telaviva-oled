# Windows handoff: laptop display protection and automatic brightness

**Feature**: laptop-display-protection-and-auto-brightness (plan Phase 7, task T032)
**Sources**: [spec.md](spec.md), [architecture.md](architecture.md) ("Windows Handoff" and Decisions 3, 4, 7, 13), [contracts.md](contracts.md), and the macOS implementation as the template.
**Checklist**: [validation/hardware-checklist.md](validation/hardware-checklist.md)

> **Status: implemented on Windows in code, with unit tests. NOT verified on Windows hardware.**
>
> The Windows shell implements the tasks in section 3 (T-W1 to T-W9) and the follow-ups in section 7 that are listed below as done. The native Windows tests pass, and the build was verified by the orchestrator. No Windows hardware row has been run; the rows in [validation/hardware-checklist.md](validation/hardware-checklist.md) are all Not run.
>
> Not implemented:
> - Section 7.6 (d): wake re-read of a known monitor that was discovered with brightness unsupported.
> - Section 7.6 (e): in-place rediscovery (a short grace before `monitor_disconnected`).
> - The adaptive give-up in section 7.5, item 6 (undecided: delete the record and show a notice, or keep blocking).
> - `primary` is sent only inside `monitor_discovered`, so a change of the Windows primary display applies at the next discovery.

## 1. What the shared core already provides

The core (`crates/telaviva-core`) decides; the shell applies. Windows has no shell code for this yet.

- `src/builtin.rs`: reserved id `BUILTIN_ID` (line 5), the reasons helper (line 32) and the target helper (line 55).
- `src/core.rs`: the `builtin_display` handler, the freeze rule (`builtin_frozen`), `follow_target`, the presence and screensaver logic, camera gating (`camera_gate_active` and `monitored_below_threshold`), `shutdown_policy` and the snapshot object (`snapshot_value`).
- `src/settings.rs`: `builtin_auto_brightness_off_on_protection` (line 59; default false at line 255; `#[serde(default)]`).
- Tests: `crates/telaviva-core/tests/builtin_protection_contract.rs` and the setting tests in `crates/telaviva-core/tests/core_contract.rs` (line 201 onward).

Contract (full text in [contracts.md](contracts.md)):

- `builtin_display` keeps `available`, `brightness` (0..100 or null) and `calibrating`. It adds two optional booleans, default false:
  - `protectable`: a built-in screen exists and its brightness is adjustable. True also while the lid is closed.
  - `holding`: the shell holds the screen dimmed and has not finished restoring it.
  A non-boolean value is rejected before any state changes ("protectable must be boolean", "holding must be boolean").
- Snapshot: top-level `builtin_protection` with `protectable`, `protected`, `available`, `reasons`, `target_percent`, `hold_auto_brightness` and `holding`.
- Setting: `builtin_auto_brightness_off_on_protection` (boolean, default false). The schema stays at 1.
- Reserved id `"builtin"`: opt-out in `protection_disabled`, dim level in `screensaver_targets`. It never appears in `monitors`.
- The C ABI stays at revision 1. The change is JSON only.

Windows today: `buildBuiltinDisplay` (`native/windows/Platform/JsonCodec.cpp`, around line 671) sends only `available`, `brightness` and `calibrating`. `protectable` is never sent, so the core's built-in policy never activates on Windows. The `builtin_protection` object in each snapshot is always the inert default. The Windows codec reads named keys only (per [contracts.md](contracts.md)).

## 2. Visibility rule (owner request, 2026-10-20)

The built-in screen is listed in the popup card and in the General page protection list **only while `builtin_protection.protectable && builtin_protection.available` is true**.

- Lid closed or display off: `available` is false, so the screen is hidden from both lists. `protectable` stays true, so camera and display-sleep logic still see it (architecture Decision 3).
- Available again: the screen reappears on the next snapshot.
- No `WmiMonitorBrightness` or no internal panel: `protectable` is false, so the screen never appears.
- Put this rule in one pure helper (see T-W7), so the card, the General page and the tests all use the same code.

## 3. Ordered tasks (test first)

Run the tasks in this order. Each task writes its tests before its code.

**Ordering rule**: keep `protectable` false in the Windows shell until T-W6 is wired. With `protectable` true, the core starts reasons and camera gating for the built-in screen, and nothing would dim or restore it.

### T-W1. JsonCodec: report flags and snapshot parsing

Files:
- `native/windows/Tests/json_codec.cpp`: next to `json_codec_builtin_display_null_brightness` (line 203).
- `native/windows/Tests/fixtures/snapshot-basic.json`: add a `builtin_protection` object, only if no other test compares the whole fixture (check first).
- `native/windows/Platform/JsonCodec.h`: new struct next to `AmbientSnapshot` (line 97) holding the fields of the snapshot object; new member next to `AmbientSnapshot ambient;` (line 129); the `buildBuiltinDisplay` declaration (line 146) gets `protectable` and `holding`, both defaulting to false.
- `native/windows/Platform/JsonCodec.cpp`: `buildBuiltinDisplay` (around line 671) writes both booleans; `parseSnapshot` (line 857) reads `builtin_protection` and uses the inert default when the key is absent.

Tests first:
- The builder writes `protectable` and `holding`. The legacy call (no flags) writes both as false.
- `parseSnapshot` reads every field of `builtin_protection`. A snapshot without the key gives `protectable == false`.

Acceptance: json_codec tests pass; the existing builtin_display test is unchanged; `core_bridge_smoke` still asserts `monitors.empty()` (`native/windows/Tests/core_bridge_smoke.cpp`, line 65).

### T-W2. Settings: the new option

Files:
- `native/windows/Tests/settings_payload.cpp`: `settings_payload_has_exactly_the_core_keys` (line 183). The key count changes from 16 to 17, and `kSettingsKeys` (line 162) gets `builtin_auto_brightness_off_on_protection`.
- `native/windows/Platform/SettingsModel.h`: `SettingsDraft` (line 14) gets `builtinAutoBrightnessOffOnProtection = false`.
- `native/windows/Platform/JsonCodec.h`: `SettingsSnapshot` (line 40) gets the field; `parseSnapshot` reads it.
- `native/windows/Platform/SettingsModel.cpp`: `draftFromSnapshotSettings` (line 159) copies the field; `buildSettingsPayload` (line 217) writes it.

Tests first: the key count and key set; a snapshot with the option `true` gives `true` in the payload after `draftFromSnapshotSettings`.

Why the copy matters: the Windows shell sends the whole settings object, and the core applies each key it receives (`update_settings` in `crates/telaviva-core/src/core.rs`). If `draftFromSnapshotSettings` did not copy the option, any other Windows setting change would write `false` and silently turn the option off.

Acceptance: settings tests pass with 17 keys, and no other key changes.

### T-W3. LaptopPanelFollower: publish the flags and the level

Files:
- `native/windows/Tests/laptop_panel.cpp`: `FakePanel` (line 27) and the existing tests (lines 86-281).
- `native/windows/Platform/LaptopPanel.h`: `LaptopPanelFollower` (line 53). `Report` (line 78) gets `protectable` and `holding` and includes them in `operator==`. New setters `setProtectionEnabled(bool)` and `setHolding(bool)`.
- `native/windows/Platform/LaptopPanel.cpp`: `monitoringLocked` (line 517) becomes `(followEnabled_ || protectionEnabled_) && available_ && !calibrating_`. `publishLocked` (line 522) passes both flags to `buildBuiltinDisplay` (line 532). `setHolding` resubmits even when the level has not changed.

Rules:
- `protectable` is true when the detection reason is `None` or `DisplayOffline`. This mirrors `BuiltInProtectionController.isProtectable(reason:)` in `native/macos/TelaViva/BuiltInProtectionController.swift`. The reason comes from `detectLaptop` (`native/windows/Platform/LaptopDetection.cpp`, line 43).
- Protection needs the level even when follow is off, because the camera brightness gate uses `builtin_percent` (`monitored_level_percent` in `core.rs`).

Tests first:
- Follow off and protection off: no WMI reads. The existing test `laptop_panel_disabled_follow_never_polls` (line 107) stays unchanged.
- Protection on and follow off: the level is polled and reported.
- Lid closed: `available` false and `protectable` true.
- WMI brightness missing (`ApiMissing`): `protectable` false.
- A `holding` change resubmits the report with the same level.

Acceptance: laptop_panel tests pass. No WMI read happens when neither feature needs the level (spec FR-018).

### T-W4. Platform/BuiltInProtection (new)

Files:
- New: `native/windows/Platform/BuiltInProtection.h` and `.cpp`.
- New: `native/windows/Tests/built_in_protection.cpp` (picked up by the `*.cpp` wildcard in `native/windows/Tests/Tests.vcxproj`).
- Changed: `native/windows/Platform/ILaptopPanel.h` gets a supported-levels accessor (the verify step needs it). Implement it in `WmiPanel` (`native/windows/Platform/LaptopPanel.cpp`) from `readLevels` (line 135), and in the `FakePanel` of the tests.
- Changed: `native/windows/Platform/AppPaths.h` and `.cpp`: accessor `builtinProtectionSessionFile()`, next to `calibrationSessionFile()` (`AppPaths.h`, line 18).

Behavior, mirroring `native/macos/TelaViva/BuiltInProtectionController.swift`:
- **Dim**: read the level fresh with `readPercent()`. The follower's report stores an integer level and is never used for the baseline. Write the record `builtin-protection-session.json` (`version`, `started_at`, `baseline_percent`) atomically before any change. Set `holding` and report it. If the option is on, acquire the shared owner (T-W5). Write `min(baseline, target)`, then verify.
- **Verify**: accept the readback when it is within 1 point of the supported level nearest to the target. For 0%, the nearest supported level is the lowest one. WMI snaps each write to a supported level (`WmiPanel::setPercent` writes the nearest one), so an exact match is not reliable. The macOS controller also trusts a verified level for 5 seconds while the same decision repeats, without a readback. Mirror that for dims only; a restore is never throttled.
- **Restore**: write the baseline (as the nearest supported level), verify the same way, release the owner, delete the record and report `holding` false. Make up to 3 attempts, 1 s apart (`IScheduler::scheduleOnce`, `native/windows/Platform/Scheduler.h`, line 15). After 3 failed attempts the restore is not capped: each later snapshot, availability change or launch tries again, as on macOS. Never write while unavailable.
- **Launch**: a leftover record means holding starts with the stored baseline. Do not read the level again, because the screen is dim. The next snapshot without reasons restores it. An unreadable record is deleted and logged.
- **Quit**: one synchronous restore attempt and no timers. A failed write keeps the record for the next launch. Ignore later snapshots.
- **Level-triggered**: `reconcile` runs on every snapshot. A change of target, option or availability starts a new attempt count.
- **Threading**: `WmiPanel` serializes its COM work on one worker (`native/windows/Platform/LaptopPanel.h`, lines 24-25). Pick one thread for the controller (the thread that calls `publish`, see T-W6) and write it in the header.

Tests first, with a fake `ILaptopPanel`, a fake `IPowerSettings`, `FakeScheduler` (`native/windows/Tests/Fakes.h`, line 37) and a temp directory. Mirror `native/macos/Tests/BuiltInProtectionTests.swift`:
- Dim and restore. Presence and screensaver minimum. No writes while unavailable. An unreadable baseline skips the dim. An unwritable record skips the dim.
- A failed write retries, then parks at the 3-attempt limit.
- A leftover record at launch: holding with the stored baseline, no fresh read, restore on the first clear snapshot.
- The nearest supported level is accepted. 0% means the lowest level.
- The quit restore runs once, and later snapshots do not dim the screen.

Acceptance: all new tests pass with fakes. No test touches WMI.

### T-W5. Shared automatic-brightness owner and calibration refactor

Files:
- New: `native/windows/Platform/AutoBrightnessOwner.h` and `.cpp`. Mirrors `native/macos/TelaViva/BuiltInAutoBrightnessOwner.swift`, with holders `calibration` and `protection`.
- New: `native/windows/Tests/auto_brightness_owner.cpp`.
- Changed: `native/windows/Platform/AdaptiveBrightness.h` (class at line 30) and `.cpp` (`suspend` at line 103, `restore` at line 144). Keep both behaviors unchanged for calibration. Add a read helper for the setting's current state (the AC and DC values of the saved scheme).
- Changed: `native/windows/Platform/AmbientCalibrationController.cpp`. The suspend call (around line 301, with its persist-first callback into `session_.begin`) and the restore call (around line 351, in `restoreState`) become `acquire(calibration)` and `release(calibration)`, at the same positions and in the same order. `begin` refuses while protection holds, with the notice `calibration.notice.protectionActive`.
- Changed: `native/windows/Platform/AmbientCalibrationController.h` (`adaptive_` member, line 176) takes the owner.
- Changed: `native/windows/Platform/AppPaths.h` and `.cpp`: accessor for `auto-brightness-hold.json`.
- Unchanged: `native/windows/Platform/CalibrationSession.cpp` (`recover`, line 135). `calibration-session.json` keeps its format.

Rules (from the Swift owner):
- The first acquire reads the state. If adaptive brightness is on, write `auto-brightness-hold.json` first (scheme GUID, AC value, DC value), then turn it off. If it is off, or not applicable (desktop), write no record. If unreadable, return the `unknown` outcome, log a diagnostic and write nothing.
- Later acquires only add a holder and return the first outcome.
- The last release restores unless a fresh read says the setting is on (a nil or off read still restores, because this owner turned the setting off). Then delete the record. If the restore fails, keep the record and mark a pending restore, so the next acquire joins it instead of reading "off" as the user's choice.
- Launch: restore from the record, then delete it, or keep it on failure.

Note: today `AdaptiveBrightness::restore` writes the saved AC and DC values to the saved scheme in all cases. It applies the scheme only when that scheme is the active one. It does not check whether the setting is on. The owner adds a fresh read that skips the restore when the setting is on. Do not change `restore` for calibration. The existing tests (`native/windows/Tests/adaptive_brightness.cpp`, lines 106-257) must pass unmodified.

Tests first: owner tests in `auto_brightness_owner.cpp` cover holders, restore unless the setting is on, a user change during the hold (no write), desktop (no record), unknown read, a failed restore that keeps the record, and launch recovery. Calibration tests in `native/windows/Tests/ambient_calibration.cpp` (its `FakePower`, line 162) keep their exact call order. A fixture change that passes an owner with a temp store is allowed. No assertion changes.

Acceptance: all existing adaptive and calibration tests pass, and the order of `IPowerSettings` calls during calibration is identical.

### T-W6. AppController wiring

File: `native/windows/TelaViva/Host/AppController.cpp`. No unit test exists for this file. The acceptance is the build plus rows L4-L6.

- `Services::publish` (line 623): next to `laptopFollower->setFollowEnabled(follow)` (line 651), call the follower's `setProtectionEnabled(protectable && protected)`. Call the controller's `reconcile(snapshot.builtinProtection)` on every snapshot. Skip it once shutdown has begun, as the macOS `terminating` guard does (`native/macos/TelaViva/MenuBarController.swift`, line 143).
- Launch (around lines 1266-1279): owner `recoverIfNeeded`, then protection `recoverIfNeeded`, then the existing calibration `recoverPending` (line 1274). All three run before the bridge sink is added (line 1279).
- Quit (`AppController::shutdown`, line 1336): calibration end with the app-quit reason, then protection `restoreNow()`, then the follower stop (line 1367), all before the core shuts down. This mirrors `MenuBarController.swift` lines 383-385.
- `canCalibrate` (line 1545) also returns false while protection holds.
- Report `holding` back to the follower with `setHolding`, from the controller's callback. This mirrors `setProtectionHolding` in `MenuBarController.swift` (around line 261).
- Lid and display events are already wired: `onLidChanged` (line 1218), `onDisplayStatusChanged` (line 1115), `PowerEvents` (line 1223) and `SessionEvents` (line 1128). Add no new event wiring. Availability reaches the controller through the next snapshot.

### T-W7. Popup card, General page and strings

Files:
- New helper in `native/windows/Platform/BuiltInProtection.h`: the visibility rule from section 2 as a pure function, tested in `built_in_protection.cpp`.
- `native/windows/TelaViva/Host/AppController.cpp`: `buildCard` is a free function (line 387), called from the monitor loop (line 445). Add the built-in card only when the helper returns true. The card has:
  - the name;
  - a protection switch that writes the whole `protection_disabled` set with `builtin` added or removed (as `setMonitorProtected` does, line 1410);
  - a brightness slider that writes through the controller's panel write, not through the core, and is disabled while `holding`, while reasons are active, or while unavailable;
  - a protection brightness row that writes `screensaver_targets["builtin"]` and shows only when presence or screensaver protection is on and the screen is protected (the macOS rule; as `setProtectionBrightness` does, line 1419);
  - a notice line.
  There is no volume control.
- `native/windows/TelaViva/PopupWindow.xaml.cpp` and `native/windows/Platform/PopupLayout.h`: change only if a new row kind is needed.
- `native/windows/TelaViva/GeneralPage.xaml.cpp` (protection list, around lines 634-650): add the built-in switch under the same visibility rule. Add the option switch below it, written through `edit(...)` on `SettingsDraft`.
- `native/windows/TelaViva/Strings/en-US/Resources.resw` and `native/windows/TelaViva/Strings/pt-BR/Resources.resw`: the new keys listed below.
- `native/windows/Tests/strings_keys.cpp`: the existing checks apply to the new keys. Parity (line 136), no "Mac" or "MacBook" (line 170), and pt-BR avoids "Ajustes" (line 265).

Strings. Mirror the macOS keys in `native/macos/TelaViva/Localization.swift`, but write Windows wording. Do not copy the macOS text.
- `builtin.name`: "Laptop screen" (en-US) and "Tela do Notebook" (pt-BR). Proposed values.
- `builtin.protection.toggle.help`
- `builtin.protection.option.title` and `builtin.protection.option.help`: the text must name the Windows setting "Change brightness automatically when lighting changes" (en-US). It must not read as the app's "Automatic brightness" page (`native/windows/TelaViva/AutomaticBrightnessPage.xaml.cpp`). The pt-BR name of the Windows setting is not verified.
- `builtin.notice.autoBrightnessUnknown`, `builtin.notice.autoBrightnessNotChanged`, `builtin.notice.restoreFailed`
- `builtin.slider.disabledWhileProtected`
- `calibration.notice.protectionActive`
- Reuse existing Windows keys for generic labels (brightness, protection brightness, protect this monitor) if they exist. This was not checked.

Acceptance: the strings tests pass. The card and the General page list the built-in screen only under the visibility rule. The option persists across a relaunch (row L5).

### T-W8. Build and run the native tests

Commands, from the repository root in Windows PowerShell:

- One command (builds the Rust DLLs, the app and the zip, then runs the tests): `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1`. Options: `-Configuration Debug`, `-SkipTests`, `-NoZip` (`scripts/build-windows.ps1`; `packaging/windows/README.md`).
- The same steps as CI (`.github/workflows/windows.yml`):
  1. Rust DLLs: `powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\build-rust-dlls.ps1 -Configuration Release` (line 182).
  2. MSBuild 17.14 from Visual Studio 2022: `msbuild native\windows\TelaViva.sln /restore /p:Platform=x64 /p:Configuration=Release /m /nr:false` (line 200). `Platform.vcxproj` and `Tests.vcxproj` use `*.cpp` wildcards, so new source files are picked up.
  3. Tests: set `TELAVIVA_TEST_FIXTURES` to `native\windows\Tests\fixtures`, put `native\windows\build\rust` first on `PATH`, and run `native\windows\build\x64\Release\tests.exe`. The exit code must be 0 (lines 205-215).
- Prerequisites: `README.md`, "Requirements" (Windows 11 x64, Visual Studio 2022 with the C++ workload, Roslyn, Windows SDK 10.0.26100, Windows App SDK C++ component).
- Run the Rust checks too (`cargo test --workspace --all-targets --locked`, see `README.md`, "Tests and checks"). The Windows DLL is built from the same core.

Acceptance: the build succeeds, `tests.exe` exits with 0, and the only existing test changed is `settings_payload_has_exactly_the_core_keys`.

### T-W9. Hardware rows

File: `specs/archived/laptop-display-protection-and-auto-brightness/validation/hardware-checklist.md`. It has no Windows rows yet (T033 is pending). Add them with status "Not run", in the format of `specs/archived/windows-version/validation/hardware-checklist.md` (its rows L1-L3 are at lines 69-71):
- L4: protection on the laptop panel (presence and screensaver), the level, WMI snapping (0% is the lowest level), and the card hidden with the lid closed.
- L5: automatic brightness during protection. Option on: turned off and restored (AC and DC). Option off: untouched. The power scheme unchanged.
- L6: force-quit recovery. The next launch restores the screen brightness and automatic brightness.
- The brief asked for L4-L7. The plan (T033) and the architecture define L4-L6. A seventh row (for example, lid close and open while dimmed) is proposed here, not decided.

Acceptance: the rows exist, and none is marked as run.

## 4. Differences from macOS to respect

- **Display sleep is one global state on Windows.** `handleDisplayState` (`native/windows/Platform/DdcController.cpp`, line 362) reports one state for all displays. Derive the built-in screen's availability from the laptop panel only (the display-on state and the lid, through `ILaptopEnvironment`), never from the DDC state.
- **The power scheme can change while protected.** Restore the saved values. Re-apply the saved scheme only if it is still the active one, as `AdaptiveBrightness::restore` does. Read the setting before writing and skip the restore when it reads on, so a user's change during the hold is never overwritten.
- **The built-in screen can be absent in three ways.** `WmiMonitorBrightness` may be missing (`ApiUnavailable`), the panel may not be adjustable (`BrightnessNotAdjustable`), or there may be no internal panel (`NoBuiltInDisplay`). In each case `protectable` is false and the screen stays out of both lists.
- **Internal panel.** `isInternalPanel` (`native/windows/Platform/DisplayCatalog.cpp`, line 284) keeps the panel out of DDC discovery (`DdcController.cpp`, lines 198, 250, 302). Keep it out of the core `monitors` map, as on macOS. `detectLaptop` does not query `WmiMonitorBrightness` directly; it reads `panelBrightnessReadable()` through `ILaptopEnvironment` (`native/windows/Platform/ILaptopEnvironment.h`, line 36).
- **WMI snapping.** A write lands on the nearest supported level, so the macOS 1-point tolerance alone is not enough. Accept the nearest supported level (T-W4).
- **Naming.** "Laptop" (en-US) and "Notebook" (pt-BR) apply to the Windows UI text only (`adr/0023-windows-laptop-naming-and-notebook.md`). Core identifiers stay (`builtin`, `builtin_display`).
- **Two meanings of "automatic brightness".** The Windows setting is the OS feature. The app's "Automatic brightness" page (`AutomaticBrightnessPage.xaml.cpp`) is the follow feature. Every new text must say which one it means.
- **Hardware keys are not involved.** Keep them unchanged, and do not route brightness keys to the built-in panel (spec FR-005).
- **Do not copy** `docs/user-guide.md` lines 84-104 (macOS behavior) into Windows text.

## 5. Core facts the Windows shell can rely on

| Fact | Where | Note |
|---|---|---|
| `protected` = `protectable` and `"builtin"` not in `protection_disabled` | `snapshot_value` in `core.rs` | |
| `available` = last reported `available`; reasons are empty while unavailable | `snapshot_value`; `builtin::reasons` (`builtin.rs`, line 32) | |
| Freeze rule: while `holding` or any reason is active, the reported `brightness` does not update `builtin_percent` | `builtin_display` in `core.rs`; `builtin_frozen` | The shell still reports the level; the core ignores it |
| Presence: after 2 consecutive absent samples while protected, target is the protection level (1..50, default 1) | `absent` handling in `core.rs` (misses >= 2); `builtin::target_percent` (`builtin.rs`, line 55) | Needs the camera to run |
| Screensaver: target is the same protection level, `screensaver_targets["builtin"]` clamped to 1..50 (default 1), only while screensaver protection is on | `builtin_target_percent` in `core.rs` | Presence and screensaver never stack |
| Target is the protection level while any reason is active, or null when there are none | `builtin::target_percent`, `builtin.rs`, line 55 | |
| Reasons are empty while calibrating | `builtin::reasons` (`calibrating` argument) | |
| Follow pauses while frozen | `follow_target` in `core.rs` | |
| Camera gate: the gate display (first awake candidate) is above its own threshold, from `presence_min_brightness_by_display` or else `presence_min_brightness_percent`; the camera also runs while any display is dimmed by presence | `monitored_below_threshold` and `camera_gate_active` in `core.rs` | A laptop alone runs the camera. Contract test `laptop_alone_runs_the_camera_and_gates_on_the_threshold` |
| `hold_auto_brightness` = option and reasons non-empty | `snapshot_value` in `core.rs` | The shell acquires the owner only when this is true |
| Shutdown: presence cleared, `closed` set, reasons empty | `shutdown_policy` in `core.rs` | |
| Diagnostics: `builtin_protection reasons=... target=...` on each change | `sync_builtin_diagnostics` in `core.rs` | |
| Legacy `builtin_display` without the new keys means `protectable` false and `holding` false | Contract test `legacy_builtin_display_without_the_new_keys_leaves_protectable_and_holding_false` | |
| `holding` is ignored unless `protectable` is true | `builtin_display` in `core.rs`; contract test `holding_without_protectable_is_ignored_and_never_freezes_follow` | |
| Brightness and volume values set from a slider or key are stored as whole raw units (`round()`) | `manual_set` in `core.rs` (about line 594) | Shared-core effect; it already reaches Windows. Windows slider and key values reach `manual_set` unrounded: the manual queue sends the normalized percent (`controls/manual.rs`, `command_json`). The max VCP value of the owner's monitors is unverified. |
| A monitor dimmed by protection gets its baseline back when it returns under the same id at the dimmed level | `park_dim` and `discover` in `core.rs` (about lines 312-463) | Shared-core effect; it already reaches Windows through `monitor_disconnected` and `monitor_discovered`. Applies only at the same id and level, with stable identity and supported brightness. Runtime only: cleared at shutdown. Not verified on Windows hardware. |

Shared-core effects that already reach the Windows shell, because it runs the same core. No shell change is needed for these:
- Presence absence dims a protected monitor to the protection level (1..50, default 1) instead of 0. Stored `screensaver_targets` values from 0 to 100 are accepted and clamped when applied.
- `presence_min_brightness_percent` accepts 0 to 90. The Windows threshold box still uses its own minimum of 10 (`kMinBrightnessMin` in `GeneralPage.xaml.cpp`) until section 7.2 is done.
- The Windows protection slider still runs 0 to 100 (`MonitorCard.xaml`). The core clamps the stored value to 1..50 when it is applied (section 7.3).
- `presence_min_brightness_by_display` is not sent by Windows. `update_settings` changes only the keys it receives, so the core keeps the stored map.

## 6. Unverified items and corrections

Corrections to the plan and architecture notes, found while reading the code:
- The plan names `buildBuiltInDisplay`. The real name is `buildBuiltinDisplay` (`JsonCodec.cpp`, around line 671).
- The plan puts the internal-panel check in `Platform/LaptopDetection.cpp`. `isInternalPanel` is set in `DisplayCatalog.cpp` (line 284). `detectLaptop` takes an `ILaptopEnvironment` (`LaptopDetection.cpp`, line 43).
- The plan names `AppController::buildCard`. It is a free function in `AppController.cpp` (line 387).
- `AdaptiveBrightness::restore` applies the saved scheme only when it is still active, but it writes the saved AC and DC values in all cases. It does not check whether the setting is on. The owner adds that fresh read (architecture, Windows Handoff item 6).
- The plan (T033) and the architecture define Windows rows L4-L6. The brief asked for L4-L7. The hardware checklist has no Windows rows yet.
- The `FakePower` classes are local to three test files (`adaptive_brightness.cpp`, `ambient_calibration.cpp`, `calibration_session.cpp`). `native/windows/Tests/Fakes.h` has only `FakeClock` and `FakeScheduler`.
- The `ILaptopPanel` comment (`ILaptopPanel.h`, line 16) says `setPercent` writes the nearest supported level. That is true for WMI, but the level is not reported back, so the verify step needs the supported-levels accessor (T-W4).

Not confirmed (not checked, or not verified on Windows):
- Whether `parseSnapshot` tolerates unknown keys line by line. [contracts.md](contracts.md) says the codec reads named keys only.
- Whether `Resources.resw` already has the generic labels (brightness, protection brightness, protect this monitor).
- The pt-BR name of the Windows automatic-brightness setting.
- The exact thread that calls `Services::publish` (`AppController.cpp`, line 623). A comment near line 560 suggests the bridge worker thread.
- Whether `CalibrationSession.cpp` writes its record with a temp file and a replace. The atomic write in T-W4 should follow the same helper; confirm which one it uses.
- Line numbers are from the review of this branch and must be rechecked after any edit.

All file paths in this document were checked to exist (new files are marked as new). Symbols were checked by reading or by search, except where listed above.

## 7. Follow-ups from macOS hardware testing (to execute later, on Windows 11)

Source: the owner's macOS hardware testing, 2026-10-20. Requirements: [spec.md](spec.md) FR-019, FR-020, FR-021. Design: [architecture.md](architecture.md) Decisions 3, 4, 7 and 14. Contracts: [contracts.md](contracts.md) (`monitored_display_*`, `builtin_protection`). Plan: [plan.md](plan.md) Phase 8, T040 to T044 (T042 is this section).

Status: nothing in this section is implemented, compiled or run on Windows. The parts already in the shared core are 7.6(b) and the park/resume and manual-rounding rules in section 5. The Windows files and symbols below were checked by reading this branch. Line numbers are approximate. Items that were not checked are listed under "Unverified" at the end of this section. Run the items in the order of 7.7.

Rules for 7.1 to 7.4:
- The Rust core already holds the shared policy for each item, except where an item says otherwise. Windows adds shell code and JSON parsing only. No C ABI change.
- Windows sends the whole settings object (`buildSettingsPayload`), and the core applies the keys it receives. Keys that are read-only or sent by their own command must stay out of that payload.

### 7.1 Monitored display selector (FR-021, Decision 14)

What the core already provides (checked in `crates/telaviva-core`):
- Settings `monitored_display_id` (writable; `null` is Automatic and clears the history) and `monitored_display_history` (read-only; `update_settings` rejects it). Maximum 8 entries; the front entry is the selection (`src/settings.rs` lines 52-62 and ~331-349; `src/core.rs` ~624-641).
- The resolver `resolve_monitored` (`src/core.rs` ~1609-1633). History first: a monitor needs a stable identity and a supported brightness; `"builtin"` needs `protectable` and `available`. Then Automatic: the `primary` monitor, the first usable external monitor by id, then the built-in screen.
- `monitor_discovered` accepts an optional `primary` boolean (`src/core.rs` ~309-327).
- Snapshot `monitored_display {id, automatic}` (`src/core.rs` ~1688-1696).
- Camera gating uses the resolved display (`src/core.rs`). `camera_gate_active` and `monitored_below_threshold` use `gate_display`, the first awake entry of the same candidate order, so a sleeping monitored display falls through to the next one. Once the Windows picker sends `monitored_display_id`, it changes the camera through the core.
- The macOS picker exists: `native/macos/TelaViva/SettingsView.swift` (`monitoredDisplaySelection`, the Picker at about line 160). It is listed only while the built-in screen is protectable and available. Windows has no template for the picker, so copy the camera picker in `GeneralPage` instead.

Windows tasks:
1. Settings model. `native/windows/Platform/SettingsModel.h` (declaration beside `buildSelectCameraPayload`, line 48) and `SettingsModel.cpp`: add `buildSelectMonitoredDisplayPayload(const std::optional<std::string>&)`, modeled on `buildSelectCameraPayload` (lines 267-272). It sends only `monitored_display_id`. Do not add `monitored_display_id` or `monitored_display_history` to `buildSettingsPayload` (lines 217-265).
2. Snapshot parse. `native/windows/Platform/JsonCodec.h`: `SettingsSnapshot` (lines ~40-61) gets `monitored_display_id` (optional string), and a new struct holds the snapshot object `monitored_display`. `JsonCodec.cpp` `parseSnapshot` (line 857; the camera read is at line 872) reads both. The history is not needed for the picker.
3. The `primary` flag. Source: `MONITORINFOF_PRIMARY` in the `MONITORINFOEXW` that `collectMonitor` already reads (`native/windows/Platform/DisplayCatalog.cpp` lines 134-148). Carry it through `MonitorHandle` (lines 129-132), a new `primary` field in `CatalogEntry` (`DisplayCatalog.h` lines 14-23), `gatherEntries` (lines 241-290; it matches the handle through `findMonitor`, line 158), `MonitorInfo` (`JsonCodec.h` lines 16-29), and the discovery loop in `DdcController.cpp` (about lines 197-236; `buildMonitorDiscovered(info)` is submitted at line 236). `buildMonitorDiscovered` (`JsonCodec.cpp` lines 596-609) writes the flag.
4. The Laptop (Notebook) entry. Show it only when `builtin_protection.protectable && builtin_protection.available` (section 2). It needs section 3 T-W1 and T-W3, which set the `protectable` flag. Until then it never appears.
5. UI. `native/windows/TelaViva/GeneralPage.xaml.cpp`: a ComboBox beside `cameraBox_` in the availability block (about lines 466-512; its submit is about line 483). Items: index 0 "Automatic" (sends null), then the usable monitors (stable identity and supported brightness, the same test as the core), then the built-in entry under the visibility rule. Rebuild the items from the snapshot with `suppress_` set, as the camera box does. macOS lists the monitored-display picker directly below the presence threshold slider (`native/macos/TelaViva/SettingsView.swift`, line 160; the slider is at line 149). See 7.8.
6. Strings. `native/windows/TelaViva/Strings/en-US/Resources.resw` and `pt-BR/Resources.resw`: keys for the label, the help text, the Automatic item and the built-in name, "Laptop" (en-US) and "Notebook" (pt-BR), per `adr/0023-windows-laptop-naming-and-notebook.md`. Follow the pattern of `camera` (en-US line 214), `cameraPicker` (217) and `cameraHelp` (220). The word "Laptop" already appears in the values of the `ambient.*` keys (lines 341-359).
7. Help text. `presenceMinBrightnessHelp` (en-US lines 175-176) says "every monitor". Proposed en-US: "The camera stays off while the monitored display is at or below this brightness." The pt-BR value says "todos os monitores" and needs native review.

Tests (`native/windows/Tests`, fakes only):
- `settings_payload.cpp` `settings_payload_has_exactly_the_core_keys` (lines 183-200): add absence checks for `monitored_display_id` and `monitored_display_history`, beside the existing `selected_camera_id` check (line 197). The key count does not change for this item.
- `settings_payload.cpp`: tests for `buildSelectMonitoredDisplayPayload`, modeled on the camera tests (lines 387-408): escaping, null, the length limit, and `"builtin"` (depends on `validId`, see Unverified).
- `json_codec.cpp`: `buildMonitorDiscovered` writes `primary` as true and as false (near lines 115-140); `parseSnapshot` reads `monitored_display` and `monitored_display_id`.
- `display_catalog.cpp`: `primary` passes through the fake `IDisplayCatalog` path. The Win32 flag read is not unit-testable; W-L8 covers it.
- New `native/windows/Platform/MonitoredDisplayRules.h` (proposed): the item order, the usable filter and the visibility rule as pure functions, tested in `native/windows/Tests/monitored_display_rules.cpp` (picked up by the `*.cpp` wildcard).
- `strings_keys.cpp`: parity, no "Mac" or "MacBook", and no "Ajustes" in pt-BR, for every new key.

Decision needed (not decided here): `primary` is sent only inside `monitor_discovered`. If the user changes the Windows primary display while the app runs, the core keeps the old flag until the monitor is discovered again. Options: resend on a display change, or accept the change at the next launch.

Checklist row: W-L8 (added to `validation/hardware-checklist.md`, Not run).

### 7.2 Presence threshold 0..90 percent

What the core already provides: `presence_min_brightness_percent` accepts 0.0 to 90.0 (`src/settings.rs` ~369-371). The gate (`monitored_below_threshold` in `src/core.rs`) compares the gate display's level with `level <= threshold`, using that display's own threshold (7.8) or this global value. At 0, the camera stops only when the gate display is at exactly 0%. The gate display is the monitored display, or the next awake candidate when it sleeps (see 7.1).

Windows tasks:
- `native/windows/Platform/SettingsModel.cpp`: `kMinBrightnessMin` (line 18) from 10.0 to 0.0. The clamp at lines 190-191 keeps its fallback of 20.
- `native/windows/TelaViva/GeneralPage.xaml.cpp`: `kMinBrightnessMin` (line 51) from 10.0 to 0.0. The threshold NumberBox `thresholdBox_` (line 394) takes its minimum from it.
- Help text: see 7.1 step 7.
- The monitor level at 0 must reach the core as 0 (WMI lowest level, DDC 0). Checked on hardware only, in W-L4 and W-L8.

Tests: `native/windows/Tests/settings_payload.cpp` `settings_payload_clamps_min_brightness_both_ends` (lines 257-269): the lower-bound expectation changes from "10" (line 262) to "0". Add a check that 0 stays 0.

Checklist: none new. Extend W-L4 with a threshold-0 step (proposed, not added).

### 7.3 Protection level 1..50 per screen (FR-019)

What the core already provides: `Settings::protection_percent` (`src/settings.rs` lines 285-291). A missing entry means 1. Stored values are clamped to 1..50 when applied. `screensaver_targets` still accepts 0..100 (`src/settings.rs` ~373-381), so old files load. The same level applies to presence and to screensaver (`reconcile_all` in `src/core.rs` for monitors; `builtin_target_percent` for the built-in screen).

Windows current state:
- `native/windows/Platform/ProtectionRules.h`: `protectionBrightnessCap` (lines ~43-53); the helpers after it (about lines 55-72: `protectionSliderEnabled`, `protectionSliderMaximum`, `clampProtectionBrightness`); `showProtectionBrightnessRow` (lines 32-36), which shows the row only when screensaver is on and the monitor is protected.
- Popup: `TelaViva/Host/AppController.cpp` line 408 (row visibility) and lines 415-416 (`card.protectionCap`, `protectionCapKnown`); `TelaViva/Host/FlyoutState.h` lines 33-35; `TelaViva/MonitorCard.xaml.cpp` `applyProtectionSlider` (about lines 226-242).
- General page: `brightnessCeiling` (`TelaViva/GeneralPage.xaml.cpp` lines 201-209); the target slider takes its maximum from the ceiling (about lines 652-660); the ceiling refresh is about lines 694-711. `buildMonitorRows` (about lines 622-690) has no `screensaver_protection_enabled` check, so its rows appear whether or not screensaver is on (unverified visually).

Windows tasks:
1. Remove the cap. Use fixed 1 and 50 in `ProtectionRules.h`. `clampProtectionBrightness` clamps to 1..50 and treats a non-finite value as 1 (proposed; the current code uses 0). Remove `protectionBrightnessCap` and its callers in `AppController.cpp`, `FlyoutState.h` and `MonitorCard.xaml.cpp`.
2. Visibility. The row shows when presence OR screensaver is on, and the monitor is protected. Pass `presence || screensaver` at `AppController.cpp` line 408. Apply the same rule to the General page target rows in `buildMonitorRows`.
3. Range. Both sliders run 1..50 (the General page `makeSlider`, line 127, takes the maximum). Proposed, owner decision: a monitor without DDC brightness keeps its slider disabled.
4. The save path stays as it is. `clampToCoreRanges` keeps 0..100 (`SettingsModel.cpp` lines 196-200); the core clamps to 1..50 when applied.

Tests: `native/windows/Tests/protection_rules.cpp`:
- `protectionRules_brightnessRowVisibilityMatrix` (lines 37-44): rewrite as the presence and screensaver matrix.
- The cap tests (lines 56-76): remove or rewrite.
- The clamp tests (lines 84-124): rewrite for 1..50. Below 1 goes to 1, above 50 goes to 50, a non-finite value goes to 1, and the stored value does not depend on the monitor's brightness.

Checklist: none new. Extend W-L4 with a 1..50 slider step (proposed, not added).

### 7.4 Lid closed: built-in camera not used (FR-020)

What the core provides: nothing for camera devices. Camera device choice stays in the shell. The macOS shell reports the built-in screen as unavailable while the lid is closed (`BuiltInDisplayBackend.status()`), so the core sees `builtin_available` false. The built-in screen then drops out of the monitored candidates (`monitored_candidates` in `src/core.rs`).

Windows current state:
- The lid signal exists: `native/windows/Platform/PowerEvents.cpp` (the GUID is at line 16; the lid branch is at lines 119-128), then `TelaViva/Host/AppController.cpp` lines 1216-1218 (`recordLidState`, `onLidChanged`), then `Platform/LaptopDetection.cpp` `Win32LaptopEnvironment::lidOpen` (line 135) and `recordLidState` (line 172). `ILaptopEnvironment.h` lines 27-28: nullopt counts as open. Nothing in the camera path reads the lid.
- Built-in detection: `Platform/MediaCaptureBackend.cpp` `isBuiltInDevice` (lines 124-135) checks EnclosureLocation Front or Back. A device without an EnclosureLocation is not built-in. `Platform/CameraSelection.cpp` line 105 sets `builtIn`.
- Choice: `Platform/CameraSelection.cpp` `chooseCamera` (lines 162-192) has no lid input. Both the history loop and the fallback (which prefers built-in) accept built-in cameras.
- Lifecycle: `Platform/CameraLifecycleDriver.cpp` `onSnapshot` (lines 144-155) sets `selectedCameraId_` (line 153). `onDevicesChanged` (lines 181-190) calls `chooseCamera(..., historyOf())` (line 190). `historyOf` (lines 353-359) returns the selected id only. `SettingsSnapshot` has no `camera_history`; `JsonCodec.cpp` line 872 is the only camera read. The driver has no lid code.

Windows tasks:
1. `Platform/CameraSelection.h` (declaration, line 69) and `.cpp`: add a `lidClosed` parameter, default false. With it true, skip built-in cameras in the history loop and in the fallback. If nothing else is left, `effective` is empty. The default keeps the existing tests compiling.
2. The picker (`selectableCameras`, `CameraSelection.cpp` lines 151-160): proposed, hide built-in cameras while the lid is closed. Owner decision.
3. `Platform/JsonCodec.h` `SettingsSnapshot`: add `camera_history` (read-only). `JsonCodec.cpp` `parseSnapshot` reads it beside line 872.
4. `Platform/CameraLifecycleDriver.h` and `.cpp`: `historyOf()` returns the full history. Add a lid input (for example `setLidClosed(bool)`) that re-runs the choice the way `onDevicesChanged` does (lines 181-190): stop the capture when the effective camera changes or becomes none, and restart when the lid opens.
5. `TelaViva/Host/AppController.cpp` lid callback (lines 1216-1218): forward the state to the driver. This adds a call on an existing event, not a new event source.
6. The remembered choice is never rewritten. The fallback is runtime only (`CameraSelection.h` lines 58-63). `buildSelectCameraPayload` is called from the picker (`GeneralPage.xaml.cpp`, about line 483); that is the only caller found, and an exhaustive search was not done.

Tests (fakes; `native/windows/Tests/FakeCamera.h`):
- `camera_selection.cpp`: with the lid closed, history `[built-in, USB]` gives USB; history `[built-in]` gives nothing; no history with built-in plus USB gives USB; only built-in gives nothing. The existing tests (lines 39-112) do not change.
- `camera_lifecycle_driver.cpp`: lid closed while the built-in is active stops the session and reports the camera unavailable. Lid open restarts on the remembered choice. No selection payload is sent (checked by the fake). Whether the harness can capture outgoing payloads is unverified.
- `json_codec.cpp`: `camera_history` parses, and `selected_camera_id` still parses.

Checklist row: W-L9 (added to `validation/hardware-checklist.md`, Not run).

### 7.5 Calibration crash recovery (PARTIAL finding)

What the core provides: nothing. This is shell state.

Windows current state:
- Record: `SavedAdaptiveState` (`Platform/AdaptiveBrightness.h` lines 13-18) holds only the scheme, AC and DC values. `Platform/CalibrationSession.cpp` `parseFile` (lines 74-100) requires schema 1, the adaptive fields and `started_at`. Neither the writer nor the parser has `monitor_id`, a monitor percent or a panel level. The writer (`save`, about lines 193-227) uses a temp file and a rename. This settles the open question in section 6.
- Drift: `specs/archived/windows-version/contracts.md` lines 189-205 document `monitor_id` and `turned_off`. The code neither writes nor reads them.
- Recovery: `TelaViva/Host/AppController.cpp` line 1274 calls `recoverPending()` before the bridge sink is added (line 1279). `Platform/AmbientCalibrationController.cpp` `recoverPending` (lines 270-277) calls `session_.recover(adaptive_)`, which restores power values only. The monitor is never restored. No snapshot has been published at that point (the sink is added last), so the monitor is not in `latestSnapshot`.
- Live path: `restoreState` (lines 344-362) writes the monitor's original level (lines 353-355) and does not check the result. `MonitorsAdapter::setPercent` (`AppController.cpp` lines 293-297) returns nothing and drops the result of `ControlsDriver::slider` (`Platform/ControlsDriver.h` line 48, `std::optional<DraftView>`). Then `session_.end()` (line 358) deletes the record when the adaptive restore succeeded, even if the monitor write failed.
- Give-up: a failing adaptive restore keeps the record, and `CalibrationSession::begin` refuses while a record is pending (`Platform/CalibrationSession.h` lines 18-19). A permanent failure blocks every new calibration. There is no give-up.
- Tests that lock the gap: `native/windows/Tests/ambient_calibration.cpp` `recovery_with_disconnected_monitor_skips_monitor` (lines 920-930) asserts only the power writes. `Tests/calibration_session.cpp` `calibration_session_missing_field_is_treated_as_corrupt` (lines 269-275) requires every field.
- Doc drift: `specs/archived/windows-version/adr-notes.md` line 263; `plan.md` line 326; `validation/ux-parity.md` line 54 (A10, "Pass"); `validation/hardware-checklist.md` line 70 (L2, "crash recovery", Not run); and `contracts.md` above.

Proposed fix (mirrors macOS `native/macos/TelaViva/AmbientCalibrationController.swift`: `pendingMonitorRecovery` and the 90 s `monitorRecoveryTimeout`, lines 131-135; cleared in `begin`, line 183):
1. Record: add optional `monitor_id`, `monitor_percent` and `panel_level`. Schema stays 1. Parsing still requires the adaptive fields, so old files load with no monitor restore.
2. Launch: restore the panel level and the adaptive values at once. If the record names a monitor that is not in `latestSnapshot`, keep the monitor fields and retry on every `publish()` (`AppController.cpp` line 623). Give up after 90 s with `IScheduler::scheduleOnce` (`Platform/Scheduler.h` line 19).
3. After the adaptive restore succeeds, rewrite the record without the adaptive fields. This is required: otherwise a later launch would restore the old AC and DC values over a later user change.
4. `begin` drops any pending monitor restore.
5. `ICalibrationMonitors::setPercent` (`Platform/AmbientCalibrationController.h` line 41) returns bool, and `MonitorsAdapter::setPercent` maps the result to it. `restoreState` keeps the record when the monitor write failed.
6. Adaptive give-up (owner decision): after 3 attempts one second apart (as the protection restore, section 3 T-W4), either delete the record and show a notice, or keep blocking. Not decided here.

Tests (`native/windows/Tests/ambient_calibration.cpp`; `FakeMonitors` at line 264; `FakeScheduler` in `Tests/Fakes.h` line 37):
- Rewrite `recovery_with_disconnected_monitor_skips_monitor` (lines 920-930): the panel and power restore run, the monitor part is kept, and the monitor is restored on the first publish after it appears.
- With no monitor after 90 s: give up, delete the record and show the notice.
- `begin` on a pending record drops the pending monitor restore.
- A live `restoreState` with a failed monitor write keeps the record.
- `Tests/calibration_session.cpp`: an old-format record (no monitor fields) loads, and the optional fields parse. `calibration_session_missing_field_is_treated_as_corrupt` (lines 269-275) does not change.

Doc fixes after the code change: the five drift locations listed above.

Checklist: none new. Extend W-L6 with an external monitor step (proposed, not added).

### 7.6 Other Windows facts

(a) `AdaptiveBrightness::restore` (`Platform/AdaptiveBrightness.cpp` lines 144-161) writes the saved AC and DC values to the saved scheme in every case, and applies the scheme only when it is still the active one. It does not read whether the setting is still off. Proposed decision: the protection owner (section 3 T-W5) restores unless a fresh read says the setting is on, so a user change during the hold is never overwritten. Calibration keeps its current restore (the tests in `Tests/adaptive_brightness.cpp` lines 106-257 do not change). Calibration has the same exposure; on macOS the shared owner gives calibration the same rule, so the Windows owner should do the same. Test in `Tests/auto_brightness_owner.cpp`: a user change during the hold produces no write.

(b) `holding` (core hardening, plan T042, a shared Rust change): done. `builtin_display` in `src/core.rs` stores `holding` as `protectable && holding`, so the core ignores `holding` unless `protectable` is true, and the freeze check reads that value. Test: `holding_without_protectable_is_ignored_and_never_freezes_follow` in `crates/telaviva-core/tests/builtin_protection_contract.rs`. Windows never sends `holding` today, so this changes no Windows behavior.

(c) Save before the first snapshot: `latestSettings` is default constructed (`TelaViva/Host/AppController.cpp` line 537) and set only in `publish` (line 628). `currentSettings()` returns it (line 1464). Pages build their draft from it with `draftFromSnapshotSettings`: `GeneralPage.xaml.cpp` (about lines 324 and 344), `KeyboardPage.xaml.cpp` (171), `AutomaticBrightnessPage.xaml.cpp` (320), `AdvancedPage.xaml.cpp` (165). A save before the first publish would send `protection_disabled` as an empty set, which clears every opt-out, including `"builtin"`. Whether a page can save before the first publish is unverified. Proposed guard: no settings save until the first snapshot. Test: a draft built from the default `SettingsSnapshot` is not sent.

(d) Wake rediscovery read. After wake, `DdcController` reads a monitor's brightness only when the monitor is listed as new (`native/windows/Platform/DdcController.cpp` lines ~197-223). Each read uses `DdcTransaction` with 3 attempts in total and delays of 200 ms and 1000 ms (`native/windows/Platform/DdcTimingProfile.h` line 14). If that read falls outside the ADR 0033 window (`adr/0033-wait-for-ddc-readiness-after-wake.md`), the monitor is discovered with brightness unsupported and stays unsupported until it is unplugged and replugged, because `DdcController` re-reads only newly listed entries. Proposed: add a re-read path for a known monitor that was discovered with brightness unsupported. Not implemented.

(e) No in-place rediscovery on Windows. A monitor that is listed again is preceded by `monitor_disconnected` for its id. `diffCatalogs` (`native/windows/Platform/DisplayCatalog.cpp` lines ~294-340) reports a removal when the id and identity-stability pair is no longer listed, and `DdcController.cpp` sends the disconnect (lines ~174-192). So the core's in-place path, a `monitor_discovered` for an id it already holds, is not used on Windows. A short grace before `monitor_disconnected` for a monitor that is only temporarily absent would keep the Monitor in place. It would change the test `ddc_controller_removed_monitor_sends_disconnected` (`native/windows/Tests/ddc_controller.cpp` lines ~329-338). Not decided.

(f) Duplicate-EDID flip (stable, ambiguous, stable). Before this fix, `discover` consumed a parked dim on every rediscovery, so an ambiguous or unsupported return lost the entry, and the later stable return was not restored. The core now consumes the entry only for a stable, supported rediscovery at the dimmed level. An ambiguous or unsupported return leaves it parked, so a later stable, supported return at the dimmed level restores the previous baseline (`core.rs` `discover`, lines ~364-374; tests `dim_survives_an_ambiguous_rediscovery_and_is_restored_by_a_later_stable_one` and `dim_survives_an_unsupported_rediscovery_and_is_restored_by_a_later_supported_one` in `crates/telaviva-core/tests/wake_rediscovery_contract.rs`). This is resolved in the shared core and needs no Windows change. It still needs a hardware check on Windows: a proposed row D9 in `validation/hardware-checklist.md` (not added here; that file has no D9 row).

### 7.7 Execution order (Windows 11)

1. 7.2 (range; small and unit-tested).
2. 7.3 (level; it touches the same General page rows as 7.1, so it goes first).
3. 7.5 (calibration record and restore; it overlaps with section 3 T-W5, so either the owner refactor starts from the 7.5 result or both land together).
4. 7.1 (picker; the built-in entry needs section 3 T-W1 and T-W3, and the camera effect comes from the core's gate display).
5. 7.8 (presence threshold per display; it needs the picker from 7.1 for the monitored display, and the range from 7.2).
6. 7.4 (lid camera; independent of the others).
7. Wake rediscovery parity (7.6 (d), (e) and (f)), before the section 3 tasks.
8. Then section 3 (T-W1 to T-W9) and the hardware rows, with W-L8 and W-L9 added.

Notes: 7.6(a) belongs to T-W5. 7.6(b) is already done in the core (T042). 7.6(c) should land before any settings UI change in 7.1, 7.3 or 7.8.

Checklist rows (`validation/hardware-checklist.md`, Windows section): W-L8 (monitored display, FR-021) and W-L9 (lid-closed camera, FR-020), both Not run. The Windows summary row now reads W-L4 to W-L9. Proposed extensions, not added: W-L4 (threshold 0 and the 1..50 slider) and W-L6 (external monitor restore).

### 7.8 Presence threshold saved per display (FR-022, Decision 15)

What the core provides (Decision 15):
- Setting `presence_min_brightness_by_display`: a map from display id (a monitor id or `"builtin"`) to a percent in 0..90, stored like `screensaver_targets` (`src/settings.rs`, field at line 41; lookup `presence_threshold`, line 301). The camera gate uses the entry of the gate display (7.1). A display without an entry uses `presence_min_brightness_percent`, which stays the default and the fallback.
- Entries for disconnected displays are kept, so a returning environment gets its threshold back.
- Windows never sends this key today. The core merges settings, so Windows is unaffected until this item is implemented.

Windows tasks:
1. General page. The presence threshold slider (`thresholdBox_`, `native/windows/TelaViva/GeneralPage.xaml.cpp`) edits the entry of the monitored display (7.1), or the global value when no display resolves. Range 0..90 (the minimum is set in 7.2).
2. Save. Send the whole map as `update_settings`, replacing the stored map, the same way `screensaver_targets` is carried. `draftFromSnapshotSettings` copies it (`native/windows/Platform/SettingsModel.cpp`, line 175), and `buildSettingsPayload` (line 217) writes it. `screensaver_targets` is written at lines 223 and 260.
3. Clamp and tests. `clampToCoreRanges` (line 181) clamps each entry to 0..90, as the core does. Update `native/windows/Tests/settings_payload.cpp` to match: the map is written whole, entries clamp to 0..90, and `settings_payload_has_exactly_the_core_keys` counts the new key.
4. Disconnected displays. A snapshot entry for a display that is not connected is copied into the payload, not dropped.
5. Strings. The built-in entry is "Laptop" (en-US) and "Notebook" (pt-BR), per `adr/0023-windows-laptop-naming-and-notebook.md`.
6. Template. The macOS template is `native/macos/TelaViva/SettingsView.swift` (`presenceMinBrightnessSelection` and `writePresenceMinBrightness`). It shows and edits the monitored display's own value.

### Unverified (not checked, or not confirmed)
1. `validId` accepts `"builtin"` (`Platform/SettingsModel.cpp` line 55; the function body was not read).
2. What a `nullopt` from `ControlsDriver::slider` means: refused, or no draft (`Platform/ControlsDriver.cpp` lines 65-68).
3. Where `CameraLifecycleDriver` is built and owned (`AppController.cpp` was not searched). Whether the lid state is read at launch (the body of `LaptopDetection.cpp` `lidOpen` was not read). If it is not read at launch, a launch with the lid closed uses the built-in camera until the next lid event.
4. The camera status key for "unavailable" (`Platform/CameraStatus.cpp` was not read).
5. Whether built-in cameras report EnclosureLocation Front or Back on the owner's laptop. A built-in camera without it would be missed by the lid rule. Hardware check.
6. Whether the General page target rows appear with screensaver off (grep only; no visual check).
7. Whether any page can save before the first publish (7.6c).
8. `MONITORINFOF_PRIMARY` on a multi-monitor setup. Hardware check.
9. Answered in the core (`src/core.rs` `discover` and `park_dim`, read for this update): a repeated `monitor_discovered` for the same id replaces the Monitor. A dim the core holds is parked first. A stable, supported return at the dimmed level resumes it; a stable, supported return at another level drops it and keeps that level; an ambiguous or unsupported return leaves it parked (7.6 (f)). Not verified on Windows hardware.
10. pt-BR wording for "Monitored display", "Notebook" and the help text, and the pt-BR name of the Windows automatic brightness setting (section 6).
11. `crates/telaviva-core/tests/monitored_display_contract.rs` appeared in the working tree during this task; it was not in the first listing. Its contents were not read.
12. Whether `native/windows/Tests/FakeCamera.h` can capture outgoing payloads.
13. Whether the `FakeScheduler` in `native/windows/Tests/Fakes.h` (line 37) suits the 90 s give-up test (only its declaration was read).
