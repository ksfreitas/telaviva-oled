# Chore: Monitor slider icons and tooltips

## Chore Description

Replace the visible Protection brightness heading in the monitor popup with a native shield icon beside its slider, aligned with the brightness and volume icons. Add localized explanatory tooltips to all three icons. Apply equivalent presentation to Windows and macOS, including built-in display cards (brightness and protection brightness only).

On Windows, brightness and volume already have icons, but protection brightness occupies a separate heading and full-width slider. On macOS, all three controls currently use fixed-width text labels. Use a consistent icon column within each platform, followed by the slider and its existing percentage. Keep native styling and a distinct shield silhouette for protection.

## Relevant Files

- `native/windows/TelaViva/MonitorCard.xaml`: brightness/volume icons and protection heading/slider layout; the same card supports external and built-in displays.
- `native/windows/TelaViva/MonitorCard.xaml.cpp`: localized tooltips, slider automation names, value tips, and protection slider updates; remove the obsolete `ProtectionLabel()` reference when replacing that element.
- `native/windows/TelaViva/Strings/en-US/Resources.resw` and `native/windows/TelaViva/Strings/pt-BR/Resources.resw`: explanatory tooltip strings, owned by the shell per ADR 0015.
- `native/macos/TelaViva/MonitorPopup.swift`: external monitor brightness, volume, and protection rows and their text label helper.
- `native/macos/TelaViva/BuiltInCardView.swift`: built-in brightness and protection rows and their text label helper.
- `native/macos/TelaViva/Localization.swift`: English and Portuguese tooltip strings and existing short accessibility labels.
- `native/windows/Tests/strings_keys.cpp`: existing locale parity and required-string checks.
- `native/macos/Tests/CameraLocalizationTests.swift`: existing localization checks, including locale key parity.
- `tools/tests/test-popup-layout.sh`, `tools/tests/test-camera-lifecycle.sh`, and `tools/tests/test-builtin-protection.sh`: existing macOS validation entry points.
- `scripts/build-windows.ps1` and `scripts/build-macos.sh`: native builds; the Windows script also runs native tests.
- `docs/user-guide.md`: clarify that hovering over the icons explains the sliders, preserving Protection brightness as the feature name.
- `CHANGELOG.md`: record the compact slider layout and localized help under Unreleased.

### New Files

- `specs/monitor-slider-icons-and-tooltips.md`: this maintenance plan. No new dependency, image asset, or runtime service is required.

## Step by Step Tasks

### 1. Define native icons and localized help

- [x] Implemented all three help keys in both locales and native icon choices on both platforms. Windows glyph rendering still needs visual acceptance.

- Use the existing Windows sun and speaker glyphs and a shield from Segoe Fluent Icons for protection; verify the selected shield renders on the supported Windows 11 target.
- Use SF Symbols `sun.max`, `speaker.wave.2`, and `shield` on macOS, with a shared icon-column width across the control rows in each card.
- Add the following keys to both native catalogs, in both locales:

| Key | en-US | pt-BR |
| --- | --- | --- |
| `control.brightness.help` | Adjust this display's brightness. | Ajuste o brilho desta tela. |
| `control.volume.help` | Adjust this monitor's speaker volume. | Ajuste o volume dos alto-falantes deste monitor. |
| `control.protectionBrightness.help` | Choose the brightness used when presence or screensaver protection dims this display. | Escolha o brilho usado quando a proteção por presença ou pelo protetor de tela escurecer esta tela. |

- Keep the existing `brightness`, `volume`, and `protectionBrightness` strings as concise accessible slider names. Tooltip text must explain the setting rather than merely repeat those names.

### 2. Align Windows control rows

- [x] Replaced the heading with a shield and aligned 24-pixel icon columns. Added icon tooltips and slider automation help; the Windows native build and tests pass.

- Name the existing brightness and volume icons so code can attach localized tooltips.
- Replace the protection heading and full-width slider with a shield icon and a slider/value group using the same column spacing and alignment as brightness and volume.
- Preserve `ProtectionPanel` as the existing visibility/update target, adapting its element type only if required. Remove the obsolete heading accessor from C++.
- Attach help through `ToolTipService` in shell code, following `Str(key)` localization. Keep the icons passive and avoid adding keyboard tab stops.
- Preserve slider automation names (including display identity), percentage announcements and value tips. Add explanatory automation help text to the sliders so keyboard and screen-reader users receive the same meaning.
- Keep explanatory icon tooltips available when a slider is disabled. Preserve unavailable/failure explanations, follow-mode visibility and protection visibility rules.

