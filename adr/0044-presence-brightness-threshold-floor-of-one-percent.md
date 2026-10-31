# ADR 0044: Presence brightness threshold floor of one percent

- Status: Accepted
- Date: 2026-10-30 (decision made 2026-10-29)
- Feature: masked-face-detection-and-lock-on-absence
- Spec: [Archived spec](../specs/archived/masked-face-detection-and-lock-on-absence/spec.md); the design source is Decision 7 of the [archived architecture](../specs/archived/masked-face-detection-and-lock-on-absence/architecture.md), and the settings rules are in the [archived contracts](../specs/archived/masked-face-detection-and-lock-on-absence/contracts.md).

## Context

[ADR 0011](0011-camera-brightness-activation-threshold.md) added the presence brightness threshold, which decides whether a monitor at its current brightness keeps the camera running. [ADR 0035](0035-laptop-display-protection-and-auto-brightness.md) lowered its minimum from 10 to 0, for the global value and for each display, including the built-in screen (`builtin`). A monitor is at or below the threshold when its level is not above it, and the camera then stays suspended.

This feature sets the minimum to 1. A stored 0 must not make the whole settings file fail validation, because that would reset every setting.

## Decision

- `Settings::validate` accepts 1 to 90 for `presence_min_brightness_percent` and for every entry of `presence_min_brightness_by_display`, including `builtin`. The range was 0 to 90.
- `Settings::load` raises any stored value in [0, 1) to 1 before validation. The file is kept, no backup is written and the value becomes 1.
- Negative, above 90, NaN and wrong-type values keep the existing behavior: validation fails and the file is reset with a backup.
- The comparison stays strictly "above". A monitor at or below the threshold still suspends the camera.
- The macOS slider and the Windows General page control use the same limits. On macOS they are `PresenceThreshold.minimumPercent` and `maximumPercent` (1 and 90) in `CoreBridge.swift`, and the slider uses `1...90`. On Windows they are `kPresenceMinBrightnessMin` and `kPresenceMinBrightnessMax` in `SettingsModel.h`.

## Alternatives

- Validate and reset a stored 0: rejected, because it loses all settings.
- Change the comparison to "at or below" so that 0 keeps working: rejected, because it was not requested and would change the outcome at the lower end.

## Consequences

A user who had 0, globally or for one display including `builtin`, silently gets 1 after upgrading. A monitor at 0 keeps its camera suspended. A monitor at exactly 1 stops counting as above a threshold of 1, so for a user who had 0 the camera is suspended at that level, where it ran before. This is the only user-visible change.

An update with 0 is rejected with `presence brightness threshold out of range` and nothing is stored, as for any other out-of-range value.

## Non-Obvious Constraints

- The raise to 1 happens only in `Settings::load`, before validation. An update that sends 0 is not raised; it is rejected.
- The per-display map is normalized in the same way, so a stored `builtin` of 0 becomes 1 as well.
