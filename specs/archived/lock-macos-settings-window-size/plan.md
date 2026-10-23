# Chore: Lock the macOS Settings window size

## Chore Description

The macOS Settings window is currently resizable: the AppKit window opened from the menu-bar popup is created with the `.resizable` style mask and a `contentMinSize` of 760 x 560, and both it and the SwiftUI `Settings` scene wrap `SettingsView` in `.frame(minWidth: 760, minHeight: 560)`. The owner wants the window to stay at a fixed size: the user must not be able to resize it (no edge or corner drag, and the green zoom button disabled).

Scope and decisions:

- **macOS only.** The request names the Mac window. AGENTS.md says every change applies to both platforms unless the owner explicitly scopes it to one; this task is scoped to macOS by the owner's wording. The Windows Settings window is not touched.
- **Size stays 760 x 720 points.** 760 x 720 is what the window opens at today (`MenuBarController.swift:345`) and 760 is the documented width (ARCHITECTURE.md, "macOS Settings and popup layout"). The minimum height of 560 stops being a limit and becomes the fixed height of 720. Every tab's `Form` uses `.formStyle(.grouped)`, which scrolls on macOS, so content that is taller than 720 points (for example the General tab with several monitors) still scrolls inside the fixed window.
- **One source of truth for the size.** Today 760/560 is written in three places (`App.swift:50`, `MenuBarController.swift:344`, `MenuBarController.swift:348`) plus the 720 in `:345`. A new small `SettingsWindowLayout` file holds the size, the style mask and a SwiftUI helper, so the two Settings entry points cannot drift apart. It follows the `PopupLayout.swift` pattern (a pure layout file with its own swiftc test script), because no automated test compiles `SettingsView.swift` or `MenuBarController.swift` (see ADR 0005).
- **No behavior change otherwise.** The window is still created once per run, centered on the invoking screen, with the same title, close handling and calibration end on close. No settings, ABI, core or localization change, so no new strings and no Rust work.

## Relevant Files

- `native/macos/TelaViva/MenuBarController.swift`: `openSettings(tab:)` (:327-368) creates the window. Remove `.resizable` from the style mask (:346), replace `.frame(minWidth: 760, minHeight: 560)` (:344) with the fixed-size helper, and replace `contentMinSize` (:348) with a fixed content size.
- `native/macos/TelaViva/App.swift`: the SwiftUI `Settings` scene (:45-51) uses `.frame(minWidth: 760, minHeight: 560)`. Replace it with the same fixed-size helper and add `.windowResizability(.contentSize)` to the scene so the scene window also cannot be resized. In this accessory (`LSUIElement`) app the scene is mostly unreachable (the real entry point is `openSettings`), but it must not be left resizable.
- `native/macos/TelaViva/PopupLayout.swift`: reference only, for the style of a pure layout file with `import CoreGraphics` / `import SwiftUI`.
- `native/macos/Tests/PopupLayoutTests.swift` and `tools/tests/test-popup-layout.sh`: reference only, for the test harness pattern (a `@main` struct with an `expect` helper that exits 1, built with `swiftc` into a temp dir).
- `packaging/macos/build-app.sh`: the `SOURCES=(...)` list at :44 names every Swift file explicitly, so the new file must be added there or the app build fails to find `SettingsWindowLayout`. `native/macos/TelaViva.xcodeproj` does not list the source files by name (no match for `PopupLayout`), so it needs no edit; the implementer should confirm this with `grep -c SettingsView native/macos/TelaViva.xcodeproj/project.pbxproj` and add the file only if `SettingsView.swift` is referenced there.
- `.github/workflows/macos.yml`: runs each `tools/tests/test-*.sh` as its own step (the popup-layout step is at about :43). Add a step for the new test.
- `ARCHITECTURE.md`: :164 says "Settings is a larger, resizable window" and :309 (dated 2026-10-18 section) says "It remains resizable". Both become wrong.
- `CHANGELOG.md`: add an entry under `Unreleased` > `Changed`.

### New Files

