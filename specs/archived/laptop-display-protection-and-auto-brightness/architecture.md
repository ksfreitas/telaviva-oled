# Architecture: Laptop display in brightness controls and protection, with automatic brightness off during protection

**Feature**: laptop-display-protection-and-auto-brightness **Date**: 2026-10-20 **Branch**: `laptop-display-protection-and-auto-brightness` **Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary

The shared Rust core gains a small built-in-display protection policy (`builtin.rs`) that decides when the laptop's built-in screen must be dimmed and to what percent, and exposes that decision in the snapshot as a level-triggered `builtin_protection` object. The macOS shell applies it through a new `BuiltInProtectionController` (baseline capture, dim, restore, recovery record) and a new `BuiltInAutoBrightnessOwner` that calibration and protection share. The built-in screen is NOT added to the core's `monitors` map. Windows is designed here but not implemented; the core stays inert for Windows because the built-in policy is opt-in per shell.

## Technical Context

**Language/Stack**: Rust core (`crates/telaviva-core`), Swift/SwiftUI/AppKit shell (`native/macos/TelaViva`). Windows (C++/WinRT) is documented only.
**Key Dependencies**: none new. Private DisplayServices and CoreBrightness already accepted by ADR 0007.
**Storage**: one new boolean in `settings.json` (schema stays 1); two small JSON recovery records in `~/Library/Application Support/TelaVivaOLED/` (shell-owned).
**Testing**: Rust contract tests (`cargo test -p telaviva-core`), macOS shell tests through `tools/tests/test-*.sh` (hand-listed swiftc sources, fakes), macOS hardware checklist rows. Windows: task list only.
**Platform**: macOS Apple Silicon implemented; Windows 11 x64 deferred by the owner (spec Decisions Q7).

## Technical Decisions

### Decision 1: The core decides, the shell applies, and the built-in is not a core "monitor"
**What**: A new `BuiltInProtection` state in the core computes `reasons` (presence, screensaver), `target_percent`, and `hold_auto_brightness` for the reserved id `"builtin"`. The snapshot publishes them in a new `builtin_protection` object. The shell reconciles toward that state (level-triggered): reads a fresh baseline, dims, and restores.
**Why**: Research showed that putting a pseudo monitor in `monitors` couples it to machinery built for DDC: the controls handle maps every brightness action to VCP 0x10 and only accepts four error names (`none|unsupported|io|disconnected`, any other name faults the controls bridge for good); readback needs an exact integer match; every attempt pays a 60 ms settle, a 2 s wake grace and a global DDC exec slot; sticky `owned` plus read-before-write would treat macOS automatic-brightness drift as a manual change and skip the first dim (`manual_override`); and a monitor entry silently joins follow eligibility, camera gating and the Windows protection/calibration lists. A separate small policy avoids all of that and keeps policy in the core (ADR 0008).
**Alternatives Considered**: (a) Pseudo monitor with a `builtin` flag and a second attempt backend: rejected for the coupling above (about ten special cases inside `reconcile_all`, camera gating and follow). (b) Shell-only policy (shell watches camera presence and screensaver itself): rejected, it duplicates the confirmation rules and violates ADR 0008.
**Trade-offs**: The built-in restore logic (retry, wake) is written once more in the shell instead of reusing the core's DDC restore. It is small because the built-in write is instant and has no ownership/readback ambiguity.

### Decision 2: Reserved id `"builtin"` reuses existing per-display settings
**What**: Per-screen protection opt-out is `protection_disabled` containing `"builtin"`; the per-screen protection level is `screensaver_targets["builtin"]`, read through `Settings::protection_percent` (fixed 1..50 percent, default 1 when there is no entry). Presence and the screensaver both dim the screen to that same level, never above the baseline (the shell writes `min(baseline, target)`). "Strongest reduction wins" is preserved.
**Why**: Satisfies "configurable per screen" with no new settings schema; the core never validates these keys against connected monitors, so a reserved id works; api.md already says entries for disconnected monitors are kept.
**Alternatives Considered**: Dedicated `builtin_protection_enabled` and `builtin_protection_percent` settings: rejected, duplicates the model and the UI code paths.
**Trade-offs**: The id `"builtin"` is reserved; real monitor ids are `vendor-product-serial-...` and cannot collide.

