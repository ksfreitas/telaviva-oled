# Feature: App icons, Liquid Glass icon and presence status icon

## Feature Description

Give TelaViva OLED a real identity on macOS and keep the vector sources in the repository:

1. The application icon from the design bundle `~/Desktop/TelaViva OLED Icons` (black squircle, warm gradient screen, white dot meaning "presence detected"), prepared for macOS 26 Liquid Glass (layered Icon Composer document) with a flat fallback for the macOS 14 deployment target.
2. The vector masters of the icon (full artwork, Liquid Glass layers, menu-bar glyphs) under `assets/branding/`, with a script that regenerates every derived file.
3. A menu-bar (status item) icon that reflects presence: dot filled when a face is detected, no dot when no face is detected, dot outline when the room is too dark.

## User Story

As a TelaViva OLED user, I want the app to have its own icon, including a menu-bar icon that shows whether it currently sees me, so that I can recognize the app and understand its state at a glance, and as a maintainer I want the vector sources in the repository so that I can edit the icon later.

## Problem Statement

- The app has no icon (`Info.plist` has no `CFBundleIconFile` or `CFBundleIconName`; the repository has no icon asset of any kind).
- The status item uses the generic SF Symbol `display` and never changes (`MenuBarController.swift:77`); presence states only appear as text in the popup and Settings.
- The core does not expose the last presence sample to the shell. `camera.display_status` collapses detected and not detected into `ready`, so the shell cannot draw the requested states from the snapshot today.
- The design bundle is an HTML mockup with a single baked-in squircle (824 px inside a 1024 px canvas). Liquid Glass needs a full-bleed, layered source; a baked mask and highlight would be applied twice.

## Solution Statement

- **Vector sources** in `assets/branding/`, extracted from the mockup's `<symbol id="tv-icon">` into plain SVG, plus Liquid Glass layer SVGs and menu-bar glyph SVGs.
- **App icon**: an Icon Composer document `assets/branding/AppIcon.icon` (background fill + screen layer + dot layer, glass on the foreground layers) compiled with `actool` into `Assets.car` and an `AppIcon.icns` fallback. Both are committed under `packaging/macos/Resources/` because the CI runner (macos-14, Xcode 15) cannot compile `.icon`; `build-app.sh` only copies them. `Info.plist` gets `CFBundleIconFile` and `CFBundleIconName`.
- **Presence field in the core** (additive, schema unchanged): `camera.presence` is `present`, `absent`, `insufficient_light` or `null`. The core already owns generation checks, suspension and staleness, so the shell stays a pure renderer. This is cheaper and safer than recomputing presence in `PresenceCamera`.
- **Status item**: a vector template image drawn with `NSBezierPath` (monitor outline plus the dot), rebuilt only when the state changes. Template rendering keeps it correct in light, dark and tinted menu bars, and a drawn image is resolution independent with no raster assets. The SVG glyphs in `assets/branding/menubar/` are the editable masters and the drawing code mirrors their geometry; a test renders the three states and checks the pixels.

## Relevant Files

- `native/macos/TelaViva/MenuBarController.swift` — creates the status item (line ~77) and receives every snapshot (line ~91); will set the icon from `snapshot.camera?.presence`.
- `native/macos/TelaViva/CoreBridge.swift:313` — `CameraSnapshot` decodable; add `presence`.
- `native/macos/TelaViva/Localization.swift` — add accessibility strings for the three states (en-US and pt-BR).
- `native/macos/TelaViva/Info.plist` — add icon keys.
- `native/macos/TelaViva.xcodeproj/project.pbxproj` — Resources phase `A10000000000000000000042` is empty; add the icon resources and `ASSETCATALOG_COMPILER_APPICON_NAME`, and register the new Swift files (the Sources phase lists them).
- `packaging/macos/build-app.sh` — hardcoded `SOURCES` list (add the new Swift files) and a copy step for the icon resources.
- `crates/telaviva-core/src/core.rs` — `presence()` (line ~609), mode resets (lines ~587-597, ~719) and the snapshot builder (line ~1246).
- `crates/telaviva-core/tests/core_contract.rs`, `ffi_contract.rs` — contract tests for `camera.presence`.
- `api.md`, `README.md`, `CHANGELOG.md`, `ARCHITECTURE.md` — document the field, the layout (`assets/branding/`) and the feature.
- `.github/workflows/macos.yml` and the test list in `README.md` — register the new test scripts.
- `/Users/cassio/Desktop/TelaViva OLED Icons/Main.dc.html` — the design source (symbol `tv-icon`; palette `#000000`, `#FFB224`, `#FF4D6A`, `#7B3FF2`, neutrals `#17171b`, `#E9E9EF`).

