# ADR-0038: Use native slider icons with localized explanatory help

**Status:** Accepted
**Date:** 2026-10-25
**Feature:** monitor-slider-icons-and-tooltips
**Spec:** [Archived chore plan](../specs/archived/monitor-slider-icons-and-tooltips/plan.md)

## Context

The Windows popup used icons for brightness and volume, but a separate Protection brightness heading above a full-width slider. The macOS popup used fixed-width text labels for all three controls. The owner requested a more harmonious layout and tooltips explaining each slider. The change must cover both native shells, both locales and built-in display cards, while preserving the existing control behavior.

## Decision

- Use aligned native sun, speaker and shield icons before the brightness, volume and protection brightness sliders. Windows uses Segoe Fluent Icons; macOS uses SF Symbols. Each card uses a fixed 24-unit icon column, with slider and percentage to its right.
- Replace the protection heading with the shield. Built-in cards use the same brightness and protection presentation and continue to omit volume.
- Store explanatory help in each shell's en-US and pt-BR catalog under `control.brightness.help`, `control.volume.help` and `control.protectionBrightness.help`, following [ADR 0015](0015-native-shells-own-ui-strings.md).
- Attach hover help to passive icons and expose equivalent help on the sliders through UI Automation or SwiftUI accessibility. Keep localized slider names and display identity; decorative icons add no keyboard stops or duplicate accessibility navigation.
- Preserve existing ranges, values, bindings, visibility, unavailable states, protection locking and dispatch paths. No core, ABI, settings, network or dependency change is needed.

## Alternatives Considered

- Keep the protection heading: retains the inconsistent layout the owner asked to change.
- Change Windows alone: would leave macOS without equivalent presentation and explanatory help, contrary to the project's platform scope.
- Rely solely on icon tooltips: would omit explanatory help for keyboard and screen-reader users.
- Add bitmap assets: native system icons provide the required silhouettes without new assets or dependencies.

## Consequences

The popup presents more compact, aligned control rows. The user guide explains the icon meanings and hover behavior. Existing localization tests require the three help keys in both locales on each platform.

The Windows Release build passed with zero warnings and errors, and all 743 native tests passed. Formatting and diff checks passed. Native macOS execution and interactive visual, Narrator, VoiceOver and physical monitor checks remain pending; implementation and archival do not establish those results. The subsequent IDEA diagnostics pass provided no coverage for these file types: its SonarLint helper skipped Swift, C++, XAML and RESW, TypeScript had no applicable files, and no IntelliJ inspections connection was available.

## Non-Obvious Constraints

- Icons remain enabled independently of a disabled slider so their tooltip can still explain the setting.
- Protection brightness sets the target used by presence or screensaver protection; it is distinct from manual brightness. This presentation change must not alter current caps or restoration policy.
- macOS popup sizing remains based on measured card heights; compact rows do not justify a new hardcoded popup height.


## Follow-up — 2026-10-25

[ADR 0039](0039-apply-macos-monitor-slider-icons.md) records the macOS compatibility correction from `accessibilityHelp` to `accessibilityHint`, the passing native build and smoke test, and the validated local installation. It also records the subsequent IntelliJ inspections pass for the two changed Swift files. These results supplement the initial validation status above; interactive visual, accessibility and physical monitor checks remain pending.
