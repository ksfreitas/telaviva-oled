# Chore: Align the Windows Settings toggles to the right edge of their cards

## Chore Description
On the Windows Settings window (General, Keyboard and Automatic brightness pages), every `ToggleSwitch` floats about 100 to 150 px to the left of the card's right edge, while sliders, number boxes and combo boxes in the same cards end at the right edge. The owner wants all toggles flush right, like Windows 11 Settings, scoped to Windows only (the request names Windows; the macOS Settings is not part of this chore).

Root cause (verified in the WinUI theme, `generic.xaml` of Microsoft.WindowsAppSDK.WinUI 2.3.9, NuGet cache):
- The default `ToggleSwitch` style sets `MinWidth` to `ToggleSwitchThemeMinWidth` = 154 (`generic.xaml:8485`, applied at `:26167`).
- The template's inner grid is `HorizontalAlignment="Left"` (`:26517`), with a 40 px track (`OuterBorder`, `:26553`), a 12 px spacer column (`:26527`) and an Auto column for `OnContent`/`OffContent` that is about 0 px when both are empty.
- `MakeSettingRow` (`native/windows/TelaViva/Host/SettingRow.cpp:67-72`) forces `HorizontalAlignment(Right)` on the control. That right-aligns the 154 px box, not the 40 px track, so the track sits about 100 to 115 px left of the card's right padding edge.
- Nothing in the repo overrides the `MinWidth`: all eight Settings toggles come from the per-page `makeSwitch` helpers, which only clear `OnContent`/`OffContent`. The only toggle with `MinWidth="0"` is the flyout's `MonitorCard.xaml:32`, which already looks right and is the precedent for the fix.

Fix: in the shared `MakeSettingRow`, when the control is a `ToggleSwitch`, set `MinWidth(0)` before right-aligning it. One central change covers every current and future toggle row and keeps the three `makeSwitch` copies untouched. Expected result: the box shrinks to about 52 px (40 px track + 12 px spacer + empty content) and the track's right edge lands on the card's right padding edge, the same edge where sliders' readouts, number boxes' unit labels and combo boxes end.

The WinUI package pinned by the project is 2.5.1 (`TelaViva.vcxproj:48`) but only 2.3.9 is in the local NuGet cache, so the 154 value comes from 2.3.9. The fix (`MinWidth` 0) does not depend on the exact default, so it holds for either version; the visual check below confirms it on the real build.

## Relevant Files
Use these files to complete the chore:

- `native/windows/TelaViva/Host/SettingRow.cpp` - `MakeSettingRow` builds every settings card and forces the control to `HorizontalAlignment(Right)` at lines 67-72; this is where the `ToggleSwitch` `MinWidth` reset goes.
- `native/windows/TelaViva/Host/SettingRow.h` - declares `MakeSettingRow`; its doc comment, if any, should mention that toggles are shrunk to their track so they right-align (no signature change).
- `native/windows/TelaViva/GeneralPage.xaml.cpp` - creates six toggle rows (launch at login, presence protection, always-on camera, screensaver, battery suspension, per-monitor protection) through `makeSwitch` (`:117`) and `MakeSettingRow`; read-only here, used to verify the result.
- `native/windows/TelaViva/KeyboardPage.xaml.cpp` - `keysToggle_` row (`:130`, `:141`); read-only, verify.
- `native/windows/TelaViva/AutomaticBrightnessPage.xaml.cpp` - `followSwitch_` row (`:244`, `:248`); read-only, verify.
- `native/windows/TelaViva/MonitorCard.xaml` - flyout toggle already uses `MinWidth="0"` (`:32`); the precedent, do not change.
- `CHANGELOG.md` - add a `### Fixed` entry under `## Unreleased` (the section is empty after the 0.1.0 release heading).

### New Files
None.

## Step by Step Tasks
IMPORTANT: Execute every step in order, top to bottom. Per the repository's delegation rule, a Haiku worker makes the edits from this plan; the orchestrator builds, runs the tests and checks the result.

### Step 1: Reset the ToggleSwitch MinWidth in MakeSettingRow
- Read `native/windows/TelaViva/Host/SettingRow.cpp` lines 30-90 and `SettingRow.h`.
- In the block that narrows the control to a `FrameworkElement` and applies `VerticalAlignment(Center)` and `HorizontalAlignment(Right)` (lines ~67-74), add, before the alignment calls, a check that the control is a `Microsoft::UI::Xaml::Controls::ToggleSwitch` (for example `if (control.try_as<xc::ToggleSwitch>()) { element.MinWidth(0); }`, using the aliases already present in the file; add `#include <winrt/Microsoft.UI.Xaml.Controls.h>` only if it is not already included). Add a one-line English comment: the default template reserves 154 px, so the track would sit away from the right edge.
- Do not touch the `makeSwitch` helpers, the `ToggleSwitchPre/PostContentMargin` keys (they are vertical heights, not horizontal margins), `MonitorCard.xaml`, sliders, number boxes or combo boxes.
- If `SettingRow.h` has a doc comment for `MakeSettingRow`, add one short sentence saying toggle controls are shrunk to their track so they sit on the right edge; otherwise leave the header unchanged.