### New Files

- `assets/branding/README.md` — what each file is, how to regenerate, provenance and license of the artwork.
- `assets/branding/app-icon.svg` — master artwork exactly as designed (1024 canvas, 100 px margin, squircle, edge highlight).
- `assets/branding/liquid-glass/background.svg`, `screen.svg`, `dot.svg` — full-bleed 1024×1024 layers: background has no corner radius, no margin and no edge stroke (the system masks and highlights it); screen and dot are separate transparent layers so the glass can separate them.
- `assets/branding/AppIcon.icon/` — Icon Composer document (`icon.json` + `Assets/`), openable and editable in Icon Composer.
- `assets/branding/menubar/presence-detected.svg`, `presence-absent.svg`, `presence-dark.svg` — template glyph masters (18 pt grid).
- `tools/branding/build-icons.sh` — regenerates `AppIcon.icns` and `Assets.car` from `AppIcon.icon` with `actool`/`iconutil`, plus PNG previews; documents the local tools it needs (Xcode 26+).
- `packaging/macos/Resources/AppIcon.icns`, `Assets.car` — generated, committed.
- `native/macos/TelaViva/StatusItemIcon.swift` — presence-to-state mapping and the template image renderer.
- `native/macos/Tests/StatusItemIconTests.swift`, `tools/tests/test-status-item-icon.sh`.
- `tools/tests/test-branding-assets.sh` — checks SVG well-formedness, `icon.json`, generated resources and `Info.plist` keys.

## Implementation Plan

### Phase 1: Foundation

Extract the symbol into `app-icon.svg` and verify it renders identically to the mockup. Derive the Liquid Glass layers and the menu-bar glyph SVGs. Spike (blocking, small): compile a minimal `.icon` with `actool` 27.1 from the shell and confirm it produces `Assets.car` and an `.icns` fallback and that the deployment target 14.0 is accepted. If `actool` cannot produce the fallback, build the `.icns` from the legacy `app-icon.svg` with `iconutil` instead.

### Phase 2: Core Implementation

Add `camera.presence` to the core with Rust tests first. Write `StatusItemIcon.swift` (state enum, renderer, accessibility text) with its tests. Generate the icon resources and wire them into `Info.plist`, `build-app.sh` and the Xcode project.

### Phase 3: Integration

Hook the status item to the snapshot, update docs and CI registration, run the full validation, and inspect the result visually (rendered previews of the app icon at 16–1024 and of the three menu-bar states on light and dark backgrounds).

## Step by Step Tasks

### 1. Extract the vector master
- Write `assets/branding/app-icon.svg` from the mockup symbol (copy values verbatim; keep gradient ids unique). Render with `magick`/`inkscape` and compare against the mockup screenshot taken from `Main.dc.html` served locally.

### 2. Liquid Glass layers
- Create the three layer SVGs at 1024×1024: `background.svg` (gradient `#17171b`→`#000`, square, full-bleed), `screen.svg` (the gradient rounded rect and the neck/base, positioned as in the master but without the 100 px margin offset so the artwork stays centered and inside the safe zone), `dot.svg` (white circle). Keep the screen artwork within the central ~70% so the system mask never crops it.
- Keep the master gradient values; do not bake shadows, highlights or rounded corners into any layer.

### 3. Icon Composer document and compile spike
- Create `AppIcon.icon/icon.json` and `Assets/` with the layers (background as `fill`/image layer, screen and dot as glass layers, `supported-platforms` macOS).
- Spike: `xcrun actool --compile <tmp> --platform macosx --minimum-deployment-target 14.0 --app-icon AppIcon --output-partial-info-plist <tmp>/p.plist assets/branding/AppIcon.icon`. Record the exact working command in `tools/branding/build-icons.sh`. Confirm `Assets.car` and `AppIcon.icns` appear. If the document does not open or compile, fix `icon.json` before continuing.

### 4. Build script and generated resources
- Write `tools/branding/build-icons.sh` (POSIX sh, works from any cwd, fails clearly if Xcode 26+ `actool` is missing). It emits `packaging/macos/Resources/AppIcon.icns` and `Assets.car`, and previews under `/tmp` only.
- Run it and commit the outputs. Verify the `.icns` with `iconutil -c iconset` (sizes 16–512 @1x/@2x present).

