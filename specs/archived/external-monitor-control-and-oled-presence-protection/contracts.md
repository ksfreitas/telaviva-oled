# Runtime Contracts: OLED Monitor Protection

This document defines proposed boundaries and invariants for the design. It records intended behavior; no implementation or hardware validation is implied.

## Dependency and platform boundary

- Use only open-source libraries and dependencies. UI uses thin native SwiftUI/AppKit and C++/WinRT WinUI 3 shells over the same Rust core; face inference uses ONNX Runtime via `ort`, with CPU fallback, OS-supplied CoreML on macOS, and optional OpenVINO through its direct C API on supported Intel Windows hardware. Bundle YuNet under its MIT license. Enable acceleration only where measured energy benefit is demonstrated.
- OS-provided APIs are accessed through Rust bindings (`windows` on Windows and `objc2` on macOS). Native APIs are platform facilities, not downloaded runtime dependencies.
- Bundle the inference runtime/model and pin their versions and cryptographic digests in a license and provenance manifest. No runtime network fetches and no arbitrary user-selected library paths.
- Platform adapters expose typed results to the application core. They do not decide protection policy.

## Settings contract

Persist a versioned JSON document with `schema_version: 1`. Validate the entire document before use or write; reject unknown future schema versions. Write through a temporary file and atomic replacement. If persisted data is corrupt, preserve it as a recovery backup and load defaults; never silently overwrite the corrupt source before recovery is possible.

| Setting | Default | Accepted values |
|---|---:|---|
| Locale | `auto` | `auto`, `en-US`, `pt-BR`; `auto` uses OS locale, falling back to `en-US` |
| Presence sampling interval | 10 s | 5–300 s |
| Return sampling interval | 1 s | 0.25–5 s |
| Consecutive valid misses | 2 | Fixed policy; not user configurable |
| Suspend presence on battery | true | Boolean |
| Presence protection enabled | false | Boolean |
| Screensaver protection enabled | false | Boolean |
| Launch at login | false | Boolean |

Each external monitor may have a screensaver target in normalized range 0–100, default 0. Built-in displays are excluded. Brightness and volume sliders use normalized 0–100 values while adapters map to device capabilities and preserve raw values for restoration. Invalid ranges, non-finite numbers, duplicate settings keys, or unsupported schema versions are rejected.

## Identity and monitor control

- Identify a monitor using stable EDID data including serial, combined with the OS connection path. If identity is missing or duplicated, report ambiguity and do not guess or apply per-monitor settings.
- A monitor control reports supported capabilities, normalized value, raw device value, range/resolution, and connection identity. Unsupported brightness or volume is explicit and independent per capability.
- Track for each monitor the observed baseline raw brightness, last value applied by this app, protection episode identifier, and command generation. Never reuse a baseline after reconnect or application restart. At episode start, capture the current readable brightness; if it cannot be read, do not apply automatic dimming.
- Before restoring, read the current raw value. Restore only when current value still matches the app-owned last-applied value. A user/manual brightness change becomes the new baseline and suspends dimming for the current episode.
- Effective protection target is the minimum of the episode baseline and targets for all active reasons. Presence target is zero; screensaver target is the configured normalized value mapped to the monitor's raw range. Clear reasons independently; recompute while any reason remains and restore the baseline only after all are clear, subject to manual-override policy.
- A protection episode ends when all raw reasons clear. A manual override becomes the new baseline and suppresses reapplication until that point. Settings changes and device events advance command generation.

## Presence and camera events

Presence observations are typed as `Present`, `Absent`, `InsufficientLight`, or `Unavailable`, and carry capture timestamp and camera/session generation. Only valid `Absent` observations increment the miss count. A `Present` or invalidating event resets consecutive misses. Camera errors, contention, stale timestamps, and insufficient light never count as absence.

