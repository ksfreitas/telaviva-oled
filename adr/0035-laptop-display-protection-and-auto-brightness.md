# ADR-0035: Protect the laptop built-in display and hold automatic brightness during protection

**Status:** Accepted
**Date:** 2026-10-21
**Feature:** laptop-display-protection-and-auto-brightness
**Spec:** [Archived spec](../specs/archived/laptop-display-protection-and-auto-brightness/spec.md)

## Context

The built-in screen of a MacBook is outside the protection model. Both shells exclude it from DDC discovery, so the core's `monitors` map holds only external monitors, and presence absence and the screensaver never dim it. The system's automatic brightness setting is touched only by calibration. If it stays on while the app dims the screen, the system raises the level again and undoes the protection.

Routing the built-in screen through the monitor path does not fit. The controls handle maps every brightness action to VCP 0x10 and accepts only four attempt error names (`none`, `unsupported`, `io`, `disconnected`); any other name is an invalid result. Its readback needs an exact integer match. Every attempt pays the DDC settle, wake grace and global exec slot. Sticky ownership with read-before-write would read automatic-brightness drift as a manual change and skip the first dim. A monitor entry would also join follow eligibility, camera gating and the Windows monitor lists.

Before this change, protection kept no persisted state, so a crash while dimmed made the next launch take the dimmed reading as the baseline. Both private macOS setters (DisplayServices for the level, CoreBrightness for automatic brightness) are unverified on hardware, as [ADR 0007](0007-private-display-apis-for-built-in-brightness.md) records.

The owner set the scope on 2026-10-20 and 2026-10-21: controls without volume, an opt-in automatic-brightness hold (off by default), a fixed 1 to 50 percent level per screen, a lid-closed camera rule, and a monitored display that chooses the camera. Windows is deferred by the owner to Windows 11.

## Decision

- **The core decides, the shell applies.** `crates/telaviva-core/src/builtin.rs` and the built-in state in `core.rs` compute the reasons (presence, screensaver), the target percent and the automatic-brightness hold for the reserved id `"builtin"`. The snapshot publishes them as `builtin_protection`, a level-triggered object. The macOS shell reconciles toward it on every snapshot through `BuiltInProtectionController`. The built-in screen is not an entry in `monitors`.
- **Opt-in by the shell.** `builtin_display` accepts optional booleans `protectable` and `holding`, both false when absent. Until a shell sends `protectable`, the core makes no decision for the built-in screen, so Windows behavior is unchanged. `holding` is ignored unless `protectable` is true.
- **Freeze rule.** While the shell holds the screen or any reason is active, the core keeps the last undimmed level as `builtin_percent`. Follow therefore never publishes the dimmed value, external monitors do not move, and follow pauses. The camera brightness gate uses the same undimmed level. Reports are accepted again after the restore.
- **Shell baseline and recovery record.** On the first dim the shell reads the level fresh, writes `builtin-protection-session.json` (`baseline_percent`, unrounded) before any write, and only then reports `holding=true`. A restore writes the baseline exactly, verifies it by readback within one point, deletes the record and reports `holding=false`. A record left by a crash starts the next launch in holding with the stored baseline.
- **One automatic-brightness owner.** `BuiltInAutoBrightnessOwner` has the holders `calibration` and `protection`. The first acquire reads the setting and, when it is on, writes `auto-brightness-hold.json` (`original_enabled: true`) before turning it off. Later holders join without reading. The last release turns the setting on unless a fresh read says it is already on (a nil or off read still turns it on), then deletes the file. A failed restore keeps the file and sets a pending restore, so the next acquire joins the owed restore instead of reading "off" as the user's choice.
- **Option.** `builtin_auto_brightness_off_on_protection` is a core setting, default false. The core publishes `hold_auto_brightness` as true when the option is on and at least one reason is active. With the option off, protection never reads or writes automatic brightness.
- **Calibration and protection never overlap.** The core publishes no reasons while calibrating, and calibration's `begin` refuses with `calibration.notice.protectionActive` while protection holds the screen. Launch recovery of an interrupted calibration restores the built-in level at once and, when its session record says automatic brightness was on, turns it back on with a direct write (a legacy path kept outside the owner). If the monitor is missing from the first snapshot, its restore waits for the monitor to return (90 seconds at most) and the session record stays until then.
- **Protection level.** Each screen has one level, fixed at 1 to 50 percent, that applies to both presence absence and the screensaver. Stored `screensaver_targets` values from 0 to 100 still load and are clamped when applied. A screen without an entry uses 1. Presence and screensaver share the level, so the reasons never stack.
- **Monitored display.** `monitored_display_id` (Automatic when null) and a read-only `monitored_display_history` (at most 8) choose the display whose brightness decides the camera, remembered like the webcam choice. Automatic picks the external display flagged `primary`, then the first usable external monitor, then the built-in screen. A connected history entry always wins over Automatic. The camera gate uses the first awake display in that order (the gate display), so a sleeping choice falls through to the next one. The Settings picker shows the resolved display. Absence counting and dimming still cover every protected screen.
- **Presence threshold per display.** `presence_min_brightness_by_display` maps a display id to a threshold from 0 to 90 percent. The camera gate compares the gate display's level with that display's entry. A display without an entry uses `presence_min_brightness_percent`, whose valid range is now 0 to 90. The Settings slider edits the monitored display's entry.
- **Lid-closed camera rule (macOS).** While the lid is closed, the built-in camera is left out of the candidates and of the default fallback, as if it were disconnected. The remembered camera history is never rewritten, and the session moves back when the lid opens. The built-in screen reports unavailable while the lid is closed. It stays protectable, and the popup and Settings lists show it only while it is available. A running session on the built-in camera moves to an external camera or reports unavailable, and the camera is re-resolved when the lid opens.
- **Windows deferred.** The Windows design and ordered task list are in `windows-handoff.md`. Nothing is implemented or verified for Windows. The built-in policy stays inert for Windows until a Windows shell opts in, while the shared reconnect restore and whole-unit manual values apply to Windows DDC monitors through the shared core once built (hardware pending).

