# ADR-0037: Give the macOS Settings window one fixed size

**Status:** Accepted
**Date:** 2026-10-23
**Feature:** lock-macos-settings-window-size
**Spec:** [Chore plan](../specs/archived/lock-macos-settings-window-size/plan.md)

## Context

The macOS Settings window could be resized. The window opened from the menu-bar popup was created with the `.resizable` style mask and a content minimum of 760 by 560 points, and both it and the SwiftUI `Settings` scene wrapped `SettingsView` in `.frame(minWidth: 760, minHeight: 560)`. The window opens at 760 by 720 points. The owner asked that it not be resizable. The request names the Mac window, so the Windows Settings window is out of scope, which the project's platform-scope rule allows when the owner scopes a task to one platform.

The size was written in four places (`App.swift`, and three lines in `MenuBarController.swift`), and no automated test compiled `SettingsView.swift` or `MenuBarController.swift` ([ADR 0005](0005-configurable-hardware-key-step.md)). Every tab is a grouped `Form`, which scrolls on macOS, so content taller than the window is still reachable.

## Decision

- **Fixed 760 by 720 points.** The window keeps the size it already opened at. The old 560-point minimum height stops being a limit.
- **One source of truth.** `SettingsWindowLayout` (`native/macos/TelaViva/SettingsWindowLayout.swift`) holds the content size, the style mask and a `fixedSettingsWindowSize()` view modifier. The popup-launched window and the SwiftUI `Settings` scene both use it.
- **Not resizable by style mask.** The style mask is `[.titled, .closable, .miniaturizable]`, without `.resizable`, so edge dragging and the zoom and full-screen actions are unavailable. The window also sets `contentMinSize` and `contentMaxSize` to the same size as a second guard.
- **Scene guard.** The `Settings` scene adds `.windowResizability(.contentSize)`. In this accessory (`LSUIElement`) app the scene is mostly unreachable, and the popup-launched window is the real entry point, but the scene must not stay resizable.
- **A testable layout file.** The layout constants live in a file with no dependency on the rest of the shell, so `tools/tests/test-settings-window-layout.sh` can compile and check them, the way `PopupLayout` is checked. The test asserts the size and that the style mask is not resizable and keeps the title bar, close and minimize buttons. The file is registered in `packaging/macos/build-app.sh` and in `project.pbxproj`.
- No setting, string, ABI or core change.

## Alternatives Considered

- **Keep `.frame(minWidth:minHeight:)` and only drop `.resizable`.** The SwiftUI minimum would still let a hosting window grow, and the size would stay duplicated in several places.
- **Pick a shorter fixed height such as 640.** This avoids clipping on short displays. It was not chosen so that the window looks the same as before; the value is one constant in `SettingsWindowLayout.contentSize` if the owner wants it changed.
- **Lock the Windows window too.** Not requested, so left for its own change with Windows tests.

## Consequences

- On a display whose visible area is shorter than about 748 points (the window plus its title bar), the user can no longer shrink the window. AppKit constrains a titled window to the visible screen when it is shown, so it is not placed off-screen, but its bottom edge may be clipped. This is not checked on any hardware or scaling setting.
- The green button's zoom and full-screen actions are disabled.
- The window is created once per run, so the change is seen after a restart. The window has no frame autosave name, so no earlier size is restored.
- No automated test renders the window. The check that the edges give no resize cursor, that the zoom button is disabled and that all five tabs are usable is manual and is still pending.

## Non-Obvious Constraints

- A fixed `.frame(width:height:)` on the hosted view and a window style mask without `.resizable` work together: the style mask removes the user's way to resize, and the frame keeps the hosting controller from resizing the window to its content.
- [ARCHITECTURE.md](../ARCHITECTURE.md) said the Settings window "remains resizable"; the 2026-10-18 section now records the change and the Settings description is updated.
