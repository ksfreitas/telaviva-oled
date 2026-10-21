# Feature Specification: Laptop display in brightness controls and protection, with automatic brightness off during protection

**Feature Branch**: `laptop-display-protection-and-auto-brightness`

## Context and Understanding

Today the built-in screen of a MacBook (macOS) or laptop/notebook (Windows) is not a first-class display in the app:

- The core `monitors` map holds only external DDC monitors. Both shells explicitly exclude the built-in panel from DDC discovery (macOS `CGDisplayIsBuiltin == 0`; Windows `isInternalPanel`). The built-in is only a runtime reading (`builtin_available`, `builtin_percent`, `calibrating`) used by the "follow the laptop brightness" feature.
- OLED protection (presence absence, screensaver) therefore never dims the built-in screen. The popup card, the per-monitor protection toggles and the protection brightness slider list external monitors only.
- Automatic brightness (macOS "Automatically adjust brightness"; Windows "Change brightness automatically when lighting changes") is touched only during calibration (macOS through the private CoreBrightness client; Windows through the adaptive-brightness power setting, AC and DC). Protection never touches it. If automatic brightness stays on while the app dims the built-in screen, the OS raises the brightness again and defeats the protection.

The request is to bring the built-in screen into the brightness and protection controls, and to let the user have automatic brightness turned off while protection is engaged (restoring it afterwards), on both platforms. This plan **implements and verifies macOS only**. Windows is **designed and broken into tasks in this plan, with no Windows code written now**; the owner will implement it later in a Windows environment. This is an explicit owner scoping of the "both platforms in the same change" rule in AGENTS.md (Windows is deferred by the owner, not forgotten).

## Feature Description

The built-in screen of a laptop becomes a protectable, controllable display alongside the external monitors:

1. It appears in the brightness and protection controls (popup card and Settings protection list) with a protection switch, a per-screen configurable protection brightness and a brightness slider. It has no volume control.
2. A new option on the built-in screen, **off by default**, makes the app turn the OS automatic brightness off while protection dims the built-in screen, so the OS cannot undo the dim.
3. When protection ends (face returns, screensaver stops, protection switched off for the screen, app quits), the app restores the previous brightness and, if the option was on and the app turned automatic brightness off, puts it back to its previous state.
4. Calibration and "follow the laptop brightness" keep working and cooperate with the new behavior: follow pauses while the built-in screen is dimmed by protection, and calibration and protection share one owner of the automatic brightness setting.

Value: laptop panels get the same burn-in and power protection as external monitors, and for users who enable the option the protection is reliable because automatic brightness cannot fight it.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As a laptop owner, I see my built-in screen in the brightness and protection controls, so I can choose whether it is protected and how dark it gets.
- **US-002**: As a laptop owner, when I walk away (no face detected) or the screensaver starts, the built-in screen dims like my external monitors do.
- **US-003**: As a laptop owner, I can enable an option on the built-in screen so that, while protection dims it, the system's automatic brightness is switched off and later switched back on if it was on before. By default the option is off and automatic brightness is never touched by protection.
- **US-004**: As a laptop owner who also uses "follow the laptop brightness", the dimmed built-in level is never read as a request to dim my monitors, and my monitors are restored correctly afterwards.
- **US-005**: As the project owner, I get a Windows design and a task list (Laptop in en-US, Notebook in pt-BR) that I can execute later on a Windows machine, without the macOS change breaking the current Windows shell in the meantime.
- **US-006**: As a user, if the app is force-quit or crashes while protection is active, the next launch puts the screen brightness and (when the option was used) automatic brightness back, instead of leaving the screen dim with automatic brightness off.

### Functional Requirements