## Alternatives Considered

- **Pseudo monitor in `monitors` with a built-in flag and a second attempt backend.** Rejected for the coupling described in Context: about ten special cases in `reconcile_all`, camera gating and follow.
- **Shell-only policy.** The shell would watch camera presence and the screensaver itself. Rejected: it repeats the confirmation rules and breaks ADR 0008.
- **Core-owned baseline fed by polling.** Rejected: stale between polls and not crash-safe. A fresh read at dim time and a file record avoid both problems.
- **Dedicated settings for the built-in screen (enabled flag and percent).** Rejected: it duplicates the per-display model. The reserved id `"builtin"` reuses `protection_disabled` and `screensaver_targets`.
- **Refcount inside the backend.** Rejected: the backend is a stateless wrapper shared with the test fakes.
- **Abort the dim when automatic brightness cannot be changed.** Rejected by the owner: dim anyway, show a notice, and restore only what was changed.
- **Dim the built-in screen only when another display runs the camera.** Rejected: a laptop alone would never be protected.
- **A per-screen "counts for the camera" flag, or always excluding the built-in screen.** Rejected in favor of the explicit monitored display choice.
- **Route the built-in slider through the controls handle.** Rejected: its queue, ownership and DDC policy do not apply to the built-in screen.
- **Windows code in this change.** Rejected by the owner.

## Consequences

- The MacBook screen is protected by default, like the external monitors. Protection dims it on absence and on the screensaver at 1 percent by default. The first design used 0 percent; the owner changed the default to 1 percent on 2026-10-20.
- Follow pauses while the built-in screen is dimmed by protection. External monitors keep their last mapped value and do not follow the dimmed screen.
- A laptop with no external monitor runs the camera for the built-in screen, which adds camera use. The steady camera cost is recorded in plan T043 and is not changed here.
- Automatic brightness is changed only when the option is on. A crash while dimmed is recovered at the next launch from the two records.
- The shared core change reaches Windows through the protection level (applied to presence as well as to the screensaver, plan T040), the presence threshold minimum of 0, and the snapshot and settings keys. The Windows slider ranges and the threshold help text need the follow-up tasks in `windows-handoff.md`.
- The owner marked the hardware checks of the private setters, the force-quit recovery and lid transitions complete on macOS on 2026-10-27, with no values recorded. Hardware verification of the per-display thresholds on Windows is still pending. This decision does not claim hardware support beyond what was recorded.

## Non-Obvious Constraints

- The private setters are unverified on hardware. Only their getters have run, on one Apple Silicon Mac. Hardware rows must run before the option is described as working.
- `applicationShouldTerminate` and `applicationWillTerminate` call `restoreNow()`. It makes one synchronous baseline write and releases the owner even when the write fails, and the record then stays for the next launch. A late snapshot cannot dim the screen again after this point.
- Settings use `deny_unknown_fields`. An older build that reads a newer `settings.json` renames it to `.corrupt` and resets it, as already accepted for the earlier additive fields.
- `primary` is sent only inside `monitor_discovered`. A change of the main display reaches the core only when that monitor is discovered again.
- The reserved id `"builtin"` cannot collide with a monitor id. A macOS monitor id is built from vendor, product, serial, EDID UUID, display UUID and the IORegistry location, so it can never equal `builtin`.
- Shell lines for the built-in screen use `DiagnosticLog.core` and so reach the `brightness` category next to the core lines (ADR 0034). The core line `builtin_protection reasons=... target=...` is written only when the set of reasons changes.
- The shell only computes `min(baseline, target)` and the recorded restore value, and never raises the level. Every dim target comes from the core.
- Monitor ids are per port on macOS. The id includes the IORegistry location, which contains a `dispextN@...` segment and can change between ports or docks; whether it also changes after sleep is not verified. Settings keyed by id (`protection_disabled`, `presence_min_brightness_by_display`, `screensaver_targets`, `monitored_display_history`, `ambient_curves`) and the reconnect restore apply only while the id is unchanged. A monitor on another port or dock starts with defaults, a protection switch turned off for it turns back on, and a dim it held is adopted as its baseline. Not verified on hardware.

## Amendment 2026-10-27: Windows implemented

The "Windows deferred" decision above no longer holds. The Windows shell now implements the laptop panel, the automatic-brightness hold, the monitored display, the per-display threshold and the lid-closed camera rule (plan: `specs/archived/windows-laptop-parity/plan.md`; decision record: [ADR 0043](0043-windows-laptop-display-parity.md)). The owner marked the Windows hardware rows W-L4 to W-L10 as passed on 2026-10-27, with no observed values recorded in the archived hardware checklist. The macOS rows A to N are still not run.
