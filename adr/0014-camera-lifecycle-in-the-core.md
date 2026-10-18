# ADR-0014: Camera lifecycle in the core

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** core-camera-lifecycle-abi-contract-and-cleanup
**Spec:** [Archived chore plan](../specs/archived/core-camera-lifecycle-abi-contract-and-cleanup/spec.md)

## Context

The macOS shell keeps camera session timing in `native/macos/TelaViva/CameraLifecycle.swift`. That state machine decides when a session is kept, the 5 s acquisition deadline, the 5 s warm-up and settle window, the once-per-sample dark settle added by the 2026-10-16 amendment to [ADR 0003](0003-camera-lifecycle-and-display-suspension.md), the stale-callback check and the assessment token. It uses no Apple SDK types and already takes time as a parameter, so it is policy written in Swift. The planned WinUI 3 shell would otherwise have to reimplement the deadlines and the dark-settle rule to behave the same way.

[ADR 0001](0001-native-shell-and-rust-core.md) puts sampling policy in the Rust core and leaves device operations to the native shells. Session timing is policy of that kind. Timers, the monotonic clock and AVFoundation are device operations and stay in each shell.

## Decision

- **Session timing moves to the core behind an opaque handle.** `telaviva-core` gains `camera_lifecycle.rs`. The C ABI gains the opaque `TvCameraLifecycle` handle and three functions: `tv_camera_lifecycle_create`, `tv_camera_lifecycle_step` and `tv_camera_lifecycle_destroy`. Each step takes one JSON event and returns one JSON result with a `value` and a `state` object. This follows the JSON style of the existing command protocol.
- **The shell supplies all time.** Every event that depends on time carries `now`, in seconds from the shell's monotonic clock. The core reads no clock, so every deadline is a comparison against a value the shell passed in.
- **Swift keeps the device side.** The shell keeps the `DispatchSource` and `asyncAfter` timers, the monotonic clock and AVFoundation. The Swift `CameraLifecycle` becomes a thin wrapper that keeps its public API and calls the handle.
- **The core's mode stays the only policy for whether the camera runs.** `camera_mode` and `capture_mode` in the core remain the only policy for whether the camera runs. The lifecycle owns only session timing for the current mode, and the shell passes the snapshot's modes in, as it does today.
- **Frame-rate selection stays in Swift.** `chooseFrameRate` and `frameDuration` configure the capture device from AVFoundation frame-rate ranges. They are device configuration, not session policy, so they move to `CameraFrameRate.swift` and stay in Swift.
- **The change is additive to ABI version 1.** `tv_abi_version()` still returns `1`, and `TV_CORE_ABI_VERSION` stays `1`. No existing function, struct or constant changes. Camera frames never cross the ABI, as [ADR 0001](0001-native-shell-and-rust-core.md) requires: the events carry only timing values, the generation and camera identifiers, the modes and assessment tokens.

## Alternatives Considered

- **Send every frame decision through `tv_submit`.** Rejected: each accepted command bumps the revision and republishes the snapshot, so per-frame decisions would amplify snapshot publishing. A presence report already causes several snapshot builds and main-thread publications, as the [snapshot publish amplification spec](../specs/archived/snapshot-publish-amplification/plan.md) records.
- **Leave the lifecycle in Swift.** Rejected: Windows would duplicate the deadlines and the dark-settle rule, and the two copies could drift apart.
- **Generate bindings with UniFFI.** Rejected: the ABI is small, JSON-based and checked by header tests, so a generator would add a dependency for little gain. The C# generator that a Windows shell would use is community-maintained.

## Consequences

- Windows reuses the same deadlines and dark-settle rule. Its shell still needs its own timers, clock and camera capture.
- The fake-clock lifecycle tests move from Swift to `crates/telaviva-core/tests/camera_lifecycle_contract.rs`. The Swift tests keep the frame-rate helpers and a small bridge test.
- The wiring in `PresenceCamera.swift` has no automated test. It is covered only by the wrapper tests and the packaged-app smoke test, and it has not been verified on hardware, so camera acceptance stays pending.
- Deferred follow-ups, each blocked by an existing record:
  - The calibration session machine (`AmbientCalibrationController`) stays in Swift. [ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) makes calibration a shell-owned session, so moving it needs a superseding record first.
  - The `HardwareKeyRouter` keystroke latch stays in Swift. [ADR 0004](0004-external-monitor-hardware-keys-and-fluid-controls.md) says its event tap never calls the core, and [ADR 0005](0005-configurable-hardware-key-step.md) keeps the router standalone, so that decision needs a record first.

## Non-Obvious Constraints

- `now` must come from the monotonic clock, never from wall-clock time. The windows are compared against it, so a wall-clock change would move every deadline.
- A rejected event (malformed JSON, unknown event type or field, or oversized input) returns an invalid-argument status and leaves the lifecycle state unchanged. The Swift wrapper treats any non-OK status as a programmer error and stops the session rather than crashing.
