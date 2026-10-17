# Architecture: MacBook Brightness Follow, Popup Sizing and Larger Settings

> Superseded in part: the calibration direction was inverted (points are monitor levels, the slider sets the MacBook). See `specs/archived/invert-macbook-calibration-direction/spec.md`.

**Feature**: macbook-brightness-follow **Date**: 2026-10-16 **Branch**: feat/macbook-brightness-follow **Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary

The Rust core gains an "ambient follow" policy: the Swift shell reports the built-in display brightness, and the core maps it through a per-monitor 4-point curve into the external monitor's brightness, using the existing baseline, ownership, protection and DDC action pipeline. The shell adds a private-API backend for the built-in display (`DisplayServices` for brightness, `CoreBrightness` for automatic brightness), a calibration controller, a tabbed and larger Settings window, a measured popup height, and a per-monitor OLED protection toggle (also core-owned, stored as an exception list so absence means "on").

## Technical Context

**Language/Stack**: Rust core (`telaviva-core`, C ABI with JSON commands) and a SwiftUI/AppKit shell (`native/macos/TelaViva`).
**Key Dependencies**: no new crates. macOS private frameworks loaded at runtime with `dlopen`/`dlsym` and the Objective-C runtime (`DisplayServices`, `CoreBrightness`), never linked. No code copied from open-source references (MIT/BSD-2 notices would be required; GPL Shifty is off limits). API facts only.
**Storage**: `settings.json` (schema 1, additive fields) plus a small shell-owned `calibration-session.json` for crash recovery, both in `Application Support/TelaVivaOLED/`.
**Testing**: `cargo test` contract tests (`core_contract.rs`, `ffi_contract.rs`); standalone `swiftc` test scripts under `tools/tests/` run by the `native-arm64` CI job; manual hardware validation notes (not assumed).
**Platform**: macOS 14+ on Apple Silicon. Windows is out of scope.

## Technical Decisions

### Decision 1: The core owns the follow policy and the curve mapping

**What**: The shell sends one command, `builtin_display {available, brightness, calibrating}`. The core stores it, computes each eligible monitor's target raw value through the curve, and reconciles it like any other brightness intent. Curves and the follow switch live in `Settings`.
**Why**: Baseline, ownership by raw value, wake read-before-write, restore retries and protection precedence are all core logic (`core.rs` `reconcile_all`). Mapping in the shell would need its own copy of the curves and would bypass those guarantees. A pure Rust mapping function is easy to test exhaustively.
**Alternatives Considered**: (a) A shell-computed `set_follow_target` command per monitor, as the core research suggested. It forces the shell to know curves and eligibility, and duplicates settings reads. (b) Reuse `set_brightness`. It sets `manual_override`, which suspends protection, so it is wrong for follow.
**Trade-offs**: The core grows by one command and one monitor field. Every `builtin_display` bumps the snapshot revision, so the shell must quantize and debounce (Decision 7).

### Decision 2: Follow targets integrate through `Monitor.follow_raw` and the existing reconcile path

**What**: Add `follow_raw: Option<f64>` to the runtime `Monitor`. It is recomputed (rounded to an integer raw value and clamped) after `builtin_display`, `update_settings`, `monitor_discovered`, and display wake. Rules:
- Follow target present: `baseline = follow_raw` immediately, so protection restore returns to the mapped value (FR-015). `manual_override` is not touched, so follow is not a manual change.
- Dimmed (presence or screensaver) or asleep: baseline only, no write. The existing `min(baseline, target)` formula keeps the dim level.
- Not dimmed and awake: the no-reason branch of `reconcile_all` targets `follow_raw` (today that branch only acts on owned monitors), so a never-written monitor still gets its first follow write and any later readback mismatch is corrected.
- Queue full: best effort, retried on the next reconcile.

**Why**: Minimal change to the proven state machine. Rounding fixes a loop the research found (a fractional target such as 37.5 against an integer readback of 38 yields read, write, read forever).
**Alternatives Considered**: Writing directly from the command handler. It skips the sleep/dim guards.
**Trade-offs**: A change made on the monitor's own buttons while following is overwritten at the next built-in change. Follow is authoritative; this is documented, not detected (the shell never sends `monitor_observed`).

### Decision 3: Settings schema stays at 1 with three additive fields