### 5. Core: presence field (tests first)
- In `core_contract.rs` add tests: `camera.presence` is `null` when disabled or suspended; `present` after a present sample; `absent` after an absent sample in face mode; `insufficient_light` after a light-only sample and while the mode is `light_only`; cleared by a new `camera_generation`, by suspension and by mode change; ignored for stale or out-of-order samples (existing guards).
- In `core.rs` add `last_presence: Option<&'static str>` set in `presence()` after the existing validation (`present`/`absent` set it, `insufficient_light` and `unavailable` clear it), cleared where `camera_status` resets to `unknown` (lines ~596 and ~719) and on suspend/disable. Emit `"presence"` in the `camera` object using the mode rules from the Solution Statement. Do not change `display_status`, hysteresis or dimming.
- Add an FFI contract test that the field reaches the snapshot JSON through the C ABI.

### 6. Shell: state mapping and renderer
- Add `presence: String?` to `CameraSnapshot`.
- `StatusItemIcon.swift`: `enum PresenceIconState { detected, absent, dark }` with `init(snapshot:)` (unknown, disabled, suspended, unavailable and `null` map to `absent`, no dot), `image(for:)` returning a template `NSImage` (18 pt, monitor outline, stand, dot filled / none / ring) drawn with `NSBezierPath` and cached per state, and `accessibilityDescription(for:locale:)`.
- Localization: `statusItem.presence.detected`, `statusItem.presence.absent`, `statusItem.presence.dark` in en-US and pt-BR.
- Tests in `StatusItemIconTests.swift`: mapping table for every `presence` value and nil; `isTemplate` is true; rendering to a bitmap shows opaque alpha at the dot center for `detected`, transparent center and opaque ring for `dark`, transparent center for `absent`; the three images differ only in the dot region; image size is 18 pt tall.
- `tools/tests/test-status-item-icon.sh` following the pattern of `test-popup-layout.sh`.

### 7. Shell: wire the status item
- `MenuBarController`: replace the static SF Symbol with `StatusItemIcon.image(for: .absent)` at launch, and in the snapshot sink update `statusItem.button?.image` and `accessibilityDescription` only when the state changes. Keep the sink free of extra work when unchanged.

### 8. Bundle wiring
- `Info.plist`: `CFBundleIconFile = AppIcon`, `CFBundleIconName = AppIcon`.
- `build-app.sh`: add `StatusItemIcon.swift` to `SOURCES` and copy `AppIcon.icns` and `Assets.car` into `Contents/Resources`.
- `project.pbxproj`: register the new Swift files and the icon resources, set `ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon`, keeping deployment target 14.0.

### 9. Branding assets test
- `tools/tests/test-branding-assets.sh`: `xmllint --noout` on every SVG, `icon.json` parses and references existing layer files, `AppIcon.icns` and `Assets.car` exist and `iconutil` lists the expected sizes, and `Info.plist` has both keys.
- Register both new scripts in `.github/workflows/macos.yml` and the README test list; `chmod +x`.

### 10. Docs
- `assets/branding/README.md` (files, regeneration command, tool requirements, Liquid Glass notes, provenance and license).
- `api.md`: `camera.presence` values and semantics. `ARCHITECTURE.md`: short section. `README.md`: project layout (`assets/branding/`) and a one-line note that the menu-bar icon shows presence. `CHANGELOG.md`: Added entries; hardware/visual checks stay "pending acceptance" until the user confirms them on a real menu bar and Dock.

### 11. Run Validation Commands
- Run every command below, then render the previews and look at them (app icon sizes, three menu-bar states on light and dark backgrounds).

## Testing Strategy

### Unit Tests
- Rust: presence field transitions and guards; Swift: state mapping, template flag, per-state pixel checks, localization keys present in both languages.

### Integration Tests
- FFI snapshot contains `camera.presence`; `build-app.sh` produces a bundle whose `Info.plist` icon keys resolve to files inside `Contents/Resources`; `codesign --verify --deep --strict` still passes with the new resources.

### Edge Cases
- Camera permission denied, camera busy or unavailable, battery or display-sleep suspension, presence disabled: no dot.
- Single-frame mode: the camera is off between checks; the icon keeps the last known state until the next sample or until the mode changes to suspended/disabled.
- Mode flips between `face` and `light_only` (dark room): ring shows while light is insufficient and the dot returns after usable light.
- Rapid snapshots: the image is only reassigned on change.
- Menu bar tinted by wallpaper, light and dark appearance, increased contrast: template rendering keeps the glyph legible.
- Old downgrade or snapshots without `camera.presence`: decode as `nil`, icon shows no dot.

## Acceptance Criteria

