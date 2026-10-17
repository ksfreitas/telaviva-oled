# ADR-0010: Pause MacBook brightness follow while the built-in display is off

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** pause-follow-when-built-in-display-is-off
**Spec:** [Archived chore plan](../specs/archived/pause-follow-when-built-in-display-is-off/spec.md)

## Context

[ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) lets external monitors follow the built-in display. When the MacBook lid is closed or its built-in screen is off, there is no screen to follow and the user needs manual control of the external monitors back.

Before this change, only part of that held. With the lid closed and an external display connected, the built-in display leaves the online list or goes inactive, so `DisplayServicesBackend.status()` already reported `available: false` and follow stopped. The Settings message could then say the Mac has no built-in display, which is wrong on a MacBook. With the lid open and the screen switched off (display sleep), the display stayed online and active, so `status()` stayed available and follow stayed active: the popup kept hiding the brightness slider and the brightness keys kept passing through to macOS for a screen nobody could see. Availability was also refreshed only on screen-parameter changes and system wake, so a display that slept or woke without a reconfiguration went unnoticed.

## Decision

A closed lid or a sleeping built-in display is "built-in unavailable" with the reason `displayOffline`, through the existing availability path.

- `DisplayServicesBackend.status()` reads the lid state (`AppleClamshellState` on `IOPMrootDomain`; a missing key, as on a Mac mini, means open) and the display's sleep state, and classifies them with the pure function `offlineReason(builtInFound:lidClosed:online:active:asleep:)`. A closed lid is always `displayOffline`, even when the display is missing from the online list; an open lid with no built-in display stays `noBuiltInDisplay`; a found display that is offline, inactive or asleep is `displayOffline`.
- `MenuBarController` also refreshes availability on `screensDidSleepNotification` and `screensDidWakeNotification`, and once more about 2 seconds after a display wake, because the display can still read asleep when the notification arrives. There is no new timer or polling.
- The Rust core is unchanged. `builtin_display { available: false }` already clears the follow target, sets `ambient.active` to false and leaves each monitor at its last value. The popup then shows the brightness slider again, and the key router's pass-through set becomes empty, so manual control returns. When availability returns, the core recomputes the target from the current built-in level and writes the mapped value, replacing any manual change made during the pause. `follow_pauses_while_builtin_is_unavailable_and_resumes_with_the_current_level` covers this sequence.
- The `ambient.unavailable.displayOffline` message (en-US and pt-BR) now also covers a switched-off screen and says that following resumes on its own.

## Alternatives Considered

The plan recorded no design alternative beyond the one chosen. It states two constraints that shaped it: no new polling for availability, and no core policy change inside this chore (if the core had not behaved as described, the work was to stop and report it).

## Consequences

Following pauses whenever all displays sleep, because the built-in display sleeps with them. It resumes on wake with a fresh read, and the core already withholds writes to sleeping monitors. While paused, calibration ends through the existing built-in-unavailable and display-sleep paths.

`status()` now reads the IORegistry on every call, and `adjustableDisplayID()` calls `status()`, so each built-in level read or write does one extra registry lookup. The level is polled once per second, and only while following is on.

Hardware behavior is not verified: whether `AppleClamshellState` and `CGDisplayIsAsleep` change as expected on Apple Silicon, including the clamshell case where the built-in display is removed from the online list, still needs a MacBook with an external monitor. The checks are section I of the [hardware checklist](../specs/archived/macbook-brightness-follow/validation/hardware-checklist.md) and are not run.

## Non-Obvious Constraints

- A manual brightness set during the pause is replaced when following resumes. This is intended and is stated in the README.
- `offlineReason` is internal and static on `DisplayServicesBackend` so that `tools/tests/test-builtin-brightness-monitor.sh`, which compiles the backend file with the monitor, can call it without IOKit or a real display.
- With the lid closed and no external display, macOS sleeps the whole system; the existing system-wake observer refreshes availability then.
- The 2-second delayed refresh after a display wake is harmless after shutdown: `BuiltInBrightnessMonitor.refreshAvailability()` returns at once unless the monitor is started.
