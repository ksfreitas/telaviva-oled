# Chore: Apply monitor slider icons to the installed macOS app

## Chore Description

Apply the existing SF Symbols slider presentation to the locally installed macOS app: sun for brightness, speaker for volume, and shield for protection brightness. Include external display cards and the built-in display card, which omits volume. Preserve localized icon tooltips, accessible slider labels and hints, numeric values, control ranges and dispatch behavior. The owner explicitly scoped this task to macOS.

The source already contains the icons from ADR 0038. Four invalid `accessibilityHelp` calls prevented packaging; the current working tree replaces them with SwiftUI `accessibilityHint`. Rebuild and install this corrected source so the running application receives the icons.

## Relevant Files

- `native/macos/TelaViva/MonitorPopup.swift`: external monitor icons, tooltips and slider accessibility.
- `native/macos/TelaViva/BuiltInCardView.swift`: built-in brightness and protection icons and slider accessibility.
- `native/macos/TelaViva/Localization.swift`: existing en-US and pt-BR explanatory strings.
- `native/macos/Tests/CameraLocalizationTests.swift`: required localized help checks.
- `scripts/build-macos.sh`: build the current Rust libraries and package the native shell.
- `packaging/macos/build-app.sh`: package resources and apply the local signature.
- `adr/0038-monitor-slider-icons-and-tooltips.md`: existing presentation decision, complemented by ADR 0039 for the macOS compatibility correction.
- `CHANGELOG.md`: record the macOS compilation correction.
- `/Applications/TelaViva OLED.app`: currently running installation to replace after validation.

### New Files

- `specs/archived/apply-macos-monitor-slider-icons/plan.md`: this scoped execution plan and validation record.

## Step by Step Tasks

### 1. Confirm source and installation

- [x] Confirm existing icon rows and localized help on external and built-in cards.
- [x] Identify the running application path and preserve the design from ADR 0038.

Completed: both card views already use SF Symbols in fixed 24-point icon columns, with localized tooltips and accessible slider names. The running installation was `/Applications/TelaViva OLED.app`.

### 2. Package the corrected source

- [x] Use `accessibilityHint` for all four slider help modifiers.
- [x] Build the current Rust libraries and native shell with the standard macOS build helper.
- [x] Keep the current application version for this local installation.

Completed: all four invalid modifiers were corrected; `./scripts/build-macos.sh` passed. The icon implementation was already present, so no duplicate presentation changes were needed.

### 3. Update the local installation

- [x] Pass the packaged smoke test and signature verification before installation.
- [x] Back up the installed bundle under `build/` and quit through the normal termination path.
- [x] Replace and verify the installed bundle, reopen it and confirm the running process path.
- [x] Retain the previous bundle for rollback.

Completed: the validated app was installed and reopened from `/Applications/TelaViva OLED.app`. Its executable matched the build byte for byte. The backup path is recorded below.

### 4. Run Validation Commands

- [x] Run the build, popup layout and camera/localization validation commands below.
- [x] Verify the packaged smoke test, both signatures, installed binary equality and diff formatting.
- [x] Record results and distinguish automated verification from pending manual checks.

Completed: all listed automated checks passed during execution. The preexisting concurrency warning remains. Interactive visual, tooltip hover, VoiceOver and physical DDC checks remain unverified because native desktop control is unavailable in this session; they are not reported as passed.

## Validation Commands

```sh
./scripts/build-macos.sh
tools/tests/test-popup-layout.sh
tools/tests/test-camera-lifecycle.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
codesign --verify --deep --strict '/Applications/TelaViva OLED.app'
cmp 'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' '/Applications/TelaViva OLED.app/Contents/MacOS/TelaViva'
git diff --check
```

## Notes

- Existing localized tooltips and help are reused; no new assets, dependencies, network behavior or settings are introduced.
- The app restarts once during installation. Its saved preferences remain outside the application bundle.
- A preexisting Swift concurrency warning in `MenuBarController.swift` is outside this presentation correction.
- Validation on 2026-10-25: the standard build, popup layout tests, camera/localization script, packaged smoke test, signature verification and diff checks passed. The build emitted the preexisting `Sendable` warning noted above.
- Installed the validated bundle at `/Applications/TelaViva OLED.app` and reopened it. Its executable matched the built executable byte for byte and its signature verified. The previous bundle is retained at `build/macos-installed-backup-20261025-105841/TelaViva OLED.app`.
- Interactive appearance, tooltip hover and VoiceOver checks remain pending because native desktop control is unavailable in this session. Physical monitor behavior was not tested.

- IDEA diagnostics on 2026-10-25: IntelliJ inspections returned no findings for `BuiltInCardView.swift` or `MonitorPopup.swift`. SonarLint skipped Swift and the TypeScript service had no applicable files. No files were opened in the editor.
- Finalization: archived this plan and recorded the compatibility decision and validation boundaries in ADR 0039 and `ARCHITECTURE.md`. No public contract changed.
