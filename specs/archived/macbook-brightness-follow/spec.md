# Feature Specification: MacBook Brightness Follow, Popup Sizing and Larger Settings

> Superseded in part: the calibration direction was inverted (points are monitor levels, the slider sets the MacBook). See `specs/archived/invert-macbook-calibration-direction/spec.md`.

**Feature Branch**: `feat/macbook-brightness-follow` (spec folder: `macbook-brightness-follow`)

Follow-up from the user during discovery: reading and setting the MacBook brightness probably should not go through DDC; a native macOS mechanism is likely better.

## Context and Understanding

Today the app controls external monitors only (DDC brightness 0x10 and volume 0x62) and deliberately excludes the built-in display (`CGDisplayIsBuiltin` filters in `MacDDCController.swift`; archived architecture states "No built-in-display controls"). Nothing in the repo reads or writes built-in brightness or the ambient light sensor. "Low light" in the core is camera-derived, not sensor-derived.

Observed problems and gaps (verified against the code):

- **Popup** is a fixed 360x420 `NSPopover`, with the size duplicated in `MenuBarController.swift:44` and `MonitorPopup.swift:41`. A single monitor leaves a large empty area (see the screenshot: one card, a lot of dead space above the footer). Monitors already sit in a `ScrollView`, but it has no count-aware sizing.
- **Settings window** is a non-resizable 480x640 `NSWindow` (`MenuBarController.swift:210`) holding one grouped `Form` with four sections. There is no tab pattern; a new tab means introducing one.
- **Brightness coordination**: users with a MacBook plus external monitors adjust brightness separately on each screen. Since each panel has a different luminance curve, linking them by raw percentage looks wrong; the request therefore includes a per-monitor calibration curve.

Constraints from the existing product:

- Manual changes take priority over OLED protection and update the baseline (archived spec FR-005/006/016, SC-003). Hardware brightness keys go through the same core policy.
- Manual brightness values are not persisted today; only `screensaver_targets` and key steps are. Monitor IDs embed the connection path, so per-monitor persisted data can be orphaned when the connection changes.
- AGENTS.md requires open-source redistributed dependencies; macOS frameworks count as platform facilities, and undocumented facilities (IOAVService, CoreDisplay) are already used behind fail-closed behavior. No ADR covers private-API use. Apple Silicon only; hardware support must not be assumed.
- Windows is deferred; this spec is macOS-only.

## Feature Description

Three related changes to the macOS shell:

1. **Right-sized popup.** The popup height follows the number of external monitors: it fits 1, 2 or 3 monitor cards exactly (with header and footer), and for more than 3 it stays at the 3-card height and the monitor list scrolls. The size also reflects what each card shows (the card is shorter when the protection row is hidden or, in follow mode, when the brightness row is hidden).
2. **Larger Settings window.** The Settings window opens larger so all content, including the new tab, is comfortable without cramping.
3. **Follow the MacBook brightness.** When enabled, the app observes the built-in display brightness (the level the user or macOS automatic brightness sets, driven by the ambient light sensor) and sets each calibrated external monitor to the brightness that its calibration curve maps to that level. A dedicated Settings tab lets the user enable the feature and calibrate each monitor with 4 points, comparing the monitor with the MacBook screen side by side. While active, the popup shows only volume for external monitors.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As a user with one external monitor, I want the popup to be as small as its content, so it does not cover a large empty area.
- **US-002**: As a user with two or three monitors, I want the popup to grow to show all of them without scrolling.
- **US-003**: As a user with more than three monitors, I want the popup to stop growing and let me scroll the list.
- **US-004**: As a user, I want a larger Settings window so tabs and controls are not cramped.
- **US-005**: As a MacBook user, I want my external monitors to brighten and dim with the MacBook screen (including automatic brightness), so the whole desk follows ambient light.
- **US-006**: As a user, I want to calibrate each external monitor at 25%, 50%, 75% and 100% of the MacBook brightness by seeing the MacBook screen at that level and adjusting the monitor by eye until they look the same.
- **US-007**: As a Mac mini (or other Mac without adjustable built-in brightness) user, I want the feature visibly disabled, with a clear reason, instead of missing or broken.
- **US-008**: As a user with the feature on, I want the popup to show only volume per monitor (plus the protection brightness when protection is on), because brightness is now automatic.
- **US-009**: As a user who moves between places with different monitors, I want each monitor's calibration and protection setting remembered by monitor, and restored when it comes back.
- **US-010**: As an OLED owner, I want to choose which external monitors take part in protection, directly from the popup.

