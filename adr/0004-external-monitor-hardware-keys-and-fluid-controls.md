# ADR-0004: Route external-monitor keyboard controls by pointer and sound output, with serialized fluid DDC writes

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** external-monitor-hardware-keys-and-fluid-controls
**Spec:** [Archived hardware-key spec](../specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys.md)
**Related specs:** [Hardware volume keys follow sound output](../specs/archived/external-monitor-hardware-keys-and-fluid-controls/hardware-volume-keys-follow-sound-output.md), [Hardware mute key restores volume](../specs/archived/external-monitor-hardware-keys-and-fluid-controls/hardware-mute-key-restores-volume.md), [Fluid monitor sliders](../specs/archived/external-monitor-hardware-keys-and-fluid-controls/fluid-monitor-sliders.md), [LG UltraFine DDC brightness writes](../specs/archived/external-monitor-hardware-keys-and-fluid-controls/lg-ultrafine-ddc-brightness-writes.md), [macOS acceptance checklist](../specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys/macos-acceptance.md)

## Context

Opt-in macOS keyboard routing had to adjust an external monitor's brightness and volume over DDC without taking control away from macOS for the built-in display, headphones, or any other target. The menu bar sliders had to stay responsive while DDC writes are slow and can fail. The connected LG ULTRAFINE ignored a single Set VCP write, so brightness writes needed their own transaction rules.

## Decision

(1) A session event tap (default tap) listens to NX_SYSDEFINED events for volume and mute, and to keyDown/keyUp for brightness key codes 144 and 145, which a probe showed are how the tested keyboard sends brightness (with the fn flag). The callback is lightweight: it reads only the key code, autorepeat flag and modifier flags, never calls CoreAudio, DDC or the core, and hands bounded coalescing intents to a worker queue that submits through CoreBridge to the Rust core. It needs both Accessibility and Input Monitoring; local builds are signed with the designated requirement `identifier "com.telaviva.oled"` so grants survive rebuilds.

(2) Brightness targets the single external display under the pointer. Volume and mute target an external monitor only while it is the Mac's default sound output, matched deterministically by CoreAudio device UID equal to the display EDID UUID (case-insensitive, unique, stable identity); anything else passes through to macOS, with no name-based fallback. A DefaultAudioOutputMonitor observes CoreAudio changes off the callback path with no polling.

(3) The routing decision latches for the whole key sequence (repeats and key-up); an output change cancels a held volume key without reinjecting events.

(4) Mute is emulated with volume 0 and restore of a remembered level (bounded to 128 entries, cleared on output change, disable, shutdown, ineligibility and external level change), because the monitor's own mute (VCP 0x8D) was not probed.

(5) Menu bar sliders use bounded draft coalescing, revoke pending work on sleep and do not replay it after wake; snapshot refreshes are conflated to one running read plus one re-run.

(6) DDC writes use a transaction that sends the write twice 10 ms apart, waits 60 ms for the reply, reads 11 bytes, retries and verifies readback equality.

## Alternatives Considered

- Pointer-based volume: rejected, because a user wearing headphones would silently change the monitor's speakers.
- Matching the audio device by name: rejected, because identical monitor models share a name.
- Global NSEvent monitors: rejected, because they cannot consume events.
- Native monitor mute: deferred until a read-only capability probe.
- Polling the audio output: rejected for energy.

## Consequences

The event tap now sees every keystroke on the main thread, so a stalled main thread would delay typing while the feature is enabled; moving the tap to its own thread is the follow-up if that is observed. Behavior is verified only on one LG ULTRAFINE over DisplayPort and one keyboard; other monitors, docks, HDMI, USB audio and keyboards are unverified, and unmatched cases pass through so the failure mode is that macOS keeps the key. CoreBridge exposes a small test seam (`refreshRunObserver`). Windows is deferred.

## Non-Obvious Constraints

- On the tested Apple Silicon keyboard, brightness keys are plain key codes 144/145, not media events, while volume and mute are media events.
- WindowServer checks Input Monitoring separately from Accessibility before delivering keys to a tap.
- An ad-hoc signature with a cdhash designated requirement invalidates permission grants on every rebuild.
- The user reported on 2026-10-16 that brightness, volume and mute work on the connected LG; the rows of the macOS acceptance checklist were not recorded individually and remain `Not run`.