- `native/macos/TelaViva/SettingsWindowLayout.swift`: the fixed size, the window style mask and a SwiftUI view helper.
- `native/macos/Tests/SettingsWindowLayoutTests.swift`: asserts the size and that the style mask is not resizable.
- `tools/tests/test-settings-window-layout.sh`: compiles and runs that test, like `test-popup-layout.sh`.

## Step by Step Tasks

### [x] 1. Add `SettingsWindowLayout.swift`

Create `native/macos/TelaViva/SettingsWindowLayout.swift` (comments in English, no user-facing strings, so no `// I18N` needed):

```swift
import AppKit
import CoreGraphics
import SwiftUI

enum SettingsWindowLayout {
    /// Fixed content size of the Settings window, in points. The window is not resizable; tab content that
    /// does not fit scrolls inside it.
    static let contentSize = CGSize(width: 760, height: 720)
    /// Title bar controls without `.resizable`, so edge dragging and the zoom button are both unavailable.
    static let styleMask: NSWindow.StyleMask = [.titled, .closable, .miniaturizable]
}

extension View {
    /// Pins this view to `SettingsWindowLayout.contentSize` so a hosting window cannot grow or shrink it.
    func fixedSettingsWindowSize() -> some View {
        frame(width: SettingsWindowLayout.contentSize.width, height: SettingsWindowLayout.contentSize.height)
    }
}
```

### [x] 2. Use it in the menu-bar Settings window

In `native/macos/TelaViva/MenuBarController.swift`, `openSettings(tab:)`:

- Replace `.frame(minWidth: 760, minHeight: 560)` (:344) with `.fixedSettingsWindowSize()`.
- Create the window with `NSRect(origin: .zero, size: SettingsWindowLayout.contentSize)` and `styleMask: SettingsWindowLayout.styleMask` (:345-346).
- Replace `created.contentMinSize = NSSize(width: 760, height: 560)` (:348) with `created.contentMinSize = SettingsWindowLayout.contentSize` and `created.contentMaxSize = SettingsWindowLayout.contentSize`, as a second guard in case the style mask is changed later.
- Leave the title, `contentViewController`, `isReleasedWhenClosed`, the close observer and the centering code (:358-364) as they are.

### [x] 3. Use it in the SwiftUI `Settings` scene

In `native/macos/TelaViva/App.swift`:

- Replace `.frame(minWidth: 760, minHeight: 560)` (:50) with `.fixedSettingsWindowSize()`.
- Add `.windowResizability(.contentSize)` on the `Settings { ... }` scene, before `.commands`. The deployment target is macOS 14, which supports it.

### [x] 4. Register the new file in the app build

In `packaging/macos/build-app.sh`, append `"$SRC/SettingsWindowLayout.swift"` to the `SOURCES=(...)` array at :44 (next to `"$SRC/PopupLayout.swift"`). Check `project.pbxproj` as described under Relevant Files.

### [x] 5. Add the test

- `native/macos/Tests/SettingsWindowLayoutTests.swift`, same structure as `PopupLayoutTests.swift` (`expect` helper writing `SettingsWindowLayoutTests: <message>` to stderr and `exit(1)`, a `@main private struct` with a `main()` that prints `Settings window layout tests passed`). Cases:
  - `contentSize` is exactly 760 x 720.
  - `styleMask` does not contain `.resizable`.
  - `styleMask` still contains `.titled`, `.closable` and `.miniaturizable`, so the window keeps its title bar, close and minimize buttons.
- `tools/tests/test-settings-window-layout.sh`, a copy of `test-popup-layout.sh` that compiles `SettingsWindowLayout.swift` and the test into `settings-window-layout-tests`. Make it executable (`chmod +x`).
- `.github/workflows/macos.yml`: add a step running `tools/tests/test-settings-window-layout.sh`, directly after the popup-layout step and in the same format.

### [x] 6. Update the docs