### Functional Requirements

**Popup sizing**

- **FR-001**: The popup MUST size its height to the monitor count: content height for 1, 2 and 3 monitors, and the 3-monitor height with a scrolling list for more than 3.
- **FR-002**: The popup height MUST account for the rows each card actually shows (brightness, volume, protection brightness), so toggling screensaver protection or follow mode changes the size consistently.
- **FR-003**: The zero-monitor state and the "core not linked" state MUST keep a sensible minimum size.
- **FR-004**: The popup width and size constants MUST live in one place (they are duplicated today). *(Direction only; mechanism is for design.)*
- **FR-005**: The popup MUST NOT be taller than the available screen area; when the content would exceed it, the monitor list scrolls.

**Settings window**

- **FR-006**: The Settings window MUST open at about 640x720 (up from 480x640) and MUST be resizable, with a sensible minimum size.
- **FR-007**: The Settings window MUST be organized in tabs: "General" (language, launch at login, OLED protection, camera availability), "Keyboard" (hardware keys) and "Automatic brightness" (the new follow feature). Existing controls are kept intact, only regrouped.

**Follow MacBook brightness: availability**

- **FR-008**: The system MUST detect whether a built-in display exists and whether its brightness can be read and changed through a native macOS mechanism (not DDC).
- **FR-009**: When no such display exists, the tab MUST remain visible but disabled, with an explanation (for example "This Mac has no built-in display with adjustable brightness"), and the feature MUST be impossible to enable.
- **FR-010**: If the native mechanism becomes unavailable at runtime, the feature MUST fail closed: external monitors keep their last brightness, the popup returns to showing brightness controls, and the user is told why.

**Follow MacBook brightness: runtime behavior**

- **FR-011**: When enabled, the system MUST detect changes of the built-in display brightness, whether caused by the user (keys, Control Center), by macOS automatic brightness, or by the app's own calibration, and apply the mapped brightness to each calibrated external monitor.
- **FR-012**: The mapping MUST use the monitor's 4-point curve: (25%, p25), (50%, p50), (75%, p75), (100%, p100), where each p is the external-monitor brightness the user judged equivalent. Between points the mapping MUST be linear interpolation. Below 25% it MUST interpolate linearly from (0%, the monitor's minimum brightness) to (25%, p25). Above 100% does not exist.
- **FR-013**: The system MUST avoid visible flicker or ping-pong: rapid built-in changes (automatic brightness ramps, key repeat) MUST be coalesced, and DDC writes MUST follow the existing serial, read-back-verified path.
- **FR-014**: Power cost MUST stay minimal: observing the built-in brightness MUST NOT require a tight polling loop; any polling interval is a design decision to justify. *(Matches the project priority on CPU and battery.)*
- **FR-015**: OLED protection MUST remain functional while follow is active. When protection ends, the monitor MUST return to the brightness mapped from the current built-in brightness (not to a stale baseline).
- **FR-016**: While follow is active, the app MUST NOT intercept hardware brightness keys for external monitors; the keys act on the built-in display (handled by macOS) and the external monitors follow it. Volume and mute keys keep their current routing. When follow is off, the current key routing applies.
- **FR-017**: Volume control (popup and keys) MUST be unaffected.

**Calibration tab**

