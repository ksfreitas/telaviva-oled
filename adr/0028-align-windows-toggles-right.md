# ADR-0028: Right-align Windows Settings toggles by shrinking the ToggleSwitch box

**Status:** Accepted
**Date:** 2026-10-18
**Feature:** align-windows-toggles-right
**Spec:** [Archived chore plan](../specs/archived/align-windows-toggles-right/spec.md)

## Context

On the Windows Settings window (General, Keyboard and Automatic brightness pages), every `ToggleSwitch` sat about 100 to 150 px to the left of the card's right edge, while sliders, number boxes and combo boxes in the same cards ended at the edge. The owner wanted all toggles flush right, as in Windows 11 Settings, on Windows only; the macOS Settings uses SwiftUI `Toggle` in a grouped `Form`, which the system already aligns.

The WinUI default `ToggleSwitch` style sets `MinWidth` to the theme value 154 (`ToggleSwitchThemeMinWidth`, read from `generic.xaml` of Windows App SDK WinUI 2.3.9), and its template draws a 40 px track, a 12 px spacer and an almost empty content column at the left of that box. `MakeSettingRow` right-aligned the whole 154 px box, so the track landed about 100 to 115 px away from the card's padding edge. The only toggle that already looked right was the flyout toggle in `MonitorCard.xaml`, which sets `MinWidth="0"`.

## Decision

- **Reset `MinWidth` in the shared row builder.** `MakeSettingRow` (`native/windows/TelaViva/Host/SettingRow.cpp`) sets `MinWidth(0)` on a control that is a `ToggleSwitch` before it applies the vertical centering and the right alignment. The box shrinks to about 52 px and the track's right edge lands on the card's right padding edge, the same edge where slider readouts, number box unit labels and combo boxes end. One central change covers all eight current Settings toggles and any future toggle row. `SettingRow.h` documents it in one sentence.
- **No signature, resource or XAML change.** The three per-page `makeSwitch` helpers, `MonitorCard.xaml`, sliders, number boxes and combo boxes are untouched.

## Alternatives Considered

- **`MinWidth(0)` inside each of the three `makeSwitch` helpers.** Rejected: it triplicates the fix and misses any toggle row added later.
- **Override the theme key `ToggleSwitchThemeMinWidth` in `App.xaml`.** Rejected: it also changes the flyout toggle and any WinUI dialog toggles, and it depends on a framework resource key that can change between Windows App SDK versions.

## Consequences

- Toggles in Settings cards sit flush right on every page, on Windows only. No core, ABI, string or macOS change.
- No automated test covers it: layout comes from XAML measurement and the repository has no Windows UI test harness. The build compiles with 0 errors and the 634 native tests pass. The owner checked the real window by eye and found it right; the planned UI Automation measurement of bounding rectangles at 100%, 150% and 225% scale was not run, and neither was the one-command production build `scripts\build-windows.ps1 -NoZip`.

## Non-Obvious Constraints

- The 154 value comes from WinUI 2.3.9, the version in the local NuGet cache; the project pins 2.5.1. The fix sets the width to zero, so it does not depend on the exact default.
- A toggle that needs on or off text next to it would have to be handled in its own row, because `MakeSettingRow` now removes the minimum width reserved for that text. The current toggles clear `OnContent` and `OffContent`.
- `ToggleSwitchPreContentMargin` and `ToggleSwitchPostContentMargin` are vertical heights, not horizontal margins, and have no part in this alignment.