- `ARCHITECTURE.md:164`: change "Settings is a larger, resizable window with General, Keyboard and Automatic brightness tabs" to say it is a fixed-size window (760 x 720 points, not resizable) with those tabs.
- `ARCHITECTURE.md:309`: keep the dated section and replace "It remains resizable." with "It was resizable until 2026-10-23; the window now has a fixed 760 x 720 point size, and tab content that does not fit scrolls." Keep the rest of the paragraph.
- `CHANGELOG.md`, `Unreleased` > `Changed`: "The macOS Settings window is now a fixed size (760 x 720 points) and can no longer be resized; content that does not fit scrolls. Hardware checks are pending."
- Do not edit the older `CHANGELOG.md` lines that describe the window as resizable (history) or ADR 0005 (a past decision record). No new ADR: this is a window-chrome change with no architecture impact.

### [x] 7. Run the Validation Commands

Run every command below, review `git diff`, and confirm only the files listed here changed.

## Validation Commands

- `tools/tests/test-settings-window-layout.sh`: the new test passes.
- `tools/tests/test-popup-layout.sh`: the neighbouring layout test still passes.
- `swiftc -typecheck native/macos/TelaViva/SettingsWindowLayout.swift`: the new file compiles on its own.
- `./scripts/build-macos.sh`: the full app builds with the new source and the changed `MenuBarController.swift` and `App.swift` (this is the only step that type-checks those two files, since no test compiles them).
- `cargo fmt --all -- --check` and `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu`: confirm no Rust regression (no Rust file is changed).
- `grep -rn "minWidth: 760\|\.resizable\]" native/macos/TelaViva`: returns nothing, so no resizable leftovers.
- Manual check on the built app (`open 'build/macos/TelaViva OLED.app'`, then Settings from the popup): the edges and corners show no resize cursor, dragging does nothing, the green zoom button is disabled, the close and minimize buttons work, and all five tabs (General, Keyboard, Automatic brightness, Advanced, About) are usable, scrolling where the content is taller than the window. Do not resize the Chrome window or any other window to test; this is a native window check only.

## Notes

- **Small screens.** A fixed 720-point-tall window plus the title bar is about 748 points. On a display whose visible area is shorter than that (for example a MacBook Air at its most scaled-up setting), the user can no longer shrink the window to fit, as they could before. AppKit constrains a titled window to the visible screen when it is shown, so it is not placed off-screen, but its bottom may be clipped. If the owner wants to avoid this, choose a smaller fixed height (for example 640, as in ADR 0005) in `SettingsWindowLayout.contentSize`; the Forms scroll, so it still works. This plan keeps 720 to avoid changing what the window looks like today.
- **Zoom and full screen.** Without `.resizable`, the green button's zoom and full-screen actions are disabled. This is intended.
- **Remembered size.** The window has no frame autosave name, so no previously saved resizable size can be restored over the fixed one. The window is created once per run, so the change is seen after restarting the app.
- **Windows.** The WinUI Settings window is out of scope (macOS only, per the owner). If the owner wants it locked too, it needs its own change with Windows tests.
- **Hardware and UI acceptance.** As with other Settings layout changes, no automated test renders the window; the manual check above is the evidence, and its result should be recorded in the PR rather than assumed.

## Implementation Notes

- All seven steps are done. Deviation from the plan: `project.pbxproj` does list source files (file reference, build file, group child and sources phase), so `SettingsWindowLayout.swift` was registered there with ids `A10000000000000000000084` (file) and `A10000000000000000000085` (build file), as the plan's conditional check required.
- `ARCHITECTURE.md:309` also had "the same minimum width", which no longer fits a fixed size; it now reads "the same width".
- Verified: `tools/tests/test-settings-window-layout.sh` and `tools/tests/test-popup-layout.sh` pass, `swiftc -typecheck` of the new file passes, `./scripts/build-macos.sh` builds the app (the one Sendable warning at `MenuBarController.swift:354` is existing code), `cargo fmt --all -- --check` passes, and no `minWidth: 760` or `.resizable]` remains in `native/macos/TelaViva`.
- Not done: the manual window check from Validation Commands (no resize cursor, zoom button disabled, all tabs usable). It needs a person at the built app.