- **FR-018**: The tab MUST contain an enable/disable control and a list of the detected external monitors. Monitors that do not support DDC brightness MUST be listed as unsupported and excluded from following.
- **FR-019**: Each monitor MUST show 4 points (25%, 50%, 75%, 100%), each indicating whether it has been calibrated.
- **FR-020**: Selecting a point MUST set the built-in display to that percentage and show a slider that adjusts the selected external monitor's brightness live, so the user can compare both screens.
- **FR-021**: Confirming the slider position MUST store it as that point of the monitor's curve; the user MUST be able to cancel without saving.
- **FR-022**: Before calibration starts, the system MUST remember the built-in brightness and whether macOS automatic brightness was on. During calibration it MUST turn automatic brightness off (so the sensor does not move the level the user is comparing against) and pause follow. When calibration ends or is cancelled, it MUST restore the original brightness and re-enable automatic brightness only if it was on before. If the app quits or crashes mid-calibration, the next launch MUST restore the saved state or report that it could not. If automatic brightness cannot be read or toggled, calibration is still allowed and the user is told it may drift.
- **FR-023**: Curves MUST persist across launches, validated in the shared core like other settings, keyed by monitor ID. Curves of monitors that are currently disconnected MUST be kept, not deleted, and MUST be restored automatically when that monitor ID reappears (for example the user connects different monitors at work and the home ones later). A curve MUST NOT apply to a different monitor ID.
- **FR-024**: The curve MUST be monotonic non-decreasing; the UI MUST prevent saving a point that breaks that rule and explain why.
- **FR-025**: The follow toggle is global but applies per monitor: a monitor follows only if it supports DDC brightness and has all 4 points calibrated. Others keep manual control and the follow tab shows why they are excluded.

**Popup in follow mode**

- **FR-026**: While follow is active, the card of a followed monitor MUST show volume and, when screensaver protection is on, the "Brilho na proteção" row; it MUST NOT show the brightness slider.
- **FR-027**: A monitor that is not followed (unsupported or uncalibrated) MUST keep its brightness slider so it never becomes uncontrollable.
- **FR-028**: New and changed user-visible text MUST be provided in en-US and pt-BR with matching keys.

**Per-monitor protection participation (new, requested during discovery)**