**What**: New `#[serde(default)]` fields in `Settings`: `ambient_follow_enabled: bool` (false), `ambient_curves: BTreeMap<String, AmbientCurve>`, `protection_disabled: BTreeSet<String>`. No schema bump, no migration code.
**Why**: Precedent in this repo (`api.md` documents additive schema-1 fields), and the app is unreleased (`CHANGELOG` Unreleased only). A bump would flip existing tests and add migration code for no user benefit today.
**Alternatives Considered**: (a) Bump to 2. Old binaries then refuse the file (preserved but the app is unavailable), which protects data on downgrade. (b) A separate profiles file read only by new builds. A second store, second atomic write path and split validation.
**Trade-offs**: Downgrading to a build without these fields makes it rename `settings.json` to `.corrupt` and start from defaults, losing curves. `api.md` gets the same caveat sentence as earlier additive fields. Validation also adds a cap (128 entries, keys 1–512 bytes) to both maps, closing the unbounded-map gap found for `screensaver_targets`.

### Decision 4: Per-monitor data is keyed by the full monitor ID and never pruned

**What**: Curves and protection exceptions use the existing monitor ID string. Entries of disconnected monitors stay in `settings.json` and apply again when the same ID reappears. A curve never applies to a different ID.
**Why**: Satisfies FR-023, FR-031, SC-010 with no identity redesign. Matches the `screensaver_targets` precedent.
**Alternatives Considered**: A secondary EDID-only key so settings survive a port or dock change. The code cannot prove that vendor, product, serial, EDID UUID or CoreDisplay UUID survive a port move (research Q1), and serial "0" is not special-cased, so a wrong match could apply a curve to the wrong panel.
**Trade-offs**: The same monitor on another port or dock gets a new ID and needs recalibration. This ships as a documented limitation with a Phase 0 check: run `TelaViva --diagnostics` with the same monitor on two ports and record which ID components change; if the stable part is sufficient, a follow-up ADR may add a fallback match. Settings lists only connected monitors; a "forget saved profiles" action is out of scope.

### Decision 5: Built-in display access through a protocol with a private-API backend

**What**: `BuiltInDisplayBackend` protocol (available, read level, write level, read and set automatic brightness) with one real implementation:
- Built-in display: `CGDisplayIsBuiltin` plus `DisplayServicesCanChangeBrightness` gate availability. Brightness: `DisplayServicesGetBrightness` and `DisplayServicesSetBrightness` (float 0.0–1.0), resolved with `dlsym`. Any missing symbol or nonzero status means unavailable (fail closed).
- Automatic brightness: Objective-C route verified by reflection on the development Mac (`CBClient` → `CBDisplayClient` → `CBDisplayBrightnessClient`, `isAutobrightnessEnabledWithError:`, `setAutoBrightnessEnabled:error:`). The C function `CBALCGetDisplayAutoBrightnessEnabled` returned 0 while the daemon reported true, so it is not used.
**Why**: Public API cannot read or set built-in brightness on Apple Silicon (spec Notes, DTS statement). A protocol keeps all logic testable with a fake backend and isolates the undocumented surface in one file, which the older architecture note also asked for. An ADR records the policy (adr_policy is full).
**Alternatives Considered**: `IODisplaySetFloatParameter` (does not work on Apple Silicon per second-hand reports, unverified); linking the frameworks (breaks the build if symbols move); reading raw lux from `AppleSPUVD6286` (model-specific, not needed because automatic brightness already moves the level).
**Trade-offs**: Mac App Store distribution is excluded (guideline 2.5.1), and notarization acceptance of dlopen'd private frameworks is unverified. The project already ships locally ad-hoc signed. Behavior on macOS 14, 15 and 26, on Mac mini and with the lid closed is unverified and enforced by fail-closed detection plus Phase 0 checks.

### Decision 6: Observation starts as low-rate polling, with push notification as a verified upgrade

**What**: `BuiltInBrightnessMonitor` polls `read` on a `DispatchSourceTimer` (1 s, 0.5 s leeway) only while follow is enabled, the built-in display is available and awake, and no calibration is running. A failed read keeps the last good value and counts as unknown, never zero. `DisplayServicesRegisterForBrightnessChangeNotifications` exists, but its callback signature is unverified, so push is a Phase 0 spike: if it works, the timer becomes a slow (10 s) safety net.
**Why**: One float read per second is negligible CPU and meets SC-003 (2 s). It does not depend on an unverified private callback.
**Alternatives Considered**: Notification only (unverified signature). Faster polling (needless wakeups, battery).
**Trade-offs**: Up to 1 s plus DDC latency between a MacBook change and the monitor. Idle cost is one timer wakeup per second only while the feature is on and useful.