### Step 2: Add the changelog entry
- In `CHANGELOG.md`, under `## Unreleased` (above `## 0.1.0 - 2026-10-17`), add a `### Fixed` heading with one single-sentence bullet in the style of the neighbours: the Windows Settings toggles now sit flush with the right edge of their cards instead of about 100 px to the left.

### Step 3: Build and run the automated checks
- Run the Validation Commands below (the orchestrator does this, not the worker). Fix any compile error with a corrected briefing to the worker.

### Step 4: Verify the layout on the real window
- The app allows one instance, so close any running `TelaViva.exe` first (ask the owner if one is open; do not kill it without their say-so).
- Launch the freshly built `native\windows\build\x64\Release\TelaViva.exe --show-settings` and, with UI Automation, read the bounding rectangles on the General page: for every toggle, its right edge must be within 2 px of the right edge of its card minus the 16 px padding, and all toggles must share the same right edge, which must equal the right edge of the NumberBox unit labels and the slider readout in neighbouring cards. Repeat on the Keyboard and Automatic brightness pages and at 100%, 150% and 225% display scale if possible.
- Also confirm the toggles still toggle by keyboard and that Narrator names are unchanged (`AutomationProperties.Name` is untouched).

### Step 5: Run the Validation Commands
- Run every command in Validation Commands and confirm zero failures.

## Validation Commands
Execute every command to validate the chore is complete with zero regressions.

- `powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\build-rust-dlls.ps1` - the native build needs the current Rust import libraries (skip if no Rust code changed since the last run).
- `& "$(& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath)\MSBuild\Current\Bin\MSBuild.exe" native\windows\TelaViva.sln /restore /nr:false /p:Platform=x64 /p:Configuration=Release /v:minimal` - the whole solution must build with 0 errors (the Platform library is /W4 /WX).
- `$env:TELAVIVA_TEST_FIXTURES = (Resolve-Path native\windows\Tests\fixtures).Path; & native\windows\build\x64\Release\tests.exe` - all native tests pass (634 at the time of writing, 0 failed).
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -NoZip` - one-command production build, tests and packaged smoke test (needs `dist\TelaVivaOLED` not to be locked by a running app).
- Step 4's UI Automation check on `TelaViva.exe --show-settings` - toggles flush right on all three pages.

## Notes
- Scope: Windows only, as requested. The macOS Settings uses SwiftUI `Toggle` in a grouped `Form`, which the system already aligns; if the owner wants the same check there, it is a separate chore (the repository rule that new work applies to both platforms is satisfied here because the request is explicitly scoped to Windows).
- The flyout toggle in `MonitorCard.xaml` is already correct; do not change it.
- No unit test can cover this: layout lives in XAML measurement, and the repository has no Windows UI test harness. The check is the UI Automation measurement in Step 4. If the owner wants a regression guard, a small `--probe-layout` diagnostic mode would be a separate change.
- A rejected alternative: setting `MinWidth(0)` inside each of the three `makeSwitch` helpers. It works but triplicates the fix and misses any future toggle row; the central change in `MakeSettingRow` is preferred.
- Another rejected alternative: overriding the theme key `ToggleSwitchThemeMinWidth` in `App.xaml`. It would also change the flyout toggle and any WinUI dialog toggles, and depends on a framework resource key that can change between Windows App SDK versions.
- Optional doc hygiene noticed during research, not part of this chore: `specs/archived/windows-version/plan.md:105` names `TelaViva/SettingRow.xaml/.cpp`, but the file is `Host/SettingRow.cpp`.

## Implementation Status
- Step 1 done: `MakeSettingRow` resets `MinWidth` to 0 for `ToggleSwitch` controls before right-aligning them (`SettingRow.cpp`, plus a sentence in the `SettingRow.h` comment).
- Step 2 done: `CHANGELOG.md` has the `### Fixed` entry under `## Unreleased`.
- Step 3 done: the solution builds with 0 errors and `tests.exe` runs 634 tests with 0 failures.
- Step 4 pending: the measurement on the real window needs the owner's running `TelaViva.exe` (`dist\TelaVivaOLED`) closed, because the app allows a single instance. Nothing was killed.
- Step 5 partly done: build and `tests.exe` pass; `scripts\build-windows.ps1 -NoZip` and the UI Automation check wait for the app to be closed.