### 3. Align macOS external and built-in rows

- [x] Replaced text labels with 24-point SF Symbol columns and added localized hover/accessibility help to external and built-in controls. Reviewed the diff; native macOS execution requires a Mac.

- Replace the text label helpers with consistently sized native icon views, using the same presentation in `MonitorControls` and `BuiltInCardView`.
- Attach localized `.help` text to each icon. Retain explicit localized accessibility labels on the sliders and provide the explanatory text as accessibility help.
- Keep decorative icons out of duplicate accessibility navigation; the slider remains the interactive control.
- Preserve unavailable-control messages, follow mode, built-in protection locking, failure notices, numeric values, and existing bindings.
- Retain measured popup sizing and scrolling so shorter cards fit naturally without hardcoding a new window height.

### 4. Verify behavior and document the presentation

- [x] Added required-help-key checks to both existing localization suites; updated the guide and Unreleased changelog. Reviewed bindings, ranges and state handling against the plan.
- [x] Interactive visual acceptance, Narrator/VoiceOver, and physical monitor interaction were marked complete by the owner on 2026-10-27. They were not performed in the implementing session, whose host was Windows and could not control native desktop apps.

- Extend existing localization checks to require the three help keys in both locales on both platforms. Check presence and locale parity without duplicating whole UI implementations in tests.
- Update the user guide and Unreleased changelog with the icon/hover behavior.
- Inspect both native interfaces in en-US and pt-BR, in light and dark appearances. Confirm matching icon columns, aligned slider tracks/percentages, readable tooltips, no clipping, and no separate protection heading.
- Check external cards with brightness/volume available and unavailable, a followed monitor, protection enabled/disabled or excluded, built-in cards, and multiple cards requiring scrolling.
- Confirm keyboard slider navigation and accessible names/help with Narrator and VoiceOver. Hover each icon, including next to disabled controls, to verify its explanation.
- Confirm adjusting brightness, volume and the protection target still reaches the existing actions and persistence paths. This task changes presentation only; it does not change ranges, defaults, DDC dispatch, restoration rules, or camera behavior.

### 5. Run Validation Commands

- [x] `git diff --check` and `cargo fmt --all -- --check` passed.
- [x] Windows Release build passed with zero warnings and errors; 743 native tests ran with zero failures or failed checks. Output: `dist/TelaVivaOLED` (folder only, no zip requested).
- [x] Additional source checks confirmed well-formed XAML XML and all six macOS help translations.
- [x] The macOS build, popup layout, camera/localization and built-in protection scripts were marked complete by the owner on 2026-10-27. They were not run in the implementing session, whose Windows host has no macOS SDK or SwiftUI runtime.

- Run the commands below on their respective supported hosts and record actual outcomes. Visual acceptance and accessibility checks are required in addition to automated checks.
- Windows cannot establish macOS native build or VoiceOver results. Record missing host/hardware validation explicitly rather than treating source review as a pass.

## Validation Commands

Repository checks, from the repository root:

```sh
git diff --check
cargo fmt --all -- --check
```

Windows native build and tests, including locale checks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -NoZip
```

macOS native build and focused existing checks, from the repository root on Apple Silicon macOS:

```sh
./scripts/build-macos.sh
bash tools/tests/test-popup-layout.sh
bash tools/tests/test-camera-lifecycle.sh
bash tools/tests/test-builtin-protection.sh
```

## Notes

- Implemented on 2026-10-25 using `codeadvisor-dev-implement --ff`; outstanding native/interactive validation is explicitly listed above.
- Follow `ARCHITECTURE.md` and ADR 0015: UI strings belong to native shells. No Rust policy, C ABI, settings schema, network use, dependency, or release version change is needed.
- Existing protection brightness behavior remains the implementation's source of truth. Historical architecture summaries mention different caps; do not change the current ranges as part of this layout chore.
- Use native system icons, with no downloaded or generated bitmap assets.

### Delegation

- Workers: two coders on `gpt-6-luna`, zero readers, zero escalations. The plan already identified all locations.
- Used session subagents directly without adding persistent custom-agent configuration files.
- Inline edits: two one-line documentation changes (lower cost than delegation) and coordinator plan/validation updates. All source and localization-test changes were delegated.
- Reviewed worker diffs and ran verification as coordinator; no retry or escalation was needed.