### Decision 7: Quantize and debounce before the core sees brightness

**What**: The monitor rounds the level to whole percent, forwards it only when it changed by at least 1, and trails by 250 ms during a ramp. Availability and calibration flags go immediately. The core ignores a target when `|new follow_raw − desired| < 1`.
**Why**: Automatic brightness ramps produce many tiny steps. Each core command bumps the revision and fires the snapshot callback, and the DDC path costs about 60 ms plus read-back per write. The queue also coalesces same-kind actions.
**Alternatives Considered**: Throttle inside the core. The core has no timers (the shell drives time).
**Trade-offs**: Targets lag by at most one debounce period.

### Decision 8: Calibration is a shell session with crash-safe restore

**What**: `AmbientCalibrationController` (main-thread `ObservableObject`) owns the session:
1. First point of a session: capture built-in level, automatic-brightness state and the calibrated monitor's current brightness; write `calibration-session.json` atomically; send `builtin_display {calibrating:true}`; switch automatic brightness off (best effort).
2. Selecting a point sets the built-in display to that percent and shows the monitor slider, which writes through the existing `ManualMonitorControls.setValue` path. The slider range is clamped between the neighboring calibrated points so monotonicity cannot be broken (FR-024).
3. Confirm sends `update_settings` with the changed curve only (core validates again).
4. End (Done, cancel, leaving the tab, closing the window, quit, display sleep, built-in unavailable): restore built-in brightness, then the automatic-brightness setting if it was on, then the monitor's pre-calibration brightness, then send `calibrating:false`; delete the file. If the process died, `recoverIfNeeded()` at launch performs the same restore from the file (or reports that it could not).
If automatic brightness cannot be read or toggled, calibration still runs and shows a notice (FR-022).
**Why**: The core need not know about the MacBook screen state or crash recovery, and the restore order puts the user's environment back before follow resumes.
**Alternatives Considered**: Core-owned calibration state. It would put OS side effects the core cannot perform into the policy layer.
**Trade-offs**: A hard power loss mid-calibration is restored only at next launch.

### Decision 9: Per-monitor protection participation lives in the core

**What**: `Core::is_protected(id)` is false when the ID is in `protection_disabled`. It gates the reasons in `reconcile_all` (presence dim at the absent path, `saver_dimmed`, the `min()` target) so the existing restore logic runs when the toggle is switched off. Toggling off also clears that monitor's `presence_dimmed` and miss counter so toggling on again does not dim instantly. A protection-off monitor does not count for camera eligibility, so the camera does not run when no protected monitor remains.
**Why**: Same gate structure as the global protection flag; restore needs no new path (mirrors the existing test `disabling_protection_during_an_unacknowledged_dim_does_not_restore_blindly`).
**Alternatives Considered**: A shell-side filter. It would desynchronize the core's dimmed state.
**Trade-offs**: Follow and protection are independent: a protection-off monitor still follows (resolves the research question "(d)").

### Decision 10: Brightness keys pass through for monitors that follow

**What**: `HardwareKeyRouter` gets a primitive `setFollowedBrightnessDisplays(Set<String>)` (monitor IDs, no `CoreSettings` or `CoreBridge` types, per ADR 0005). When the key's target monitor is in the set, the brightness key latches pass-through (same mechanism as the existing gate at `:252-260`) and macOS handles it for the built-in display. Volume and mute are untouched. A monitor that is not followed (unsupported, uncalibrated) keeps key control, matching FR-027.
**Why**: Satisfies "with follow on the keys control the MacBook" without leaving an uncalibrated monitor with no hardware control.
**Alternatives Considered**: Pass every brightness key through whenever follow is on, which is the literal FR-016 wording. It would leave uncalibrated monitors controllable only by the popup slider. Revisit if the user prefers the literal rule; it is a one-line change in the set computation.
**Trade-offs**: Slightly more state in the router. `processForTesting` forces `enabled`, so tests set the set explicitly.

