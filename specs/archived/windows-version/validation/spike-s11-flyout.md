# Spike S11: native flyout prototype

Date: 2026-10-17. Machine: Windows 11 build 26300, display 3840x2160 at 125% scale (window DPI 120), dark theme.

**Result: promising, owner review pending (task T044).** The prototype is a real WinUI 3 window in the unpackaged app with fake monitor data. It shows the Windows 11 Fluent look; two issues need work before the real flyout (T047).

Images: `images/s11-flyout-prototype-on-screen.png` (the window on the real screen) and `images/s11-flyout-prototype-window.png` (the window content, without the acrylic material because `PrintWindow` does not render it).

## What the prototype does

- Borderless `AppWindow` with an `OverlappedPresenter` (no title bar, not resizable, always on top, no minimize or maximize), 360x420 DIP converted with the window DPI (450x525 pixels at 125%).
- `DesktopAcrylicBackdrop` material; rounded window corners with `DWMWA_WINDOW_CORNER_PREFERENCE`.
- Hidden from the switchers with `AppWindow.IsShownInSwitchers`. The window class is `WinUIDesktopWin32WindowClass` and the extended style is `WS_EX_TOPMOST` without `WS_EX_TOOLWINDOW`.
- Placement from `Shell_NotifyIconGetRect` clamped into the work area, 12 DIP above the taskbar when the taskbar is at the bottom; falls back to the bottom-right corner when the icon rectangle is empty.
- Shown from a tray click and from the `--show-flyout` flag; hides when the window is deactivated.
- Content: two cards (name, status, a brightness and a volume slider with Segoe Fluent Icons), a presence row, and settings and close buttons, using only system theme resources and `AutomationProperties.Name` on the controls.

## Observations

| Aspect | Result |
|--------|--------|
| Window created, visible, not cloaked by DWM, foreground | yes |
| Fluent controls, system dark theme and accent color on the sliders | yes |
| Acrylic material over the desktop | visible on screen |
| Rounded window corners (`DWMWA_WINDOW_CORNER_PREFERENCE` = round) | applied (reads back 2) |
| Hidden from Alt+Tab through `IsShownInSwitchers` | the property is accepted; behavior with Alt+Tab not tested by an automated check |
| 1 px window border | **a bright white border is visible**; native Windows 11 flyouts have a subtle one. `DWMWA_BORDER_COLOR` with `DWMWA_COLOR_NONE` did not remove it (the read-back returned `E_INVALIDARG`, so the attribute may need a different approach, for example a custom border color matching the Windows flyout stroke) |
| Empty space under the footer | the height is a fixed placeholder; the real flyout takes it from `PopupLayout` |
| Anchor | the right edge follows the tray icon rectangle (the overflow chevron here because the new icon is in the hidden area), 15 px above the bottom edge |

## Update: the white border was the dialog frame (fixed)

Pixel probes on the running window showed that `DWMWA_BORDER_COLOR` worked (the outermost pixel was the configured RGB(58,58,58); a red test color appeared when set from outside), and that the bright line sat one pixel inside it. The window style was `0x14480000`, which contains `WS_DLGFRAME` and `WS_SYSMENU`; the OS painted a 3 px dialog-frame bevel (RGB 255 then 240) and the client area was inset by 3 px (444x244 in a 450x250 window). `SetBorderAndTitleBar(false, false)` leaves those bits, and editing the style by hand (from inside or outside the process) is reverted immediately because the `AppWindow` presenter owns the style.

Fix: create the presenter with `OverlappedPresenter::CreateForContextMenu()` (the preset meant for flyouts) and keep only the `DWMWA_WINDOW_CORNER_PREFERENCE` and `DWMWA_BORDER_COLOR` calls. Result on the same probe: style `0x14800000`, client inset 1 px, pixel 0 = (58,58,58) border, pixel 1 onward = the acrylic content (39,39,74): no white line. The screenshot shows a subtle rounded border like the system flyouts.

Re-run with `powershell -File <scratchpad>\flyout-probe.ps1` style probes after any change to the window setup.

## Not verified

- A side-by-side comparison with the real Windows 11 Quick Settings and volume flyouts (owner review), in light theme, in high contrast, at 100% and 150%, and with the taskbar on another edge.
- Open and close motion, light-dismiss behavior when clicking the taskbar or another monitor, keyboard focus order, Narrator.
- `SettingsControls` compatibility (not needed for the flyout).
- Whether the flyout idles at zero cost once hidden and destroyed (S10 follow-up).

## Next

Fix the border, then build the real flyout on this prototype (T047 and T048). The owner should run `TelaViva.exe --show-flyout`, compare it with the Quick Settings panel and list changes before the layout metrics in `Platform/PopupLayout.h` are finalized.
