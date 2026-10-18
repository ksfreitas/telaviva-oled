# ADR-0005: Configure the hardware-key step per feature and wait for our own writes before rebasing key intent

**Status:** Accepted; the macOS rebase guard and the router's step handling are moved into the core by [ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md)
**Date:** 2026-10-16
**Feature:** hardware-key-step-settings
**Spec:** [Archived hardware-key-step settings spec](../specs/archived/hardware-key-step-settings/spec.md)

## Context

Each brightness or volume hardware-key press moved the external monitor by a fixed 6.25 percentage points (`HardwareKeyPolicy.normalStep`), and Shift+Option moved it by a fixed 1.5625-point fine step. The user found the normal step too coarse. 6.25 is not an integer, so the step could not be expressed by an integer control from 1 to 15.

Separately, pressing the volume or brightness keys quickly made the target jump back: a target of 14 was replaced by 1, and then 4 was replaced by 14. `HardwareKeyRouter.reconcileDisplaysLocked` rebased the key intent to the monitor's last read value, which could still reflect an older write of ours. A DDC write plus readback takes up to about 1.4 s, and `pending` in the core snapshot is briefly false between one hardware result and the next queued action, so the rebase ran while our own write was still settling.

## Decision

(1) `brightness_key_step` and `volume_key_step` are two independent core settings: integers from 1 to 15 percentage points, default 5, stored in `Settings` with serde defaults. They are additive in settings schema 1, need no ABI change, and are expressed in normalized percentage points (0-100). `update_settings` rejects non-integer, negative and non-numeric values, and `Settings::validate` rejects values outside 1 to 15. The native router receives both values through `setSteps(brightness:volume:)`, which rounds to whole points, clamps to 1...15 and falls back to the default for non-finite input, all under its state lock; `reserveStep` picks the step by feature. Shift+Option moves a fixed 1 point, so the fine step is never larger than the normal step. The popup sliders are continuous and are not affected. The router does not reference core types, so its standalone test script can compile it alone.

### Rebase of the key intent waits for our own hardware writes

The router tracks the hardware actions the core accepted, per monitor, connection generation and feature. `MenuBarController` reports acceptance (`hardwareActionAccepted`) and completion (`hardwareActionCompleted`), matched by action generation. While an accepted action is outstanding, and for a 1000 ms settle window after the last completion, the router does not rebase that key's intent. An outstanding action whose completion never arrives stops blocking after 10 s. The state is dropped when the display, its generation or the intent goes away, or when routing is cleared. After that, the existing rule applies, so external changes and monitor clamping still rebase the intent. The slider path is unchanged.

## Alternatives Considered

- Keeping 6.25 as the default through a fractional setting: not used, because the control is an integer from 1 to 15 and 6.25 cannot be represented in it.
- Default 6 or 3 instead of 5: 5 was chosen because it is slightly finer than today, which matches the user's complaint; this is a judgment call that can be changed in one pass.
- Shift+Option scaling with the configured step: not chosen; the fine step is a fixed 1 point.
- Passing `CoreSettings` into the router: rejected, because the router is compiled alone by `tools/tests/test-hardware-key-router.sh` and receives plain `Double` values instead.
- A settings schema bump or ABI change: not needed, because the fields are additive with defaults.
- No alternative to the rebase guard was recorded at the time of the fix. The DDC readback retry timing was left unchanged (see Consequences).

## Consequences

Existing users get a 5-point step after upgrading, with no migration, because the missing fields take their defaults.

The Settings window height grows from 560 to 640 points; the width and the 420-point minimum are unchanged. The window is created once per app run, so the new height is seen after a restart. No automated test compiles `SettingsView.swift`, so the layout and the Settings UI rely on the manual check. The results of that manual check are not recorded in the repository.

Hardware behavior is verified only on one LG ULTRAFINE over DisplayPort with one keyboard. Other monitors, connections and keyboards are unverified.

For the rebase guard, external changes made during a write or within 1 s after it are noticed late. If routing is disabled while a write is in flight, the guard state is dropped. The slow DDC readback retries (`DDCTransaction.readVCP`, 0.2 s and 1.0 s backoffs) were not changed. The user reports that the jump no longer happens on their LG ULTRAFINE setup; this is not a broader hardware claim. Four router tests cover the guard: `rebaseWaitsForOwnHardwareActions`, `rebaseResumesAfterSettleWindow`, `outstandingActionExpires` and `earlyCompletionLeavesNothingOutstanding`.

## Non-Obvious Constraints

- The `CoreSettings` fields must be optional in Swift. Synthesized `Decodable` requires keys for non-optional properties even when they have a default value. `CoreSettings.json` must emit both keys, or a save from the Settings window drops them. Use `?? 5` at the use sites, as for `hardware_keys_enabled`.
- An out-of-range step in a hand-edited `settings.json` fails `Settings::validate`, so the whole file is renamed to a `.corrupt` backup and all settings load as defaults. No special case is added for steps; an out-of-range interval behaves the same way.
- `HardwareKeyRouter.swift` must not reference `CoreSettings` or `CoreBridge`, because `tools/tests/test-hardware-key-router.sh` compiles it alone.
- The DDC layer rounds to raw monitor units. On a monitor whose raw maximum is not 100, an actual change can differ by one raw unit; read-back verification already covers this.
- The rebase guard depends on `MenuBarController` reporting every accepted hardware key write and its completion with the same action generation. A completion that arrives before its acceptance is remembered (bounded to 64 entries), so the late acceptance registers nothing.
- The Shift+Option fine step is fixed at 1 point and does not scale with the configured step.

## Amendment 2026-10-18: Controls handle

[ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md) moves the rebase guard and the step handling out of the macOS router into the core controls handle. The step settings (integers from 1 to 15, default 5) and the fixed 1-point fine step keep their values, and the 1 s settle window after our own writes is now the core's. The shell sends `brightness_step` and `volume_step` through `configure`. The router no longer receives steps through `setSteps`, and `MenuBarController` no longer reports hardware acceptance or completion, so the constraint above about reporting every accepted write no longer applies.
