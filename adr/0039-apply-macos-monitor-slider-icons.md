# ADR-0039: Use SwiftUI accessibility hints for macOS monitor sliders

**Status:** Accepted
**Date:** 2026-10-25
**Feature:** apply-macos-monitor-slider-icons
**Spec:** [Archived chore plan](../specs/archived/apply-macos-monitor-slider-icons/plan.md)

## Context

[ADR 0038](0038-monitor-slider-icons-and-tooltips.md) introduced native slider icons and localized help on both platforms. The macOS build failed because four slider modifiers used `accessibilityHelp`, which SwiftUI does not provide. The owner requested applying the icons to the installed macOS app; Windows presentation was already implemented.

## Decision

Use `accessibilityHint` for the four explanatory slider strings in `MonitorPopup.swift` and `BuiltInCardView.swift`. Preserve localized accessibility labels, the existing passive icon `.help` tooltips and SF Symbols layout. Build through `scripts/build-macos.sh`, validate the packaged app before installation, back up the existing bundle, quit normally and reopen the updated application.

This complements ADR 0038 without changing its icon design, control bindings, ranges or dispatch behavior. The change is scoped to macOS and adds no core policy, ABI, dependency, setting or network behavior.

## Alternatives Considered

- Retain `accessibilityHelp`: leaves the macOS app unable to compile.
- Remove explanatory slider help: loses the accessibility behavior required by ADR 0038.
- Reimplement the icon layout: unnecessary because the existing source already contains the selected SF Symbols and localized tooltips.

## Consequences

The standard Apple Silicon build and packaged smoke test passed, as did popup layout and camera/localization tests. Both packaged and installed signatures verified. The installed executable matched the build byte for byte and the app was reopened from `/Applications/TelaViva OLED.app`. This local installation does not change the application version or constitute a release.

IntelliJ inspections returned no findings for the two changed Swift files. The headless SonarLint helper skipped Swift; TypeScript had no applicable files. A preexisting `Sendable` warning in `MenuBarController.swift` remains outside this correction. Interactive visual, tooltip hover, VoiceOver and physical monitor checks remain pending; build and inspection results do not establish those outcomes.

## Non-Obvious Constraints

- Accessible slider names include display identity; passive icons remain excluded from accessibility navigation to avoid duplicate stops.
- Icon tooltips remain available independently of a disabled slider.
- Quit normally before replacing the installed bundle so brightness restoration hooks can run. Retain the previous bundle for rollback; saved preferences are outside the bundle.
