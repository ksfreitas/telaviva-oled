# Privacy and camera lifecycle review

Date: 2026-10-16. This is a source review of the current macOS shell, Rust policy core, and inference ABI. It is not a physical camera test or a privacy certification.

## Code-reviewed behavior

- Presence protection defaults to disabled in `crates/telaviva-core/src/settings.rs`. The Rust core reports camera mode `disabled` when that setting is off. With battery suspension enabled (the default), unknown power state and on-battery state suspend presence. The macOS camera adapter schedules capture only in `face` or `light_only` modes, and cancels/stops capture for all other modes.
- The core snapshot exposes each monitor's `presence_dimmed` state and an explicit camera `capture_mode`. While present, the macOS adapter acquires one VGA frame and stops the session. During return checks, it holds one session open, discards delivered frames until the configured sample deadline, and runs at most one inference per deadline. The AVFoundation sample delegate locks the selected pixel buffer read-only and evaluates light locally or sends bytes directly to the local inference function. The camera worker does not submit frame bytes to `CoreBridge`; its only report contains a classification/status, timestamp, and camera generation.
- The Rust C ABI exposes JSON commands and snapshots. No camera frame or pixel buffer is part of that contract. Inference accepts a caller-owned BGRA pointer and uses it synchronously; it does not persist the frame. Reviewed camera and inference code contains no image/video logging or file writes.
- When permission is denied/restricted, acquisition is not attempted and the core receives a permission failure. Camera teardown cancels timers, clears the sample-buffer delegate, stops a running session, and destroys the detector on application shutdown.

## Limits and follow-up

These points are established by source inspection and compilation only. Native Swift/AVFoundation behavior was not exercised with a physical camera, denied/revoked permission, another camera client, or live battery transitions. Add native integration checks for zero capture while disabled and while battery-suspended, cancellation when settings/power changes during a pending sample, and no frame data in logs, persisted files, or ABI traffic. Measure capture duration and camera/energy use on supported hardware before making performance claims.