### Decision 11: Popup height is measured, with a three-card cap

**What**: Remove the fixed `.frame(height: 420)` and the duplicate `contentSize`. Each card reports its height through a `PreferenceKey`. A pure function `PopupLayout.listHeight(cardHeights:maxVisible:spacing:screenCap:)` returns the scroll-area height: the sum of the first `min(n, 3)` cards plus spacing, capped by the visible screen minus header and footer. The hosting controller uses `sizingOptions = .preferredContentSize` so the popover follows the SwiftUI content (deployment target 14, verify). Width stays 360. The empty state keeps its minimum.
**Why**: Card height depends on the protection row, the protection toggle row, follow mode (no brightness row) and the 9 pt status lines, so constants would drift. The math is a pure function that standalone tests cover.
**Alternatives Considered**: Computed constants per row. Simpler but already wrong when a status line appears.
**Trade-offs**: A measurement pass on first display; needs a manual look at 0, 1, 2, 3 and 4 monitors.

### Decision 12: Settings window becomes a tab view without splitting draft ownership

**What**: 640x720 default, `.resizable` added to the style mask, minimum about 560x560. `SettingsView` keeps one owner of all `@State` drafts and `save()`; tabs are `@ViewBuilder` sections inside it (General, Keyboard). The Automatic brightness tab is a separate `AmbientBrightnessView` with its own model, because it only touches new fields; `save()` copies the latest snapshot settings before overwriting its own fields, so it never clobbers them. The protection section also lists connected monitors with the per-monitor toggle (FR-030). The unused SwiftUI `Settings` scene in `App.swift` keeps compiling through defaulted initializer parameters.
**Why**: `save()` overwrites every field it knows, so a tab subview with its own copy of the drafts could write stale values.
**Alternatives Considered**: A shared `SettingsDraft` object. A larger refactor with no benefit now.
**Trade-offs**: `SettingsView` stays large; splitting it further is deferred.

## Architecture Overview

**Before**: external monitors are controlled by DDC only; brightness is set manually (popup, keys) or by protection. The built-in display is deliberately excluded everywhere. The popup is a fixed 360x420 and Settings a fixed 480x640 single form.

**After**: the shell also watches the built-in display. When "Automatic brightness" is on, each eligible monitor's brightness is derived from the MacBook level through its curve inside the core, and the popup shows volume only (plus the protection row) for those monitors. Calibration temporarily takes over the MacBook screen and automatic-brightness setting under a recoverable session. OLED protection becomes selectable per monitor. Popup and Settings sizes follow the content.

### Component Structure

**New Files/Modules:**
- `native/macos/TelaViva/BuiltInDisplayBackend.swift` - protocol plus DisplayServices/CoreBrightness implementation (the only file touching private symbols).
- `native/macos/TelaViva/BuiltInBrightnessMonitor.swift` - polling, availability, quantize/debounce, `builtin_display` submission.
- `native/macos/TelaViva/AmbientCalibrationController.swift` - session state machine, recovery file.
- `native/macos/TelaViva/AmbientBrightnessView.swift` - the tab (enable switch, monitor list, 4 point buttons, comparison slider).
- `native/macos/TelaViva/PopupLayout.swift` - height math and the card-height preference key.
- `native/macos/Tests/BuiltInBrightnessMonitorTests.swift`, `AmbientCalibrationTests.swift`, `PopupLayoutTests.swift`, with matching `tools/tests/test-*.sh`.
- `adr/0007-private-display-apis-for-built-in-brightness.md` and `adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md`.