- **FR-029**: Each external monitor MUST have an on/off setting for participation in OLED protection (presence dimming and screensaver dimming). Today protection applies to every external monitor.
- **FR-030**: The setting MUST be reachable from the popup, as a toggle on each monitor card, and MUST be visible in Settings (in the General tab's protection section, as a list of monitors with the same toggle) so the state is findable in both places.
- **FR-031**: The default MUST be on for every monitor, including monitors already known, so existing behavior does not change on upgrade. The setting MUST persist keyed by monitor ID and be restored when that monitor reconnects, same as curves (FR-023).
- **FR-032**: A monitor with protection off MUST never be dimmed or have its brightness changed by presence or screensaver protection, and its "Brilho na proteção" row MUST be hidden or disabled. Turning protection off while the monitor is currently dimmed MUST restore it through the existing restore path.
- **FR-033**: The card height rules (FR-002) MUST cover the new toggle.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With 1, 2 and 3 external monitors the popup shows every card with no scrolling and no empty band larger than a normal margin; with 4 or more it shows exactly 3 cards' worth and scrolls.
- **SC-002**: The Settings window shows each tab's full content with no clipped control or inner scrolling at its default size.
- **SC-003**: On a MacBook, changing built-in brightness (keys, Control Center, or automatic brightness) updates every calibrated external monitor within 2 seconds, with no oscillation, and with no sustained CPU usage while idle.
- **SC-004**: After calibration, at each of the 4 points the external monitor is set to the user-chosen value (read-back within the existing tolerance), and values between points are interpolated per the decided rule.
- **SC-005**: On a Mac without adjustable built-in brightness the feature cannot be enabled and the reason is displayed.
- **SC-006**: Calibration leaves the built-in brightness as it was before (per the decision in Q4) and never leaves the external monitor at an unintended level after cancel.
- **SC-007**: Enabling follow never removes the only brightness control of a monitor that cannot follow.
- **SC-009**: A monitor with protection off is never dimmed by presence or screensaver; turning it back on resumes protection within one evaluation cycle; both states survive restart and reconnection.
- **SC-010**: Disconnecting a monitor and reconnecting it (same ID) restores its curve and protection setting with no user action.
- **SC-008**: The new logic is covered by tests that run in the existing standalone Swift test scripts and Rust contract tests; hardware behavior is validated manually on a MacBook with at least one DDC-capable external monitor and recorded as validation notes, not assumed.

## Decisions (discovery answers)

- **Q1 Popup height**: content-driven, capped by the visible screen area; more than 3 monitors shows 3 cards' height and scrolls.
- **Q2 Settings**: about 640x720, resizable, tabs General / Keyboard / Automatic brightness.
- **Q3 Curve**: linear interpolation, linear from 0% to the first point below 25%, strictly guarded monotonic points, global toggle with per-monitor eligibility (supported and fully calibrated), others keep their slider.
- **Q4 Calibration**: save built-in brightness, pause follow, turn automatic brightness off during calibration, restore brightness and re-enable automatic brightness only if it was on before.
- **Q5 Protection in follow mode**: protection keeps working and returns to the mapped brightness; card shows volume plus "Brilho na proteção". Extra request: per-monitor protection participation toggle in the popup and in Settings (FR-029 to FR-033).
- **Q6 Keys**: with follow active the app does not handle brightness keys for externals; keys control the MacBook and externals follow.
- **Q7 Identity**: curves and protection settings are keyed by monitor ID, kept for disconnected monitors and restored on reconnection. Known limitation: the current ID embeds the connection path, so the same monitor on a different port or dock may not match its saved settings; see Open items for design.
- **Q8 Built-in brightness API**: use the private `DisplayServices` framework with fail-closed behavior (feature disabled if unavailable) and write an ADR for undocumented-API use.

## Open items for /design

- Stability of the monitor ID across ports and docks (Q7 limitation): whether to add a secondary EDID-based match (vendor, product, serial) so settings follow the monitor across ports without ever applying to a wrong monitor, or accept "recalibrate" after a port change.
- How to read and toggle macOS automatic brightness (FR-022) and how to observe built-in brightness changes cheaply (FR-014). Both are unverified; if automatic brightness cannot be toggled, define the degraded calibration experience.
- Where the curve mapping and the protection-participation setting live (core versus shell), and the settings schema change given `deny_unknown_fields` (schema version bump or tolerant fields).
- Crash-safe restore of built-in brightness and automatic brightness after an interrupted calibration (FR-022).
- Interaction between follow, `manual_override` and the baseline in the core (for example whether a followed change counts as a manual change).

## Notes

- **Built-in brightness API (design input, from the user's hint plus repo findings).** No public, stable API reads/sets built-in brightness on Apple Silicon. The usual route is the private `DisplayServices` framework (`DisplayServicesGetBrightness`, `DisplayServicesSetBrightness`, `DisplayServicesCanChangeBrightness`), present in the local SDK's PrivateFrameworks but not linked by the app today. Its symbol names and change-notification options come from memory and MUST be verified during `/design`. Reading the resulting brightness is enough; reading the raw ambient light sensor is not needed because automatic brightness already moves the built-in level.
- **Policy gap.** AGENTS.md says open-source redistributed dependencies; the repo already uses undocumented facilities (CoreDisplay, IOAVService) as platform facilities with fail-closed behavior, but no ADR covers it. Adding another undocumented framework likely warrants an ADR (adr_policy is "full").
- **Hardware-key router** treats built-in as excluded in many places (`HardwareKeyRouter.swift` `isBuiltin` checks); follow mode must not make brightness keys on the MacBook route to external monitors.
- **Core/shell split.** Rust core owns policy, settings validation and storage; Swift owns discovery, I/O and UI. Where the curve mapping lives is a design question, but ownership compares raw values (`core.rs` ~778–780, 889–891), so any mapping must keep raw as the ownership unit. Files likely touched are listed in the discovery report: `settings.rs`, `core.rs`, `core_contract.rs`, `api.md`, `CoreBridge.swift`, `MonitorPopup.swift`, `ManualMonitorControls.swift`, `HardwareKeyRouter.swift`, `MacDDCController.swift`, `Localization.swift`, `SettingsView.swift`, `MenuBarController.swift`, `App.swift`, tests, ARCHITECTURE.md, CHANGELOG.md and a new ADR.
- **Settings compatibility.** `Settings` uses `deny_unknown_fields` with `schema_version` 1: an older binary reading a file with new fields treats it as corrupt and resets it. Adding curve data needs a schema decision.
- **Pre-existing issues spotted, out of scope**: pt-BR is missing `cameraStatus.suspended_display_sleep` in `Localization.swift` (the key would show raw); the unused `Settings` scene in `App.swift:44-49`; no test covers popup/settings layout.
- **Branch note.** `feat/macbook-brightness-follow` was created from `feat/oled-monitor-protection`; the previous feature's work has since been committed (HEAD `80f6e0c`) and is the base of this branch.
