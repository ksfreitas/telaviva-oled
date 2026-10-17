# ADR-0021: Windows volume and mute keys through a low-level hook, a lock-free route and container-id ownership

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

`WH_KEYBOARD_LL` is the only documented way to consume a volume key. A slow callback silently removes the hook. Brightness keys keep their Windows behavior (Q5). The macOS router is `HardwareKeyRouter.swift`, governed by [ADR 0004](0004-external-monitor-hardware-keys-and-fluid-controls.md) and [ADR 0005](0005-configurable-hardware-key-step.md).

## Decision

- A dedicated hook thread with its own message loop installs `WH_KEYBOARD_LL` for `VK_VOLUME_UP`, `VK_VOLUME_DOWN` and `VK_VOLUME_MUTE` only.
- The callback reads an atomically published `KeyRoute` (`enabled`, `owner_monitor_id`, `connection_generation`, `volume_supported`, `display_awake`, `step`, `revision`). It swallows a key only when `enabled && owner != null && volume_supported && display_awake`. It posts the intent to the bridge queue and never calls the core.
- The decision is latched for the whole key sequence (down, repeats, up).
- Ownership: the default render endpoint (`eConsole`) container id must equal a monitor's PnP container id. No match, or no id, means the key passes through. There is no name match.
- Mute is emulated as `set_volume` to 0, with the previous level remembered.
- A watchdog runs only while a key is held. If no hook callback arrives within `kKeyHookWatchdogWindowMs` (1500 ms, `native/windows/Platform/KeyHook.h`), the hook is reinstalled and the event is logged.
- The step, rebase guard and per-key latch are a C++ port of `HardwareKeyRouter.swift` (`KeyIntentRouter`).

## Alternatives Considered

- **Raw Input.** Observe only; cannot swallow a key.
- **`RegisterHotKey`.** Volume-key behavior is not documented.

## Consequences

- Windows does not show its own volume OSD for a swallowed key.
- Router logic is duplicated in Swift and C++, kept aligned by ported test vectors.
- The container-id match is inferred from Windows device information. It is not documented by Microsoft.
- Known limitations:
  - Router outstanding-action inputs are not wired. The C ABI `tv_submit` returns only a status, so the key router does not receive outstanding-action inputs from the core.
  - Hook removal while idle is noticed only on the next key press, because the watchdog runs only while a key is held.
  - A user-forced Efficiency mode may delay the hook. The delay is logged (see [ADR 0020](0020-windows-efficiency-ecoqos-user-managed-mode.md)).
- Evidence:
  - [Spike S2b](../specs/archived/windows-version/validation/spike-s2b-audio.md): the NVIDIA HDMI endpoint has the same container id as the DELL monitor. The default output on this machine is the headset, so keys pass through (K3 expected). The AMD and DisplayPort/USB-C paths are not exercised.
  - `native/windows/Tests/key_route.cpp` holds watchdog and route tests.
  - Hardware rows K1 to K4 are not run.

## Non-Obvious Constraints

- Do not add a name-based endpoint match. The endpoint name embeds the EDID name (for example "Dell 32 Plus QD-OLED"), which differs from the PnP monitor name (`DELL_S3225QC`). A name match would fail (spike S2b).
