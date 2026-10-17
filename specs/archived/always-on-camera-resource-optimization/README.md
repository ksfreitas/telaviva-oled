# Always-on Camera Resource Optimization

This archive preserves the implementation plan for cutting the CPU, wakeup and energy cost of the macOS camera path while "Keep camera on" is enabled. The work was a standalone fast chore, so it has no separate `spec.md` or `contracts.md`; requirements, decisions, tasks, deviations, incidents and verification are recorded together in [plan.md](plan.md) (see "Implementation Status" there).

- [Implementation plan, status and incidents](plan.md)

## What shipped

- One low frame rate per retained capture session (target 2 fps, matched to the nearest rate the camera supports), set once after `startRunning` and never changed while the session runs; single-frame sessions are not capped.
- Camera queue at background QoS, with the watchdog and settle deadlines on a separate utility queue; sample timer with leeway; safe session preset fallback.
- Inference: identity fast path for 640x480 frames, constant output names, ONNX Runtime spinning disabled.
- Release profile with thin LTO and one codegen unit.

The decision record is the 2026-10-16 amendment of [ADR 0003](../../../adr/0003-camera-lifecycle-and-display-suspension.md).

## Still open

- Hardware baseline and after measurements (plan tasks 1 and 9): CPU of the TelaViva process and the camera daemons, energy, wakeups, and the `.background` versus `.utility` comparison of the camera queue (`cameraQueueQoS`). Nothing was measured; no CPU or energy claim is made.
- A real capture session with the VGA preset on every camera in use. The chosen frame durations were exercised only on the pre-start format and without a session, and the LED behavior of the one-time rate change after start is unconfirmed.
- Snapshot amplification (plan task 5) was deferred because it touches `core.rs`, `ffi.rs` and `CoreBridge.swift`, which the macbook-brightness-follow work is rewriting. It continues in [specs/snapshot-publish-amplification.md](../../snapshot-publish-amplification.md).

Two defects found on hardware during this work are described in the plan: an uncaught Objective-C exception from an out-of-range frame duration (startup crash), and LED blinking caused by changing the rate of a running camera.