### Decision 3: Shell opt-in through `builtin_display` (`protectable`, `holding`)
**What**: The existing `builtin_display` command gains two optional booleans, default false. `protectable` is "a built-in screen exists and its brightness is adjustable" (true also while the lid is closed, so display-sleep logic still sees an eligible-but-asleep screen). `holding` is "the shell currently holds the screen dimmed and has not finished restoring".
**Why**: The Windows shell also sends `builtin_display`. Without opt-in the core would start deciding and gating the camera for a screen nobody dims on Windows. With opt-in, Windows behavior is byte-for-byte unchanged (spec FR-015, SC-007).
**Alternatives Considered**: A new command (`builtin_protection_capability`): rejected, more surface for the same data and an extra ordering problem.
**Trade-offs**: `holding` makes the shell's state part of the core's input, but it is the only way to freeze follow until the restore is verified. UI visibility is a separate rule: the card and the Settings row are listed only while `builtin_protection.available` is true, so the built-in screen disappears from the lists with the lid closed even though `protectable` stays true for camera and display-sleep logic.

### Decision 4: Freeze rule keeps follow and camera gating correct
**What**: While `holding || reasons non-empty`, the core ignores the reported `brightness` of `builtin_display` for `builtin_percent` (it keeps the last undimmed value). Follow therefore keeps mapping the undimmed level and its targets do not move. When `holding` returns to false and no reason remains, reports are accepted again with a fresh read. `follow_target` additionally returns None while frozen. The camera brightness gate uses `builtin_percent` (never the dimmed value).
**Why**: Fixes the case where externals are unprotected but the built-in is dimmed (externals must not follow the dimmed level) and keeps the camera from stopping because of the app's own dim, as `monitor_level_percent` already does for monitors.
**Alternatives Considered**: Let follow track the dimmed level: rejected, externals would dim. Core-owned baseline: rejected, see Decision 6.
**Trade-offs**: The popup slider shows the frozen value while protected; it is disabled in that state anyway.

