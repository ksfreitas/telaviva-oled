# ADR-0009: Layered Liquid Glass app icon and a core-owned presence status icon

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** app-icons-and-presence-status-icon
**Spec:** [plan](../specs/archived/app-icons-and-presence-status-icon/plan.md)

## Context

The app had no icon: `Info.plist` had neither `CFBundleIconFile` nor `CFBundleIconName`, and the bundle had no icon asset. The status item was the static SF Symbol `display`.

The design was delivered as an HTML mockup with one squircle baked in (824 px inside a 1024 px canvas, with an edge highlight). That does not suit macOS 26 Liquid Glass, where the system applies the mask, lighting and glass itself, so a baked squircle would be masked and lit a second time.

The minimum deployment target is macOS 14, so a flat fallback is still required.

The shell could not tell "face detected" from "no face detected", because `camera.display_status` reports both as `ready`.

CI runs on macos-14 (Xcode 15), which cannot compile Icon Composer `.icon` documents.

## Decision

- Vector sources live in `assets/branding/`: the master `app-icon.svg`, full-bleed Liquid Glass layers in `liquid-glass/`, the Icon Composer document `AppIcon.icon` (whose `Assets/` are byte-identical copies of the layers), and the menu-bar glyph masters in `menubar/`.
- `tools/branding/build-icons.sh` compiles `AppIcon.icon` with `xcrun actool` (minimum deployment target 14.0) into `Assets.car`. It renders the fallback `AppIcon.icns` from `app-icon.svg` at all ten iconset sizes with `tools/branding/render-svg.swift` (AppKit) and `iconutil`. Both outputs are committed under `packaging/macos/Resources/`, and `packaging/macos/build-app.sh` copies them into the bundle.
- `Info.plist` sets `CFBundleIconFile` and `CFBundleIconName` to `AppIcon`. The Xcode project references `AppIcon.icon` as a resource with `ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon`.
- The core owns the latest presence sample as the additive snapshot field `camera.presence`, with the values `present`, `absent`, `insufficient_light` and `null`. It is cleared on every camera generation change, on suspension and on reset.
- The status item is a drawn template `NSImage` in `native/macos/TelaViva/StatusItemIcon.swift`, 18 pt, showing a display outline plus a dot. A filled dot means a face is detected. No dot means not detected, or presence is off, suspended or unavailable. A ring (outlined dot) means insufficient light. The image mirrors the SVG masters and is reassigned only when the state or the locale changes.

## Alternatives Considered

- Use the mockup's baked squircle on every OS: rejected, because on macOS 26 the system would mask and light it a second time.
- Use the `.icns` that actool emits as the fallback: rejected, because it contains only the 16 and 128 px sizes, so Dock and Finder would upscale it and it would look blurry.
- Compile `.icon` in CI: rejected for now, because macos-14 with Xcode 15 cannot. Moving CI to a newer runner would allow it.
- Derive presence in the shell from `PresenceCamera` observations: rejected, because the core already owns generation, suspension and staleness checks.
- SVG or PNG assets for the menu-bar icon: rejected. A drawn template image is resolution independent and has no raster assets. The SVGs stay as editable masters, and a test checks the drawn pixels.
- SF Symbols only: rejected, because no symbol shows a dot outline state next to a display.

## Consequences

- New committed binaries: `Assets.car` (about 2 MB) and `AppIcon.icns` (about 0.6 MB). They must be regenerated whenever the artwork changes, which needs Xcode 26 or later.
- The glyph geometry exists twice, in the SVG masters and in `StatusItemIcon.swift`, and both must be kept in sync.
- A settings or monitor change clears `camera.presence` until the next sample, so the dot disappears briefly.
- The snapshot grows by one field. The schema stays at version 1, and older shells ignore the field.

## Non-Obvious Constraints

- Liquid Glass rendering, Dock and Finder appearance, and the menu-bar look are unverified on real hardware.
- The `.icon` compile was verified with actool 27.1 only. The layer order (first group is topmost) was confirmed by rendering the compiled output.
- `Assets.car` is used from macOS 26; the `.icns` is used before that.
- The Xcode project edit was validated with `plutil -lint` and `xcodebuild -list`, but no Xcode build was run. `scripts/build-macos.sh` is the verified build path.
- Presence is shown per latest sample, not with the two-miss hysteresis used for dimming.
- The artwork is original work of the project owner under the repository's MIT license (to be confirmed).
