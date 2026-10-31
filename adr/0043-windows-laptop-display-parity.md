# ADR 0043: Windows parity for the laptop display protection

- Status: Accepted; Windows hardware rows W-L4 to W-L10 marked passed by the owner on 2026-10-27, with no observed values recorded
- Date: 2026-10-29 (implemented 2026-10-24 to 2026-10-27)
- Feature: windows-laptop-parity
- Spec: [Archived plan](../specs/archived/windows-laptop-parity/plan.md); the design source is the [Windows handoff](../specs/archived/laptop-display-protection-and-auto-brightness/windows-handoff.md). This feature had no `spec.md` or `architecture.md` of its own.

## Context

[ADR 0035](0035-laptop-display-protection-and-auto-brightness.md) made the laptop's built-in screen a protectable display on macOS and left Windows deferred by the owner. The Windows design and an ordered task list were written down in the handoff (sections 3 and 7). The macOS hardware tests also produced follow-ups that the Windows shell had to match: the monitored display selector, the presence threshold range, the protection level range, the lid-closed camera rule, calibration crash recovery and the threshold saved per display.

## Decision

Bring every handoff item to the Windows shell, with the handoff's proposed option wherever it left an open question. Tests come first, and the Windows wording is "Laptop" in en-US and "Notebook" in pt-BR (ADR 0023).

- The Windows shell opts in to the core's built-in policy by sending `protectable` and `holding`. Until it does, the core makes no decision for the built-in screen. The shell reads and writes the panel through WMI and accepts the nearest supported level, because WMI snaps a write to a supported level.
- Presence and the screensaver protect the screen at a fixed 1 to 50 percent level. The presence threshold goes down to 0 (it was 10) (since [ADR 0044](0044-presence-brightness-threshold-floor-of-one-percent.md) the minimum is 1), and it is saved per display (0 to 90) through `presence_min_brightness_by_display`.
- A shared owner (`AutoBrightnessOwner`) holds and restores the Windows setting that changes brightness automatically with lighting, for both protection and calibration, with recovery after a forced quit. Calibration also restores the monitor after a crash and waits up to 90 seconds for it to reappear (the monitor part of the recovery only).
- The monitored display picker chooses which display gates the camera. `primary` is sent only inside `monitor_discovered`, so a change of the primary display applies at the next discovery.
- The picker for cameras hides built-in cameras while the lid is closed.
- Settings saves are ignored until the first snapshot arrives. The guard blocks every submit before that snapshot, not only settings saves.

## Alternatives

- Not implemented because the handoff marks them as undecided: the wake re-read of a known monitor that was discovered with brightness unsupported (7.6 d), in-place rediscovery (7.6 e) and the adaptive give-up of calibration recovery (7.5 item 6: delete the record and show a notice, or keep blocking).
- `lock-macos-settings-window-size` is scoped to macOS by the owner and is not part of this work.
- Routing the built-in screen through the DDC monitor path was already rejected in ADR 0035, and the Windows shell keeps the panel out of the core `monitors` map.

## Consequences

The Windows UI shows the Laptop (Notebook) card and General page rows only while the panel is available and adjustable, with the monitored display picker and the per-display threshold. New Windows `Platform` components are `AutoBrightnessOwner`, `BuiltInProtection`, `MonitoredDisplayRules`, the laptop panel follower and the adaptive-brightness read helper, with matching tests, and the existing calibration controller and session were reworked on top of the shared owner. The shared core was not changed by this feature, except for what ADR 0035 already recorded.

The three undecided items above stay open. `scripts\build-windows.ps1 -NoZip` passed with 742 tests and no failures, and `cargo test --workspace --all-targets --locked` passed. The hardware status is the owner's mark of 2026-10-27 recorded in the amendment of ADR 0035, with no observed values.

## Non-Obvious Constraints

- Display sleep is one global state on Windows. The built-in screen's availability comes from the laptop panel only (display-on state and lid through `ILaptopEnvironment`), never from the DDC state.
- The power scheme can change while the screen is protected. The restore writes the saved values, re-applies the saved scheme only if it is still the active one, and reads the setting first so a user's change during the hold is never overwritten.
- The built-in screen can be absent in three ways: the WMI brightness class is missing, the panel is not adjustable, or there is no internal panel. In each case `protectable` is false and the screen stays out of both lists.
- "Automatic brightness" has two meanings on Windows: the operating system setting and the app's follow feature page. Every text must say which one it means.
- Hardware keys do not touch the built-in panel.

Implementation tracking is in the [archived plan](../specs/archived/windows-laptop-parity/plan.md), and the contracts it relies on are in [api.md](../api.md#laptop-display-protection).