The core snapshot exposes per-monitor `presence_dimmed` and camera `mode`, `capture_mode`, `generation`, `status`, and `next_sample_in_ms`. `mode` is `disabled`, `suspended`, `light_only`, or `face`; `capture_mode` is `stopped` when acquisition is disallowed, `single_frame` during ordinary sampling, and `continuous_return` during an active return check. Capture mode is policy output and must not be inferred from cadence or brightness targets. In `continuous_return`, keep one shared camera session open, discard frames until each sample deadline, and run at most one inference per deadline. On a present result, immediately return to `single_frame`. Disabled, suspended, unavailable, battery-suspended, and shutdown states stop acquisition. `next_sample_in_ms` is null when camera mode is disabled or suspended.

No raw frame, image, or video may enter application events, persisted settings, or logs. Frames remain in the capture/inference boundary, unused frames are discarded, and each processed frame is released after inference. Do not perform identity recognition, enrollment, or face storage.

When light is insufficient: remove presence as an active dimming reason and restore presence-only dimming if still owned; stop face inference. Use light-only samples every 10 seconds. Resume face inference after two usable-light samples and reset the miss count. This light recovery path is disabled whenever presence is disabled or suspended on battery; in those states camera acquisition is zero.

Priority order for acquisition is: application shutdown, presence disabled, battery suspension, camera unavailable/permission denied/busy, low-light suspension, normal presence sampling. Screensaver observation remains independent of camera state.

## Commands, retries, and failure handling

- UI and core communicate through typed commands and revisioned state snapshots. Commands include a settings/revision identifier; discard stale queued commands before native dispatch, and do not let stale completions overwrite newer state. Native calls already submitted may be non-cancellable and may still change hardware: serialize writes per monitor, then read back after completion and reconcile to the latest desired target before reporting current state. There is no external HTTP control endpoint.
- Camera and monitor operations return typed errors: `Unsupported`, `Busy`, `Permission`, `Disconnected`, `Io`, and `Timeout`. Expose capability and operational status separately from requested values.
- Use a 2-second soft watchdog for OS calls. Synchronous native calls may not be cancellable; isolate them on bounded workers. On watchdog expiry quarantine the worker and do not start unbounded replacement workers or retries. Do not issue a concurrent replacement write to the same monitor while an earlier native write may still be running.
- Retry transient DDC failures at most three total attempts, with 200 ms then 1 s backoff. Do not retry `Unsupported` or invalid requests. Report final failure visibly and keep observed state distinct from requested state.
- On disconnect, invalidate baseline and pending generations. On reconnect, rediscover identity/capabilities and read a fresh baseline before applying protection. On app restart, read current hardware state; never infer ownership from settings or restore a persisted baseline.
- On normal exit, remove app-owned reductions where safe using current-value ownership checks. A failed readback or failed restore is reported and never represented as success.

## Minimum acceptance checks for implementation

- Defaults, bounds, schema migration/rejection, corrupt-file recovery, and atomic persistence behave as specified.
- Two valid misses dim; invalid observations do not count. Presence and screensaver overlap uses the minimum target, and clears restore only app-owned values.
- Manual changes, low light, battery transitions, camera errors, exit, disconnect/reconnect, and stale command completions preserve the ownership rules above.
- Ambiguous monitor identity never receives guessed per-monitor commands. Unsupported capabilities and bounded retry exhaustion remain visible.
- Logs/events contain no frame data, and disabling or battery-suspending presence yields zero camera acquisition and inference.
- Performance numbers in the feature architecture are initial engineering acceptance targets only; they are not measurements or achieved results. No runtime, model, or platform capability is claimed hardware-verified here.

## Native shell ABI

Both shells consume the same Rust static library through `include/telaviva_core.h`. Export an ABI version, opaque core handles, submit/snapshot operations, shutdown/destroy, and an explicit Rust-owned buffer free operation. UTF-8 JSON envelopes validate to typed Rust commands; input is capped at 64 KiB and snapshots at 8 MiB. No Rust containers, C++ objects, Swift objects, panics or exceptions cross the boundary. The core formats Fluent messages; shells supply native accessibility roles and rendering without duplicating policy.

The snapshot-ready callback carries a revision only and posts UI work to the native main thread. Shutdown stops new callbacks and acknowledges active callback completion before shell userdata is freed; queued closures use a lifetime token and are drained safely. No raw frame enters this ABI. Detailed ABI function names, buffer ownership and thread dispatch are defined in architecture.md and must be verified by both native smoke clients before feature implementation.
