# UX parity walkthrough (T169)

Walked on 2026-10-17 against `specs/archived/windows-version/mac-ux-parity.md`. Method: code, strings and test review row by row, plus a UI Automation dump of the running Settings window (pt-BR). The development session was locked during the run, so **no rendered screenshot was captured** and every purely visual row is "Not run".

Legend: **Pass** = implemented and covered by code, strings or a test; **Not run** = needs a rendered UI, scaling, a screen reader or hardware; **Deviation** = intentional or accepted difference, with the reason.

## Flyout

| Row | Status | Evidence or reason |
|-----|--------|--------------------|
| F1 width 360, three cards then scroll, screen cap | Deviation | Width and three-card scroll match (`PopupLayout.h`, `popup_layout.cpp`). Card spacing is 8 DIP (Windows 11 flyout rhythm) instead of the macOS 12. The height cap is the fixed 640 DIP plus a work-area clamp, not the macOS `max(120, visible - 250)`. The owner may change the numbers. |
| F2 header and Refresh | Pass | `PopupWindow.xaml.cpp`, `AppController::refresh` |
| F3 empty state with the DDC explanation | Pass (fixed after the audit) | Shows `noDisplays` with the `ddcUnavailable` caption below it; height handled for two lines. Not rendered. |
| F4 "core is not linked" notice | Pass | `flyout.coreunavailable` in both locales |
| F5 card contents | Pass | `MonitorCard.xaml(.cpp)` |
| F6 `N%` label, tabular digits, draft while pending | Pass | digit alignment itself: Not run |
| F7 failed line | Pass | `flyout.failed` |
| F8 unavailable controls disabled with a reason | Pass | `flyout.reason.*` |
| F9 footer "Camera: status", one line with ellipsis, Settings and Quit | Pass | `PopupWindow.xaml` |
| F10 per-monitor protection toggle rules | Pass | `ProtectionRules.h`, `protection_rules.cpp` |
| F11 protection brightness cap, clamp, disabled at 0 | Pass | `protection_rules.cpp` |
| F12 followed monitor: no brightness slider | Pass | `FollowedRow`; followed = ambient active and the monitor eligible (the core's rule) |
| F13 writes ignored while asleep or unstable, 128-draft cap | Pass | `manual_controls.cpp` |
| F14 tray states, accessible names, no-face state before the first snapshot | Pass (tooltip fixed after the audit) | Low-light tooltip now reads "insufficient light" (pt-BR "pouca iluminação"). The tray starts in the no-face state as the macOS item does. Icon shapes: Not run. |
| Tray context menu (right click) | Pass (changed after the owner's review) | A WinUI menu window in the Windows 11 style (acrylic, rounded, icons, keyboard navigation, light dismiss) replaces the classic Win32 menu; placement above and left of the click verified with a simulated click on two anchors. Rendered look: Not run. |

## Settings

| Row | Status | Evidence or reason |
|-----|--------|--------------------|
| S1 three tabs, auto-save, no Save button | Pass | UI Automation dump lists Geral, Teclado, Brilho automático |
| S2 opens, closes the flyout, re-centers on the invoking monitor, reused | Pass (fixed after the audit) | Every open, including a re-open, centers on the tray icon's monitor and keeps the user's size. Multi-monitor placement: Not run (one monitor here). |
| S3 language picker applies at once | Pass | `GeneralPage.xaml.cpp` |
| S4 launch at login with revert and alert on failure | Pass | `launch_at_login.cpp` (11 cases) |
| S5 OLED protection controls and ranges | Pass | threshold is a NumberBox, not a slider (layout only) |
| S6 monitors in protection | Pass | |
| S7 availability: camera picker, camera status, help captions | Pass | |
| S8 keyboard tab wording | Pass (fixed after the audit) | The page now uses the override keys `hardwareKeys.toggle` and `hardwareKeys.help`. The step is a NumberBox, not a slider (layout only). |
| S9 Shift fine step, mute as zero with restore | Pass | `key_route.cpp`, `key_intent_router.cpp` |

## Automatic brightness

| Row | Status | Evidence or reason |
|-----|--------|--------------------|
| A1 follow toggle, help, unavailable reason | Pass | On this desktop the page shows "Este computador não tem tela de notebook integrada" (UI Automation) |
| A2 per-monitor status captions | Pass | "Não calibrado" seen in the dump |
| A3 calibration hidden for unsupported or unstable monitors | Pass | |
| A4 points list, Calibrate and Remove | Pass | |
| A5 add row rules | Pass | |
| A6 hint below two points | Pass | |
| A7 calibration dialog | Pass | The dialog uses MinWidth 420 rather than a fixed width. Visual size: Not run. |
| A8 end triggers | Pass | Done, Cancel, close, leaving the tab, closing Settings, display sleep, Laptop unavailable, app quit |
| A9 six notices | Pass | |
| A10 crash recovery at launch | Pass | `calibration_session.cpp` |

## Other refinements and cross-cutting checks

| Row | Status | Evidence or reason |
|-----|--------|--------------------|
| C1 popup height follows the count, Settings tall enough | Pass | |
| C2 camera stays off at or below the threshold | Pass | core logic (`core.rs`); Windows passes the setting only; not traced end to end on hardware |
| C3 first sample 1 s after resume, 5 s dark settle | Pass | core logic |
| C4 missing pt-BR strings caught by key parity | Pass | `strings_keys.cpp` (both locales have identical key sets) |
| C5 restore retried up to three times after a failed wake readback | Pass | core logic |
| Laptop and Notebook naming | Pass | en-US "Laptop", pt-BR "notebook"; no Mac, MacBook or Apple in user-visible strings (`strings_values_avoid_mac_wording`) |
| Rename rule (`settings.title`, `protection.title`) | Pass | |
| Windows wording overrides | Pass | exact text in both locales |
| Dropped macOS-only keys | Pass | `strings_dropped_keys_absent` |
| Value formats `N%`, `N s`, `1.25 s`, `N pt` | Pass | rendered output: Not run |
| Tray menu Settings and Quit | Pass | `TrayHost.cpp` |
| Brightness keys not routed | Pass | decision Q5 C; `IKeyHook.h` |

## Open items for the owner

- Rendered review (light, dark, high contrast, 100/150/225%) and Narrator: **Not run** (T137).
- Flyout comparison with Windows 11 Quick Settings (T044): owner.
- F1: whether to keep the 8 DIP spacing and the 640 DIP height cap.