- **FR-001**: System MUST represent the built-in screen as a protectable display in the shared core, with a stable identity for per-display protection opt-out, without breaking the existing external monitor model or the settings schema (older settings files must still load).
- **FR-002**: System MUST list the built-in screen in the macOS popup card and the General-tab protection list with a protection switch and a per-screen configurable protection brightness, only when a built-in screen is present and available. The card has no volume control. While the lid is closed (the built-in screen is unavailable) it MUST NOT be listed in the popup or in the Settings protection list, and it MUST reappear when the screen is available again (owner request, 2026-10-20).
- **FR-003**: System MUST dim the built-in screen on presence absence and on screensaver start, applying the same confirmation rules (two consecutive misses, confirmation latency) as for external monitors.
- **FR-004**: System MUST use the same dimming-level model as external monitors for the built-in screen: a level per screen, fixed at 1..50 percent with a default of 1 (FR-019), shared by presence and screensaver, so the two reasons never stack.
- **FR-005**: System MUST show a brightness slider for the built-in screen in the popup card that writes the built-in level, and MUST keep hardware keys unchanged.
- **FR-006**: System MUST provide a setting, shown with the built-in screen (macOS: MacBook), that is **off by default**. When it is on, System MUST turn the OS automatic brightness off before the built-in screen is dimmed by protection, and only when the built-in screen is actually being dimmed (not when only external monitors are protected, and not when the built-in screen is excluded from protection). When it is off, protection MUST NOT read or change automatic brightness.
- **FR-007**: System MUST record the automatic brightness state it found before changing it, and on protection end MUST restore that state unless a fresh read says it is already on (never overriding a user's explicit change made during protection). A nil or off read still restores, because the app turned the setting off.
- **FR-008**: System MUST treat calibration and protection as two users of one automatic brightness owner: whichever suspends it first records the original state, and the setting is restored only when the last user releases it (no case where the second user records "already off" and never restores).
- **FR-009**: System MUST pause "follow the laptop brightness" while the built-in screen is dimmed by protection, MUST NOT publish the dimmed level as a follow input, and MUST resume with a fresh read after the baseline is restored.
- **FR-010**: System MUST persist a recovery record (`builtin-protection-session.json`) whenever protection dims the built-in screen, whether or not the option is on, and an automatic-brightness record while the app holds the setting off. On next launch the app restores the built-in level from the first and automatic brightness from the second. The built-in baseline MUST NOT be taken from a dimmed reading after a crash.
- **FR-011**: System MUST restore the built-in brightness (and automatic brightness when applicable) on app quit, on display sleep/wake and on lid close/open following the existing read-before-write and no-write-to-asleep-display rules, with bounded retries.
- **FR-012**: System MUST fail open if automatic brightness cannot be read or changed (including the unverified macOS private setter): the screen is still dimmed, a diagnostic is logged and a notice is shown, and no restore of a setting that was never changed is attempted.
- **FR-013**: System MUST keep policy in the shared core (shells never compute dim targets) and keep the C ABI at revision 1 unless the design proves a change is required; any new effect for "suspend/restore automatic brightness" MUST be level-triggered because each snapshot revision is published once.
- **FR-014**: System MUST provide localized strings in en-US and pt-BR for every new macOS UI text (macOS says "MacBook"), including the new option, its help text and the failure notice, and these MUST say clearly that they refer to the system's automatic brightness, not the app's "Automatic brightness" tab.
- **FR-015**: System MUST NOT regress the current Windows shell: shared core changes (built-in display, new setting, new snapshot fields) MUST be backward compatible with the existing Windows codec, UI and tests, and the Windows native tests MUST keep passing when they are next run. No Windows source is written in this plan.
- **FR-016**: The plan MUST include a Windows section (design notes and an ordered task list) covering: built-in panel as a protectable display through `ILaptopPanel`/`WmiPanel`, adaptive brightness through the existing `AdaptiveBrightness`/`IPowerSettings` with the shared owner, the new option in the Windows UI with "Laptop"/"Notebook" strings, native tests with fakes, a recovery record, and new rows for `hardware-checklist.md` (protection on a laptop panel, adaptive brightness during protection, force-quit recovery). The Windows part MUST NOT be reported as implemented or verified.
- **FR-017**: System MUST add no new network use and no new dependency beyond what ADR 0007 (macOS private APIs) already accepts; a new ADR is required if a new private API or a new Windows API path is introduced.
- **FR-019**: System MUST treat the protection brightness as a fixed 1..50 percent level per screen, independent of the screen's current brightness, applied for both presence absence and the screensaver, with a default of 1 percent (owner request, 2026-10-20). Stored values outside the range are clamped when applied.
- **FR-020**: System MUST NOT use the built-in camera while the laptop lid is closed; it behaves as if the camera were disconnected, without losing the user's remembered camera choice, and it is usable again when the lid opens (owner request, 2026-10-20; macOS now, Windows documented).
- **FR-021**: System MUST let the user choose which display is monitored to turn camera presence detection on or off (owner request, 2026-10-20). The camera gate uses the gate display: the monitored display, or the next awake display in the same order when it sleeps. The camera runs while the gate display is above its threshold, or while any display is presence-dimmed; absence still dims every protected screen, the MacBook screen included. The choice works like the webcam choice: a picker, remembered by priority (most recent first, up to 8), and "Automatic" (the default) picks the main external display, then the other external displays by id, then the Mac screen. When the chosen display is not connected, the next one in the priority list that is connected is used, then Automatic.
- **FR-022**: System MUST save the presence-check brightness threshold ("Verificar presença só acima do brilho", 0..90 percent) per display, together with the display's id, following the same remembered-by-environment logic as the monitor selection (owner request, 2026-10-21). A display without its own value uses the global default `presence_min_brightness_percent`. The Settings slider edits the threshold of the monitored display (or the global value when no display resolves), and the camera gate compares the gate display's brightness with that display's own threshold.
- **FR-018**: System MUST keep CPU, memory and battery cost minimal: no new polling loop for automatic brightness; reads happen on protection transitions only.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On a MacBook with the option on and automatic brightness initially on, within the usual absence-confirmation time after the user leaves, the built-in screen reaches its protection level and stays there for at least 10 minutes (no OS-driven brightening).
- **SC-002**: After the user returns (or the screensaver stops), the built-in brightness returns to its value before protection and, when the option was on, automatic brightness returns to its prior state, in 100% of the tested transitions (presence, screensaver, switch off, quit, sleep/wake, lid close/open).
- **SC-003**: After a force-quit while protection is active with the option on, the next launch restores the built-in level and automatic brightness without user action.
- **SC-004**: During protection of the built-in screen with "follow" on, external monitor brightness does not change as a result of the dimmed built-in level.
- **SC-005**: Calibration followed by protection (and the reverse) always ends with automatic brightness in its original state.
- **SC-006**: With the option off (default), automatic brightness is never read or changed by protection.
- **SC-007**: All new macOS UI strings exist in en-US and pt-BR, enforced by the existing locale parity tests, and the existing Windows tests still pass unmodified when run later.
- **SC-008**: The plan contains a complete Windows task list and hardware-checklist rows that the owner can execute without redoing discovery or design.
- **SC-009**: With the lid closed the built-in screen is absent from the popup and from the Settings protection list; opening the lid brings it back within one availability refresh.

## Decisions

Owner answers to the discovery questions (2026-10-20):

- **Q1 (scope of controls)**: A, without volume. Card for the built-in screen with protection switch, protection brightness and brightness slider; no volume control; hardware keys unchanged.
- **Q2 (triggers)**: A. Presence absence and screensaver, as for external monitors.
- **Q3 (level)**: A, configurable per screen. Same model as external monitors.
- **Q4 (when automatic brightness goes off)**: A. Only when the built-in screen is actually dimmed by protection.
- **Q5 (fixed vs option)**: B. An option on the built-in screen of the MacBook/notebook/laptop, **disabled by default**.
- **Q6 (setter failure)**: A. Dim anyway, log a diagnostic, show a notice, restore nothing that was not changed.
- **Q7 (Windows)**: B. Windows is designed and its tasks documented in this plan, with no Windows code; the owner implements it later on a Windows machine.

## Notes

### Findings from codebase research (verified by reading, not by running)

- macOS: `BuiltInDisplayBackend` (`native/macos/TelaViva/BuiltInDisplayBackend.swift`) already exposes `readLevel`, `writeLevel`, `autoBrightnessEnabled` and `setAutoBrightness` (private DisplayServices and CoreBrightness, ADR 0007). Both setters are unverified on hardware (only getters ran, on one M4 Pro). `AmbientCalibrationController` is the only caller of the auto brightness setter today and persists `calibration-session.json` for crash recovery.
- Windows: `Platform/LaptopPanel.cpp` reads and writes the panel through WMI (`WmiMonitorBrightness`, `WmiSetBrightness`); `Platform/AdaptiveBrightness.cpp` suspends and restores the adaptive-brightness power setting (AC and DC) and is called only from calibration. There is no ADR for the Windows WMI path. Hardware rows L2 and L3 (laptop follow, adaptive read/write) are "Not run" and the repository's validation machine is a desktop.
- Core: protection has no enter/exit event. State is bools and counters on `Monitor` plus `brightness.reasons`; the best choke point is a change in whether `brightness.reasons` is empty inside `reconcile_all` (`core.rs`). The core emits effects only through the controls handle (`tv_controls_*`, DDC `run_attempt`), which is keyed to DDC monitors and VCP codes; the built-in panel cannot be addressed through it. This is the main design question for `/design`.
- Reasons reflect decided intent, not applied state: they stay set after a failed dim write and reconcile re-enqueues the write.
- Before this change no protection state was persisted, so a crash while dimmed made the next launch adopt the dim level as the baseline (existing behavior for monitors, and a risk for the built-in screen). The built-in screen now keeps a baseline record (FR-010).
- Settings: adding a boolean is a documented checklist (core settings struct, default, `update_settings` arm, both shells, tests). Settings use `deny_unknown_fields`, so an older build that reads a newer file renames it to `.corrupt` and resets everything (accepted trade-off today). Because Windows is deferred, the design must check that the current Windows codec tolerates the new snapshot and settings fields and that the Windows UI does not suddenly list a built-in entry it cannot control.
- Naming trap: in both UIs "Automatic brightness" is the app's own tab for the follow feature, not the OS setting. New texts must say clearly which one they mean.
- `docs/user-guide.md` lines 84-104 describe macOS behavior and conflict with the Windows keys rule (Windows FR-016 of the archived Windows spec); do not copy them for Windows text.

### Constraints to carry into design

1. Shared owner of automatic brightness between calibration and protection (FR-008).
2. Recovery record for protection (FR-010), plus quit-path handling: restoration today depends on the shell polling after `tv_shutdown` with an 8 s cap, and `applicationWillTerminate` does not wait.
3. Automatic brightness is a global OS setting that also affects an unprotected built-in screen and interacts with follow (which reads the built-in level every second).
4. macOS automatic brightness setter is a private API: no Mac App Store, hardware verification required (extend the macOS hardware checklist with protection rows).
5. Windows power-scheme adaptive setting is global, AC and DC, and the active scheme can change; restore only if the saved scheme is still active (already how `AdaptiveBrightness::restore` works). Some laptops may also have a separate content-adaptive control that this change does not touch (to confirm on hardware, later).
6. Not every laptop exposes `WmiMonitorBrightness`; the built-in must be absent from the controls in that case.
7. The built-in display in the core must be introduced only when a shell reports it, so the unchanged Windows shell behaves exactly as today (FR-015).

### Testing direction

- Core: contract tests for the built-in protected display (enter, exit, restore, sleep/wake, shutdown, follow pause, option on/off), settings compatibility with old files, snapshot compatibility with the current Windows codec fixtures.
- macOS: unit tests with a fake `BuiltInDisplayBackend` for the shared owner, the option, the recovery record and the failure notice; hardware checklist rows for protection, automatic brightness off and on, force-quit recovery, lid close and wake.
- Windows (later, by the owner): native tests with fakes (`ILaptopPanel`, `IPowerSettings`) in the existing harness; hardware-checklist rows left "Not run"; the macOS tests and rows are the template.