### Decision 5: Camera, cadence and sleep logic include the built-in
**What**: When protectable and protected, the built-in counts as a camera-eligible display (not requiring a baseline): `camera_mode`, `monitored_below_threshold` (via `builtin_percent` against the gate display's own threshold; the global-only wording of the first design is superseded by Decisions 14 and 15), `absence_confirmation_pending`, `sync_brightness_gate`, `all_eligible_displays_asleep` (built-in asleep means `builtin_available == false`), `camera_display_status`, cadence/capture selection (`any presence dimmed`) and every site that clears presence dims/misses also clear the built-in's. A laptop with no external monitor therefore gets presence protection.
**Why**: The main laptop-only use case needs the camera to run for the built-in; otherwise `camera_mode` stays `suspended` with no monitors.
**Alternatives Considered**: Built-in dims only when another display already runs the camera: rejected, useless on a laptop alone.
**Trade-offs**: Slightly more code in camera gating; covered by contract tests that mirror the existing monitor tests.

### Decision 6: The shell owns the baseline and the recovery record
**What**: On the transition to "dim", `BuiltInProtectionController` reads the level fresh (`readLevel()`), stores it as `baseline`, writes `builtin-protection-session.json` first, then reports `holding=true`, then (option on) acquires the automatic-brightness owner, then writes `min(baseline, target)`. On restore it writes `baseline` exactly (unrounded), verifies by readback within 1 point, releases the owner, deletes the record, reports `holding=false`.
**Why**: A fresh read at dim time avoids stale baselines (no continuous polling needed for it), and a baseline in a file survives a crash, fixing the existing hazard where the next launch adopts the dimmed reading as baseline (spec FR-010).
**Alternatives Considered**: Core-owned baseline fed by polling: rejected, stale and not crash-safe.
**Trade-offs**: Policy in core, state in shell; documented in the ADR.

### Decision 7: One shared automatic-brightness owner
**What**: New `BuiltInAutoBrightnessOwner` with holders `{calibration, protection}`. First `acquire` reads the state; if it is on, it writes `auto-brightness-hold.json` (`original_enabled: true`) and turns it off; later acquires only add holders. The last `release` turns it on unless a fresh read says it is already on (a nil or off read still turns it on, because the app turned it off), then deletes the file. A failed write keeps the file and owes a restore that the next acquire joins. `recoverIfNeeded()` at launch restores from the file. Calibration is refactored to call `acquire(.calibration)` and `release(.calibration)` at the same positions as today, and `AmbientCalibrationController` keeps its own session record unchanged (its `auto_brightness_was_enabled` field stays for format compatibility). Crash recovery is not fully delegated to the owner's record: when the calibration session record says automatic brightness was on and this run holds nothing, calibration recovery still calls `setAutoBrightness(true)` directly (a legacy path).
**Why**: Spec FR-007/FR-008. Today calibration reads and writes the setting directly; two independent users would record "already off" and never restore.
**Alternatives Considered**: A refcount inside the backend: rejected, the backend is a stateless wrapper and its fakes are shared by tests.
**Trade-offs**: Calibration tests need a fixture change (an owner with a temp-dir store) but the sequence of backend calls stays identical, so the existing exact-order assertions hold.

### Decision 8: The option is a core setting, default off
**What**: New boolean `builtin_auto_brightness_off_on_protection` (default false, schema stays 1). The core publishes `hold_auto_brightness = option && reasons non-empty`; the shell acquires and releases the owner from that flag. With the option off protection never reads or writes automatic brightness (SC-006).
**Why**: Owner decision Q5 (option on the built-in screen, off by default). A core setting means one persistence path and Windows can read the same key later.
**Alternatives Considered**: A shell-only preference: rejected, settings live in the core.
**Trade-offs**: An older build reading a newer `settings.json` resets it (`deny_unknown_fields`); already accepted and documented (api.md, CHANGELOG 0.1.1).

### Decision 9: Calibration and protection never overlap
**What**: The core makes `reasons` empty while `calibrating` is true. `AmbientCalibrationController.begin` refuses (notice `calibration.notice.protectionActive`) while the protection controller is holding. The monitor's polling is already paused during calibration.
**Why**: Calibration captures the built-in level as its restore target; capturing a dimmed level would restore the dimmed level.
**Alternatives Considered**: Let calibration win silently and restore protection after: rejected, complex and rare (the user is present during calibration).
**Trade-offs**: A short window after the user returns where Calibrate is refused until the restore finishes.

### Decision 10: Manual built-in slider writes the backend directly
**What**: The popup card's brightness slider calls `backend.writeLevel` on the main queue (no core, no controls handle). It is disabled while the built-in is protected/holding/unavailable. Hardware keys are unchanged. The existing follow poll then reports the new level, so a followed monitor tracks the slider.
**Why**: The controls queue exists for DDC slowness, ownership and key policy; none apply. Follow already treats the built-in as its source.
**Alternatives Considered**: Route the slider through the controls handle: rejected, see Decision 1.
**Trade-offs**: The card slider reflects the polled level only while the poll runs; on popup open the card reads the level directly once.

### Decision 11: Polling and thread safety
**What**: `BuiltInBrightnessMonitor.setEnabled` becomes `follow || (protectable && protected)`. The monitor already reports availability when disabled (without a level), which is how `protectable` is derived. `DisplayServicesBackend` gets an internal lock around `readLevel`/`writeLevel` because the monitor now polls while the controller writes. The `BuiltInDisplayBackend` protocol is unchanged, so the two existing fakes keep compiling.
**Why**: Protection needs the level for the camera brightness gate. FR-018 (no new loop) holds: the existing 1 s poll is reused and only runs when protection or follow needs it.
**Alternatives Considered**: A slower idle poll (e.g. 5 s) when only protection needs it: deferred, measure first. Removing the double `status()` call per tick: noted as a follow-up optimization, not done here.
**Trade-offs**: The poll now also runs for protection-only users.

### Decision 12: Failure model
**What**: Automatic brightness unreadable (`nil`) or not changeable: log a diagnostic, show a notice (`autoBrightnessUnknown` / `autoBrightnessNotChanged`), dim anyway, release nothing that was not changed (spec FR-012). Baseline unreadable: skip the dim and log (nothing to restore to). Dim write fails or does not verify: log, retry after 1 s, and park after 3 attempts per episode. A parked dim writes nothing until a changed target, option or availability starts a new episode. A verified level for the same decision is trusted for 5 s, so the readback is not repeated on every snapshot. Restore failure: up to 3 attempts one second apart, then keep `holding` and the record and retry at the next availability change, snapshot or launch.
**Why**: Honest failure behavior for private, hardware-unverified setters; the record guarantees a later recovery.
**Alternatives Considered**: Abort the dim when auto brightness cannot be changed: rejected by owner decision Q6.
**Trade-offs**: A stuck `holding` after repeated failures leaves the screen dim until the next trigger.

### Decision 13: Windows is documented, not implemented
**What**: See "Windows Handoff". Shared core changes are additive and inert for Windows. No Windows source or strings are touched.
**Why**: Owner decision Q7 and the AGENTS.md scoping clause.
**Alternatives Considered**: Writing unbuilt Windows code: rejected by the owner.
**Trade-offs**: A later Windows change must also extend the Windows tests and hardware checklist.

### Decision 14: Monitored display selection (owner request, 2026-10-20)
**What**: Two settings mirror the webcam choice: `monitored_display_id` (selected, `null` means Automatic) and `monitored_display_history` (read-only, most recent first, at most 8, front entry is the selection; `null` clears it). The id is a monitor id or the reserved `"builtin"`. The core resolves the monitored display: the first history entry that is connected (a monitor present in the core with stable identity and supported brightness, or the built-in when protectable and available); otherwise Automatic: the external monitor flagged `primary` (new optional `monitor_discovered` boolean, set by macOS from `CGDisplayIsMain`), else the first external by id, else the built-in when protectable and available, else none. The camera gate uses the gate display: the first awake candidate in the same order, so a sleeping monitored display falls through to the next one. The camera runs while the gate display is above its threshold, or while any display is presence-dimmed so that the return check continues; it is suspended for display sleep only when every candidate sleeps and no display is dimmed. Absence counting and dimming still apply to every protected screen. The snapshot gains `monitored_display {id, automatic}`, where `id` is the resolved (monitored) display, and the shell shows a picker in Settings.
**Why**: With ambient follow the external monitor can sit near 0% while the Mac screen is bright, and the "any protected display above the threshold" rule kept the camera on; the user wants to choose which display represents "I am looking at it".
**Alternatives Considered**: a per-screen threshold; a per-screen "counts for the camera" flag; excluding the built-in always. Rejected by the owner in favor of the explicit, remembered selection.
**Trade-offs**: multi-monitor setups no longer keep the camera on because any display is bright; schema stays 1 (new optional fields, default empty), and older builds reading the file reset it as before.

### Decision 15: Presence threshold per display (owner request, 2026-10-21)
**What**: New setting `presence_min_brightness_by_display`: map from display id (a monitor id or `"builtin"`) to a percent in 0..=90, stored like `screensaver_targets` (keys 1..512 bytes, at most 128 entries, kept for disconnected displays so a returning environment restores them). The camera gate compares the gate display's level with `presence_min_brightness_by_display[id]`, or with the global `presence_min_brightness_percent` when that display has no entry (the global value stays valid 0..=90 and is the default and the fallback when no display resolves). `update_settings` replaces the whole map as for `screensaver_targets`. The Settings slider edits the monitored display's entry (or the global value when no display resolves).
**Why**: Each display has its own brightness behavior (a followed Dell sits near 0%, the MacBook screen much higher), and the user moves between work environments.
**Alternatives Considered**: a single global threshold (the old behavior); a per-environment profile object. Rejected as less direct than the existing per-display maps.
**Trade-offs**: another per-display map in the settings file (schema stays 1, optional field); Windows is unaffected because it never sends the key and the core merges settings.

### Addenda (owner requests during hardware testing, 2026-10-20/21)

- Monitored display selection and its picker in Settings: Decision 14.
- Presence threshold per display, 0..90 percent: Decision 15.
- Lid-closed camera rule (FR-020): while the lid is closed, built-in cameras are excluded from camera selection. Implemented in `LidStateMonitor` and `PresenceCamera.setLidClosed`. The remembered camera history is never rewritten.
- Protection level 1..50 percent (FR-019): presence and the screensaver dim to the same per-screen level, default 1 (see Decision 2).
- Calibration monitor recovery (deferred): a monitor restore left pending by a crash gives up after 90 s (`monitorRecoveryTimeout` in `AmbientCalibrationController.swift`).

## Architecture Overview

Before: protection (presence absence, screensaver) decides per DDC monitor in `reconcile_all` and the controls handle writes through DDC. The built-in screen is only a follow source (`builtin_display` reports level and availability). Automatic brightness is touched only by calibration.

After: the core also evaluates the built-in screen with the same rules (two-miss confirmation, screensaver, per-id opt-out, strongest reduction) and publishes `builtin_protection`. The shell applies it with the DisplayServices backend, holding the screen dimmed until the core says reasons are gone and the restore is verified. Automatic brightness is held through one owner shared with calibration. Follow and camera gating account for the frozen state.

### Component Structure

**New Files/Modules:**
- `crates/telaviva-core/src/builtin.rs` - `BuiltIn` state (`protectable`, `holding`, `misses`, `presence_dimmed`), constant `BUILTIN_ID`, pure helpers for reasons, target percent and hold flag, snapshot JSON.
- `crates/telaviva-core/tests/builtin_protection_contract.rs` - contract tests for the new policy (US-001..004, SC-001, SC-004, SC-006).
- `native/macos/TelaViva/BuiltInAutoBrightnessOwner.swift` - shared owner with holders, record store, recovery (US-003, US-006).
- `native/macos/TelaViva/BuiltInProtectionController.swift` - snapshot reconcile, baseline, dim, restore, retry, record, quit and launch recovery, notices model.
- `native/macos/TelaViva/BuiltInCardView.swift` - popup card (name, protection toggle, brightness slider, protection brightness slider; no volume).
- `native/macos/Tests/BuiltInAutoBrightnessOwnerTests.swift`, `native/macos/Tests/BuiltInProtectionTests.swift` - tests with fakes.
- `tools/tests/test-builtin-protection.sh` (and the owner test script) - hand-listed swiftc sources, same style as `test-ambient-calibration.sh`.
- `specs/archived/laptop-display-protection-and-auto-brightness/validation/hardware-checklist.md` - macOS rows (to run) and Windows rows (Not run, for the owner).
- `specs/archived/laptop-display-protection-and-auto-brightness/windows-handoff.md` - the Windows design and ordered task list (FR-016).

**Modified Files:**
- `crates/telaviva-core/src/core.rs` - `mod builtin`; `Core.builtin` field; `builtin_display` parses `protectable`/`holding` (allow-list at the command-key check) and applies the freeze rule; presence and clear sites; camera gating and cadence helpers; `follow_target` freeze; `shutdown_policy`; `snapshot_value` adds `builtin_protection`; `update_settings` arm and clear on opt-out.
- `crates/telaviva-core/src/settings.rs` - field `builtin_auto_brightness_off_on_protection` (`#[serde(default)]`), `Default`.
- `crates/telaviva-core/tests/core_contract.rs` - settings default/persist/old-file/wrong-type tests.
- `api.md` - `builtin_display` keys, `builtin_protection`, setting, reserved id (appended at finalize, drafted in contracts.md).
- `native/macos/TelaViva/CoreBridge.swift` - `CoreBuiltInProtection` snapshot struct, `submitBuiltInDisplay(protectable:holding:)`, `CoreSettings` new key (`CodingKeys`, `json`).
- `native/macos/TelaViva/BuiltInDisplayBackend.swift` - lock around DisplayServices read/write (no protocol change).
- `native/macos/TelaViva/AmbientCalibrationController.swift` - use the owner; refuse `begin` while protection holds.
- `native/macos/TelaViva/MenuBarController.swift` - wire owner and controller; compose `builtin_display` with `protectable`/`holding`; `setEnabled(follow || built-in protected)`; launch recovery before `builtInMonitor.start()`; quit restore after `calibration.end(.appQuit)`; resubmit report when `holding` changes.
- `native/macos/TelaViva/MonitorPopup.swift`, `SettingsView.swift` - built-in card, protection row, option toggle; `Localization.swift` - new keys en-US and pt-BR.
- `packaging/macos/build-app.sh` (SOURCES), `native/macos/TelaViva.xcodeproj/project.pbxproj`, `tools/tests/test-ambient-calibration.sh` and `test-core-bridge.sh` - new source files.
- `native/macos/Tests/AmbientCalibrationTests.swift`, `CoreBridgeTests.swift` - fixture and decode updates.
- `docs/user-guide.md`, `CHANGELOG.md` (Unreleased), `ARCHITECTURE.md` and the ADR (finalize).

### Component Relationships

`MenuBarController` feeds each core snapshot to `BuiltInProtectionController.reconcile(snapshot.builtin_protection, availability)` and to `BuiltInBrightnessMonitor` (enable flag). The controller owns the record store, uses `DisplayServicesBackend` for levels and the `BuiltInAutoBrightnessOwner` for automatic brightness. Calibration uses the same owner. The controller reports `holding` back through `core.submitBuiltInDisplay` (the monitor's last report is cached and resubmitted with the new flag).

### Data Flow

**Absence while the option is on (US-002, US-003):**
1. Camera `absent` samples: core increments `builtin.misses`; at 2 and protected, `presence_dimmed = true` (diagnostic `presence_dim` with id `builtin`).
2. Snapshot: `builtin_protection.reasons=["presence"]`, `target_percent` = the per-screen protection level (1..50), `hold_auto_brightness=true`.
3. Shell (main queue): reads baseline, saves the record, reports `holding=true`, acquires the owner (reads state, saves `auto-brightness-hold.json`, sets auto off), writes `min(baseline, target)`, verifies.
4. `present` sample: core clears the dim; snapshot `reasons=[]`.
5. Shell: writes `baseline`, verifies, releases the owner (turns auto back on unless a fresh read says it is already on), deletes both records, reports `holding=false`; the core accepts the fresh level again.

**Quit:** `applicationShouldTerminate` calls `calibration.end(.appQuit)` then `protection.restoreNow()` (synchronous, bounded) before `core.beginShutdown`; the core already forces `reasons=[]` when `closed`.

**Crash:** next launch, `owner.recoverIfNeeded()` restores automatic brightness; `protection.recoverIfNeeded()` loads the record and starts in `holding` with the stored baseline, so the normal restore path runs as soon as the first snapshot (reasons empty) arrives. A new dim never re-reads a dimmed level as baseline because `holding` is already true.

## Implementation Approach

### User Story Mapping

**US-001 (controls)** - Files: `BuiltInCardView.swift`, `MonitorPopup.swift`, `SettingsView.swift`, `Localization.swift`, `CoreBridge.swift`, `core.rs` snapshot. Dependencies: Decisions 2, 3, 10. Testing: Swift decode tests; hardware checklist (UI).

**US-002 (dim on absence/screensaver)** - Files: `builtin.rs`, `core.rs`, `BuiltInProtectionController.swift`. Dependencies: Decisions 1, 2, 5, 6. Testing: Rust contract tests (two-miss confirmation, screensaver level, strongest reduction, opt-out, asleep clears, shutdown clears); controller tests with fake backend (dim, restore, retry, availability flaps).

**US-003 (automatic brightness)** - Files: `BuiltInAutoBrightnessOwner.swift`, `AmbientCalibrationController.swift`, `settings.rs`, `core.rs`. Dependencies: Decisions 7, 8, 12. Testing: owner tests (holders, restore-only-if-still-off, unknown state, setter failure, recovery), calibration tests unchanged in call order, Rust option tests.

**US-004 (follow)** - Files: `core.rs` (freeze and `follow_target`), `MenuBarController.swift`. Dependencies: Decision 4. Testing: Rust contract test (externals do not move while the built-in is dimmed; follow resumes with a fresh level).

**US-005 (Windows handoff)** - Files: `windows-handoff.md`, checklist rows. Dependencies: Decision 13. Testing: review only; confirm existing Windows fixtures and tests are unaffected by the additive core changes (core_bridge_smoke asserts `monitors.empty()`, unaffected).

**US-006 (crash recovery)** - Files: owner and controller records, `MenuBarController.swift`. Dependencies: Decisions 6, 7. Testing: controller/owner tests that simulate a leftover record; hardware force-quit row.

## Integration Points

- `builtin_display` command (shell to core), snapshot `builtin_protection` (core to shell), `update_settings` (existing path for the new boolean, `protection_disabled`, `screensaver_targets`).
- `DisplayServicesBackend` and CoreBrightness client (existing, ADR 0007); no new private symbol.
- `BuiltInBrightnessMonitor` (reports availability and level).
- `AmbientCalibrationController` (shares the owner; refuses while holding).
- The macOS quit path in `MenuBarController` and `applicationWillTerminate` (idempotent).

## Technical Constraints

- ABI revision stays 1; JSON-only changes (api.md).
- Settings schema stays 1; the new field uses `#[serde(default)]`.
- Core policy only; shells never compute dim targets. The shell computes only `min(baseline, target)` (never raise brightness) and the restore value it recorded.
- Every new macOS UI string exists in en-US and pt-BR (`Localization.swift`, parity test); macOS says "MacBook".
- No new network use, no new dependency, no new private API symbol (so no new ADR for private APIs; one ADR for this design is written at finalize).
- Each new Swift file is added to `build-app.sh` SOURCES, the Xcode project and every `tools/tests` script that compiles it.
- Everything the shell calls on the backend runs on the main queue except the monitor's own timer queue; the backend lock serializes them.

## Windows Handoff (documented, not implemented here)

The full ordered task list goes to `windows-handoff.md`. Design outline:
1. Shell reports `protectable` and `holding` in `builtin_display` (`Platform/JsonCodec.cpp` builder; `LaptopPanelFollower` publishes) and enables polling when follow or built-in protection needs it. `protectable` is true when `detectLaptop` finds an internal panel with `WmiMonitorBrightness`, also while the lid is closed.
2. New `Platform/BuiltInProtection.{h,cpp}` mirroring the macOS controller on top of `ILaptopPanel` (fake-able). WMI snaps writes to the panel's supported levels, so "reached the target" must accept the nearest supported level (or the lowest level for 0%).
3. Generalize `AdaptiveBrightness` with holders and restore-only-if-still-off, keep the saved scheme and AC/DC values, add a recovery record next to `calibration-session.json` (`AppPaths`).
4. Parse `builtin_protection` in `parseSnapshot`; add the new key to `buildSettingsPayload` (and update `settings_payload_has_exactly_the_core_keys`, 16 to 17 keys); UI in `GeneralPage` and the popup; strings in both `Resources.resw` ("Laptop" en-US, "Notebook" pt-BR; parity and no-"Mac" tests).
5. Native tests with fakes for the controller and the owner; hardware-checklist rows L4..L6 (protection on a laptop panel, adaptive brightness during protection, force-quit recovery).
6. Windows differences to respect: sleep is one global state; lid and session display events already wired in `AppController`; the active power scheme can change while protected. `AdaptiveBrightness::restore` does not check that the value is still off: it writes the saved AC and DC values to the saved scheme in all cases, and applies the scheme only when it is still active. The owner's rule (restore unless a fresh read says the setting is on) is NEW for Windows (see `windows-handoff.md` sections 6 and 7.6).

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| The automatic-brightness setter (private CoreBrightness) is unverified on hardware | Option may not take effect | Fail open with notice and diagnostic (Decision 12); hardware checklist rows; option is off by default |
| `DisplayServicesSetBrightness` may not apply or may need a different value scale | Dim not applied or restore wrong | Readback verify within 1 point; baseline written exactly as read; retries; hardware rows |
| Concurrent DisplayServices calls from the poll queue and the main queue | Undefined private-API behavior | Lock inside `DisplayServicesBackend` (Decision 11) |
| Camera now runs for the built-in (laptop with no external monitor) | More camera use and battery | Same gating as monitors: brightness threshold, battery suspension, display sleep; poll only when needed; document in user guide |
| Behavior change on upgrade: built-in protected by default (opt-out model) | Built-in dims for users who never asked | Default matches monitors; per-screen switch in the popup and Settings; documented in CHANGELOG and user guide |
| Crash while dimmed (with the option on, automatic brightness is also off) | Screen dim, and auto off | The baseline record is written for every dim; the automatic-brightness record exists only when the option turned the setting off. Both are restored at launch (Decisions 6, 7); hardware force-quit row |
| `holding` stuck after repeated restore failures | Screen stays dim | Retry on availability change, snapshot and launch; record kept; diagnostics |
| Existing launch-time calibration recovery likely runs before the snapshot exists, so it may not restore the monitor and still deletes its record (pre-existing, found in research) | Out of scope but adjacent | Not changed here; recorded in Notes for a separate fix |
| Calibration refactor changes backend call order | Existing exact-order tests fail | Owner issues the same calls at the same points; run `test-ambient-calibration.sh` first |
| New Swift files missed in build lists | Build or test script fails | Plan task lists the three places (build-app.sh, pbxproj, tools/tests scripts) per file |
| Windows regressions from additive core fields | Windows UI or tests break | Opt-in `protectable`; Windows codec ignores unknown keys; `core_bridge_smoke` unchanged; Windows Rust CI (`cargo test --workspace`) covers the core |

## Open Questions

None. The defaults are recorded in the Decisions: the built-in screen is protected by default; presence and the screensaver share one per-screen "Protection brightness" level of 1..50 percent with default 1 (Decision 2); the poll stays at 1 s (Decision 11).

## Next Steps

1. Review this architecture.
2. Run `/codeadvisor-dev:plan`.
3. Run `/codeadvisor-dev:implement --ff` (macOS only; Windows stays documented).
