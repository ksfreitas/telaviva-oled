# ADR-0022: Windows presence camera: shared read-only MediaCapture, infrared sources excluded

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

Presence detection must not interfere with video calls. Infrared cameras must not be used for presence without validation (Q8).

## Decision

- A shared, read-only `MediaCapture` with `MediaFrameReader` in CPU memory, using the smallest adequate format (about 640x480, about 2 fps, single-frame checks in sampled mode), converted to top-down BGRA8 for `tv_inference_detect_bgra`.
- Enumeration uses `MediaFrameSourceGroup` and `DeviceWatcher`. Sources with `MediaFrameSourceKind.Infrared` are excluded from selection and listing. There is no unhide control in the first release.
- The camera id is the symbolic link. Device identifiers are not logged.
- Errors map to core camera statuses as in [contracts](../specs/archived/windows-version/contracts.md): `E_ACCESSDENIED` to `permission`, device absent to `unavailable`, start failure with the device present to `busy`, other errors to `error`.
- The lifecycle runs through `tv_camera_lifecycle_step` with `steady_clock` seconds (see [ADR 0014](0014-camera-lifecycle-in-the-core.md)).
- Fallback: a Media Foundation `IMFSourceReader` behind the same `ICameraBackend`, used if the unpackaged `MediaCapture` path fails.

## Alternatives Considered

- **Media Foundation `IMFSourceReader` as the primary path.** Harder infrared classification and enumeration. It remains the fallback.

## Consequences

- Windows Studio Effects may alter frames on supported hardware. This is a validation row, not a design input.
- "In use by another app" cannot be separated from other start failures, so `busy` is shown as "could not start".
- Evidence:
  - No camera validation note exists yet. The planned spike (unpackaged `MediaCapture` on RGB and infrared cameras) is open.
  - `native/windows/Tests/camera_lifecycle_driver.cpp` holds lifecycle driver tests with a fake clock.
  - Hardware rows P1 (permission denied), P2 (shared with a video call) and P3 (RGB and infrared, infrared hidden) are not run.

## Non-Obvious Constraints

- The core reads no clock. The shell passes `now` in `steady_clock` seconds on every time-dependent camera event.