- `assets/branding/` holds an editable SVG master, the Liquid Glass layer SVGs, the Icon Composer document and the three menu-bar SVG glyphs; `tools/branding/build-icons.sh` regenerates `AppIcon.icns` and `Assets.car` from them.
- The built `TelaViva OLED.app` has `CFBundleIconFile`/`CFBundleIconName` and both resources in `Contents/Resources`; the Finder and Dock show the new icon (Liquid Glass on macOS 26 and later, flat squircle fallback on macOS 14 and 15).
- The menu-bar icon shows a filled dot when a face is detected, no dot when no face is detected (and when presence is disabled or suspended), and a dot outline when light is insufficient.
- `camera.presence` is documented in `api.md` and covered by Rust and FFI tests; schema version and existing snapshot fields are unchanged.
- All validation commands pass; `cargo clippy -D warnings` is clean.
- Visual results on a real menu bar and Dock are recorded as pending acceptance until the user confirms them.

## Validation Commands

```bash
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo build --release --target aarch64-apple-darwin -p telaviva-core
for t in tools/tests/test-*.sh; do sh "$t" || exit 1; done
sh tools/branding/build-icons.sh
sh tools/tests/test-branding-assets.sh
scripts/build-macos.sh
plutil -p "dist/TelaViva OLED.app/Contents/Info.plist" | grep -i CFBundleIcon
ls "dist/TelaViva OLED.app/Contents/Resources"
codesign --verify --deep --strict "dist/TelaViva OLED.app"
```

(Adjust `dist/` to the output directory printed by `scripts/build-macos.sh`.)

## Clarification Needed

1. "O ícone do relógio" — the code has no clock icon; the only status icon is the menu-bar monitor glyph, which sits next to the macOS clock. **Default**: treat it as the menu-bar status icon.
2. States without a detection result (presence disabled, suspended on battery or display sleep, camera unavailable or permission missing, first sample pending). **Default**: same as "not detected" (no dot), because the request defines only three states; the existing popup text still explains why.
3. Dot in the menu bar for the "detected" state: filled disc versus the design's white dot. **Default**: filled template disc (the system tints it; it cannot be white-on-black in a template).
4. Where the vector sources live. **Default**: `assets/branding/`, next to `assets/model/`, with generated binaries in `packaging/macos/Resources/`.
5. License and provenance of the artwork (AGENTS.md requires open-source redistributable assets). **Default**: documented in `assets/branding/README.md` as original artwork of the project owner, released under the repository's MIT license; confirm.
6. Single-frame mode keeps the last sample on the icon between checks. **Default**: yes, with the icon cleared on suspension or when presence is disabled.
7. Whether the original mockup (`Main.dc.html` plus its runtime) should be archived. **Default**: no; the extracted SVG is the source of truth.

## Notes

- Nothing here is hardware-verified: Liquid Glass rendering, the `.icon` compile on Xcode 27.1, the Dock and Finder appearance, and the menu-bar look are checked only through `actool` output and rendered previews until the user confirms them.
- Icon Composer `.icon` documents are only honored on macOS 26 and later; the deployment target stays 14.0, so the `.icns` fallback is required, not optional.
- CI builds on macos-14 (Xcode 15), which cannot compile `.icon`; that is why generated resources are committed and `build-app.sh` does not call `actool`. Moving CI to a newer runner would allow compiling at build time instead.
- `camera.presence` is additive; the core C ABI and `settings.json` schema stay version 1. Older shells ignore the field.
- No new dependencies. Rasterizing uses tools already on the machine (`inkscape`/`magick`, `actool`, `iconutil`); they are only needed to regenerate assets, not to build the app.
- The shell source list in `build-app.sh` is hardcoded; any new Swift file must be added there and to the Xcode project, as in earlier features.

## Implementation notes (2026-10-16)

- The compile spike passed: actool 27.1 accepts `AppIcon.icon` with deployment target 14.0 and emits `Assets.car`. Layer order, top to bottom, is dot, stand, screen, background.
- Deviation: actool's `.icns` contains only the 16 and 128 px sizes. The fallback `AppIcon.icns` is therefore rendered from `app-icon.svg` with an AppKit renderer (`tools/branding/render-svg.swift`) and `iconutil`; only `Assets.car` comes from actool.
- The layer SVGs include a separate `stand.svg` layer, so there are four layers, not three.
- `camera.presence` is cleared on every camera generation change, including settings changes. This is the documented limitation.
- Clarification questions: the defaults were kept for the menu-bar icon interpretation, for no dot when presence is disabled, suspended or unavailable, and for keeping the last sample in single-frame mode.
- Validation run: `cargo fmt`, `cargo clippy` and `cargo test`; all `tools/tests` scripts; the `scripts/build-macos.sh` bundle check (Info.plist keys, resources, `codesign --verify`); and rendered previews. No Xcode build and no hardware check were run.