**Modified Files:**
- `crates/telaviva-core/src/settings.rs` - new fields, `AmbientCurve` type, validation, caps.
- `crates/telaviva-core/src/core.rs` - `builtin_display` command, `Monitor.follow_raw`, curve mapping function, reconcile change, per-monitor protection gating, camera eligibility, snapshot fields.
- `crates/telaviva-core/tests/core_contract.rs`, `ffi_contract.rs` - new tests.
- `api.md`, `ARCHITECTURE.md`, `CHANGELOG.md` - contract, section, entries.
- `native/macos/TelaViva/CoreBridge.swift` - `CoreSettings` fields and `json`, `CoreMonitor.follow/protected`, `CoreAmbient`, `submitBuiltInDisplay`.
- `native/macos/TelaViva/MonitorPopup.swift` - hide brightness for followed monitors, protection toggle, height measurement.
- `native/macos/TelaViva/MenuBarController.swift` - own the new controllers, popover sizing, settings window size and style, observer lifetime and termination, feed followed IDs to the router.
- `native/macos/TelaViva/SettingsView.swift`, `App.swift` - tabs, monitor protection list, init compatibility.
- `native/macos/TelaViva/HardwareKeyRouter.swift` - followed display set.
- `native/macos/TelaViva/Localization.swift` - new keys in en-US and pt-BR, plus the existing missing pt-BR key `cameraStatus.suspended_display_sleep`.
- `packaging/macos/build-app.sh` (SOURCES list), `native/macos/TelaViva.xcodeproj/project.pbxproj` (four entries per new file), `.github/workflows/macos.yml`, `README.md` (test list).

### Component Relationships

`BuiltInBrightnessMonitor` → (`BuiltInDisplayBackend`) → `CoreBridge.submitBuiltInDisplay` → core `builtin_display` → `follow_raw` → `reconcile_all` → pending DDC action → `MacDDCController.process`/`perform` (unchanged). `AmbientCalibrationController` uses the same backend, `ManualMonitorControls` for the live monitor slider, and `CoreBridge.updateSettings`/`builtin_display{calibrating}`. `MenuBarController` wires lifecycle (start after `ddc.start`, stop on both termination paths) and pushes the followed ID set to `HardwareKeyRouter`. Views read `core.snapshot` (`ambient`, per-monitor `follow` and `protected`).

### Data Flow

- **Follow loop**: timer reads the built-in level → round, change ≥ 1, debounce → `builtin_display` → core recomputes `follow_raw` per eligible monitor → baseline updated; if awake and not dimmed, enqueue DDC write → shell writes and reads back → `hardware_result` → snapshot shows the new brightness.
- **Protection episode**: dimmed monitors keep following in the baseline only; when protection ends, the existing restore writes the baseline, which equals the mapped value for the current MacBook level.
- **Calibration**: select point → `calibrating:true` and automatic brightness off → built-in set to the point → slider writes the monitor through `set_brightness` → confirm stores the point → end restores all state and sends `calibrating:false` → core resumes follow.
- **Availability loss** (lid closed, display unplugged, API failure): `available:false` → `ambient.active` false → monitors keep their last brightness, brightness sliders return in the popup, the tab explains why.

## Implementation Approach

### User Story Mapping

**US-001/002/003 (popup sizing)** — Files: `PopupLayout.swift`, `MonitorPopup.swift`, `MenuBarController.swift`. Key: measured card heights, cap of 3 cards, screen cap. Testing: `PopupLayoutTests` (0 to 5 cards, tall cards, screen cap); manual check with 1, 2, 3 and 4 monitors.

**US-004 (larger Settings)** — Files: `SettingsView.swift`, `MenuBarController.swift`, `App.swift`. Key: 640x720, resizable, tabs. Testing: manual; save round trip stays covered by `CoreBridgeTests`.

**US-005 (follow)** — Files: core (`core.rs`, `settings.rs`), `BuiltInDisplayBackend.swift`, `BuiltInBrightnessMonitor.swift`, `CoreBridge.swift`, `MenuBarController.swift`. Testing: Rust contract tests (mapping at anchors and between, below 25%, rounding, dim and sleep deferral, protection restore returns mapped value, ineligible monitors untouched, `calibrating` pause, unavailable); Swift fake-backend tests (quantize, debounce, failed read keeps last, pause, availability).

**US-006 (calibration)** — Files: `AmbientCalibrationController.swift`, `AmbientBrightnessView.swift`, `CoreBridge.swift`. Testing: controller tests with a fake backend and temp directory (restore order, cancel, crash recovery file, automatic-brightness unavailable path, monotonic clamp); Rust test that a non-monotonic curve is rejected.

**US-007 (disabled on unsupported Macs)** — Files: `BuiltInDisplayBackend.swift`, `AmbientBrightnessView.swift`, `Localization.swift`. Testing: fake backend with no built-in display and with `CanChange` false; manual check where possible (Mac mini is unavailable, so it stays "pending acceptance").

