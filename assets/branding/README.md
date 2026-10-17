# TelaViva OLED branding

This directory holds the vector sources for the TelaViva OLED app icon and the
menu-bar presence glyphs. The compiled macOS icon resources are generated from
these sources and committed under `packaging/macos/Resources/`.

## Files

| Path | Description |
| --- | --- |
| `app-icon.svg` | Flattened 1024x1024 master of the app icon, with the rounded-square mask, edge highlight and offsets baked in. Used for previews and as the design reference. |
| `liquid-glass/background.svg` | Full-bleed background layer for the Liquid Glass icon. |
| `liquid-glass/screen.svg` | Screen layer (gradient rounded rectangle). |
| `liquid-glass/stand.svg` | Stand and base layer. |
| `liquid-glass/dot.svg` | Sensor dot layer. |
| `AppIcon.icon/icon.json` | Icon Composer document that groups the four layers. |
| `AppIcon.icon/Assets/*.svg` | Copies of the four layer files, referenced by `icon.json`. Byte-identical to `liquid-glass/`. |
| `menubar/presence-detected.svg` | Menu-bar glyph master for face detected. |
| `menubar/presence-absent.svg` | Menu-bar glyph master for face not detected. |
| `menubar/presence-dark.svg` | Menu-bar glyph master for insufficient light. |

## Regenerate

Run the build script from the repository root:

```sh
sh tools/branding/build-icons.sh
```

The script needs Xcode 26 or later, with `actool` and Icon Composer. It checks
that `liquid-glass/` and `AppIcon.icon/Assets/` are identical, compiles
`AppIcon.icon` with `actool`, and copies the results to
`packaging/macos/Resources/AppIcon.icns` and `packaging/macos/Resources/Assets.car`.

The generated `AppIcon.icns` and `Assets.car` are committed because CI runs on
`macos-14`, which cannot compile `.icon` documents. Regenerate them and commit
the results whenever the artwork changes.

## Liquid Glass notes

- Each layer is a full-bleed 1024x1024 SVG. The layers have no baked-in corner
  mask, margin, shadow or edge highlight.
- On macOS 26 and later, the system applies the icon mask and lighting.
- On macOS 14 and 15, the `.icns` file is the fallback.
- When editing in Icon Composer, keep `liquid-glass/` and
  `AppIcon.icon/Assets/` identical. The build script rejects differences.

## Palette

| Name | Hex |
| --- | --- |
| Black | `#000000` |
| Background top | `#17171B` |
| Amber | `#FFB224` |
| Coral | `#FF4D6A` |
| Violet | `#7B3FF2` |
| Stand grey | `#E9E9EF` |
| White | `#FFFFFF` |

## Menu-bar glyph states

The menu-bar glyphs are template images: black on transparent, on an 18x18 pt
grid.

- Filled dot: face detected.
- No dot: face not detected, or presence is off, suspended or unavailable.
- Ring: insufficient light.

`native/macos/TelaViva/StatusItemIcon.swift` mirrors the SVG geometry. When the
glyph geometry changes, update both files.

## Provenance and license

The artwork is original, designed for this project by the project owner. It is
released under the repository's MIT license (see [../../LICENSE](../../LICENSE)).
It uses no third-party assets or fonts.