**US-008 (popup in follow mode)** — Files: `MonitorPopup.swift`, `CoreBridge.swift`. Testing: view model gating tested through the monitor snapshot flags; manual check.

**US-009 (settings restored per monitor)** — Files: `settings.rs`, `core.rs`. Testing: persistence round trip with disconnected and reconnected IDs (new IDs do not get another monitor's curve).

**US-010 (per-monitor protection toggle)** — Files: `core.rs`, `settings.rs`, `MonitorPopup.swift`, `SettingsView.swift`. Testing: Rust tests (dim skipped, saver skipped, toggle off while dimmed restores, toggle on does not dim at once, camera eligibility); `CoreBridgeTests` for JSON.

## Integration Points

- **Core C ABI**: no change to the exported functions; only new JSON keys (`builtin_display`, settings fields, snapshot fields).
- **DDC pipeline**: unchanged; follow writes use the same actions and read-back verification.
- **Hardware key router**: new primitive set; no new dependency on core types.
- **OS**: private `DisplayServices` and `CoreBrightness` through `dlsym` and the Objective-C runtime; public `CGDisplay*` APIs for built-in detection, display reconfiguration and sleep.

## Technical Constraints

- Open-source policy: no linked private frameworks and no copied third-party code; undocumented APIs behind one protocol, fail closed, documented in ADR 0007.
- `deny_unknown_fields` and `schema_version` 1 (Decision 3 caveat).
- `CoreSettings.json` is a fixed key list and `update_settings` replaces `screensaver_targets` whole; the same applies to the two new collections (the shell must send the full value).
- The router and its test script compile alone; no `CoreSettings` or `CoreBridge` references in `HardwareKeyRouter.swift`.
- New Swift files need explicit registration in `project.pbxproj`, `build-app.sh` and the relevant test scripts (no synchronized group).
- Ownership compares raw values; follow targets are integer raw values.
- Do not assume hardware support: behavior is "pending acceptance" until validated on a MacBook with a DDC monitor.

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Private API symbols change or are missing on a macOS version | Feature unavailable or crash on resolve | `dlsym` with NULL checks, fail closed, runtime availability (Decision 5); Phase 0 checks on this Mac |
| Brightness-change callback signature unverified | Cannot use push notification | Ship polling (Decision 6); push only after a spike proves it |
| `DisplayServicesSetBrightness` and the automatic-brightness setter were never invoked by research | Calibration may not move the MacBook or toggle the setting | Phase 0 spike on hardware with the user present; the calibration notice path covers the toggle failing |
| Calibration crash or force quit leaves the MacBook dim or automatic brightness off | User-visible regression of their setup | Recovery file restored at launch (Decision 8) and on all normal exits |
| Fractional or oscillating targets cause DDC read/write loops | Battery, flicker | Integer rounding, 1-unit dead band, quantized/debounced input (Decisions 2 and 7) |
| Automatic-brightness ramps flood the core | CPU and DDC traffic | Decision 7 plus action coalescing |
| Settings downgrade resets `settings.json` | Lost curves | Documented caveat; app is unreleased (Decision 3) |
| Port or dock change creates a new monitor ID | Calibration looks lost | Documented limitation and Phase 0 diagnostic (Decision 4) |
| Lid closed with an external display: built-in leaves the online list | Follow loses its source | Availability check (`CGDisplayIsBuiltin`, online, active); sliders return (spec FR-010) |
| Mac App Store and notarization with private frameworks | Distribution limits | Out of scope today (local ad-hoc build); recorded in ADR 0007 |
| `NSHostingController.sizingOptions` behavior in an `NSPopover` | Popup does not resize | Phase 0 check; fallback is setting `popover.contentSize` from the measured height |
| A protection-off monitor is also unprotected during presence loss | OLED burn-in by user choice | Default on (FR-031); clear toggle copy |

## Open Questions

None blocking. Items to verify during implementation (Phase 0 spikes, not user decisions): the brightness-change callback signature, hardware behavior of `DisplayServicesSetBrightness` and of the automatic-brightness setter, ID stability across ports, `NSPopover` content sizing. Decision 10 deviates slightly from the literal FR-016 and is flagged for confirmation.

## Next Steps

1. Review this architecture.
2. Confirm Decision 10 (keys pass through only for followed monitors).
3. Run `/codeadvisor-dev:plan`.
