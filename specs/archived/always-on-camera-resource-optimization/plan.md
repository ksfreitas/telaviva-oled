# Chore: Always-on camera resource optimization

## Chore Description

With "Keep camera on" (`always_on_camera`) enabled, the user sees about 10% CPU. The archived design budget is at most 1% of one core while present at the default 10 s cadence and at most 5% while absent at the 1 s cadence (`specs/archived/exagoraternal-monitor-control-and-oled-presence-protection/architecture.md:171-173`). Neither budget has been measured with an active camera (`.../validation/performance.md:29`). Reduce CPU, GPU, memory and wakeups of the camera path to the practical minimum without changing product behavior: cadence, hysteresis, the presence semantics and the C ABI stay as they are.

### Findings (static read, confirmed against the code; nothing was profiled)

- The always-on flag changes only the session lifecycle. `capture_mode` becomes `continuous_monitoring` (`crates/telaviva-core/src/core.rs:1073-1074`) and the `AVCaptureSession` stays running (`PresenceCamera.swift:271, 289`). Sampling cadence is unchanged (`core.rs:1055-1068`).
- The session never caps the frame rate. `PresenceCamera.startCapture` (`PresenceCamera.swift:194-224`) sets only `sessionPreset = .vga640x480`, forces `kCVPixelFormatType_32BGRA` (`:209`) and sets `alwaysDiscardsLateVideoFrames`. There is no `lockForConfiguration`, `activeVideoMinFrameDuration` or `activeVideoMaxFrameDuration` anywhere in the repo. The camera therefore delivers device-default frames (typically about 30 fps) to a delegate that discards all but one frame per 10 s (`PresenceCamera.swift:261`). The ISP, auto-exposure, BGRA conversion and camera-daemon IPC run continuously. Part of that cost lands in the camera daemons (`VDCAssistant`, `AppleCameraAssistant`, `cameracaptured`), not in the TelaViva PID.
- Dimmed monitors use `continuous_return` at 1 Hz with or without always-on (`core.rs:1058-1059, 1071-1072`). That path also holds the session open at the device frame rate and runs YuNet once per second.
- Per inference, `preprocess_bgra` (`crates/telaviva-inference/src/lib.rs:397-429`) allocates a zeroed 3x640x640 f32 buffer (4.9 MB) and runs a per-pixel, per-channel bilinear resample even when the scale is exactly 1.0 (640x480 input gives an identity resample). `decode` builds 12 `format!` key strings per call (`lib.rs:437-447`).
- ORT session options set threads to 1 and optimization to Level3 (`lib.rs:166-177`). Intra-op and inter-op spinning are left at ORT defaults (enabled), which can keep a worker spinning after a run.
- Every presence report amplifies into about 3 snapshot builds, 5 JSON serializations, 2 decodes and 2 main-thread publications (`ffi.rs:150-170`, `CoreBridge.swift:71-119, 238-241`, `core.rs:1089-1112`, `MenuBarController.swift:50-66`). `CoreSnapshot` is not `Equatable`, so every publish fires every sink.
- The sample timer uses `queue.asyncAfter` with no leeway (`PresenceCamera.swift:136-147`), so wakeups cannot coalesce.
- The workspace has no `[profile.release]` overrides (opt-level 3, 16 codegen units, no LTO).

### Strategy, ordered by expected payoff

1. Measure first, so every later change is judged against a baseline and the camera daemons are counted.
2. Cap the camera frame rate while idle (largest expected win), raise it only while a sample or auto-exposure settle needs frames, and run the camera queue at background QoS (efficiency cores) with the deadline timers moved off it.
3. Remove avoidable work per inference (identity resample, zero-fill, string keys, spinning).
4. Remove snapshot amplification per presence report.
5. Coalesce timer wakeups with leeway.
6. Release-profile tuning.

Out of scope here (see Notes): switching capture to YUV with a Y-plane ABI, CoreML, smaller model input, thermal and Low Power Mode policy, pausing on screen lock.

## Relevant Files

- `native/macos/TelaViva/PresenceCamera.swift`: session setup (`startCapture`, 194-224), delegate (`captureOutput`, 259-340), sample timer (`scheduleSample`, 136-147), `requestSample` (153-175), `stopCapture` (419-430). Frame-rate cap, rate switching and timer leeway land here.
- `native/macos/TelaViva/CameraLifecycle.swift`: pure, unit-tested lifecycle logic (`shouldKeepSession`, `reconcile`, settle deadlines). Hosts the new pure frame-rate policy so it is compiled by the existing test script, the app build and the Xcode project without registering a new file.
- `native/macos/Tests/CameraLifecycleTests.swift`: tests for the frame-rate policy.
- `native/macos/TelaViva/CoreBridge.swift`: `submit`, `refresh` and the snapshot callback (45-119, 238-241). Remove the duplicate refresh and skip unchanged publishes.
- `native/macos/Tests/CoreBridgeTests.swift`: guards the ordering and revision semantics of the bridge.
- `native/macos/TelaViva/MenuBarController.swift`: snapshot sink (50-66) that fans out on every publish.
- `crates/telaviva-core/src/ffi.rs`: `tv_submit` builds a full snapshot only to read the revision (150-170); `tv_snapshot` (180-205).
- `crates/telaviva-core/src/core.rs`: `snapshot_value` (1046-1087) and `localized_messages` (1089-1112) re-parse the FTL text on every snapshot.
- `crates/telaviva-core/tests/ffi_contract.rs` and `crates/telaviva-core/tests/core_contract.rs`: contract tests that must stay green (the ABI does not change).
- `crates/telaviva-inference/src/lib.rs`: `create_session` (166-177), `preprocess_bgra` (397-429), `decode` key strings (433-453), existing unit tests (557-627).
- `crates/telaviva-inference/tests/ort_yunet_smoke.rs`: integration test that runs the pinned model (needs the fetched runtime).
- `Cargo.toml`: workspace manifest, home of the new release profile.
- `tools/validation/sample_process.py`: existing CPU sampler for the baseline and the after measurement.
- `tools/tests/test-camera-lifecycle.sh`, `tools/tests/test-core-bridge.sh`: Swift test entry points.
- `scripts/build-macos.sh`, `packaging/macos/build-app.sh`: release build used for the measurement.
- `specs/archived/external-monitor-control-and-oled-presence-protection/validation/performance.md`: existing performance report format and budgets.
- `specs/archived/camera-lifecycle-and-display-suspension/validation/macos-acceptance.md`: acceptance matrix (all always-on hardware rows are "Not run").
- `adr/0003-camera-lifecycle-and-display-suspension.md` and `ARCHITECTURE.md`: document the always-on lifecycle; updated with the frame-rate policy.
- `CHANGELOG.md`: Unreleased entry.

### New Files

- `specs/always-on-camera-resource-optimization-validation.md`: baseline and after measurements (CPU of the TelaViva PID and the camera daemons, thread count, wakeups) per scenario, in the format of `performance.md`. Created in the first task, completed in the last.

## Step by Step Tasks

### 1. Capture the baseline on hardware (before any code change)

- Build the current tree: `scripts/build-macos.sh` (release, Apple Silicon), then launch the app from the produced bundle.
- Scenarios, each 120 s after a 30 s warm-up, always-on ON and OFF where applicable: (a) present and idle at the default 10 s cadence, (b) absent and dimmed at the 1 s return cadence, (c) light-only (dark room or covered lens), (d) protections off (control).
- For each scenario run `python3 tools/validation/sample_process.py <pid> --expected-executable "<app>/Contents/MacOS/TelaViva" --interval 1 --duration 120 --output <file>` and, in parallel, record the camera daemons with `top -l 120 -s 1 -stats pid,command,cpu -pid <pid>` for `VDCAssistant`, `AppleCameraAssistant` and `cameracaptured` (look the PIDs up with `pgrep -x`). Record thread count with `ps -M <pid>`.
- Stream `log stream --predicate 'category == "wake"' --level debug` and note `sample_scheduled next_sample_ms` (1000 means return or resume, 10000 means presence or light-only) to confirm which state each scenario is in.
- Write the table into `specs/always-on-camera-resource-optimization-validation.md` under "Baseline". If the 10% is not in the TelaViva PID but in the daemons, say so. This decides whether tasks 4 and 5 are the main lever or only secondary.

### 2. Add a pure frame-rate policy and its tests

In `native/macos/TelaViva/CameraLifecycle.swift` add, with English comments:

- `enum CaptureRatePhase { case idle, active }`.
- `func captureFrameRate(phase: CaptureRatePhase, supportedMinimum: Double, supportedMaximum: Double) -> Double`. Targets: idle 2 fps and active 15 fps, both clamped into `[supportedMinimum, supportedMaximum]`. Constants `idleFrameRate` and `activeFrameRate` are named and documented in one place so hardware tuning is a one-line change.
- A second pure helper `func frameDuration(forRate:)` returning a `CMTime`-agnostic `(value: Int64, timescale: Int32)` so the file stays free of AVFoundation (it must keep compiling standalone in the test script).

In `native/macos/Tests/CameraLifecycleTests.swift` add tests: idle below the device minimum is clamped up, active above the device maximum is clamped down, a device range that excludes both targets resolves to the nearest bound, an inverted or zero range falls back to the active rate, and the duration helper round-trips.

### 3. Cap the frame rate in `PresenceCamera`

In `native/macos/TelaViva/PresenceCamera.swift`:

- After `capture.commitConfiguration()` in `startCapture`, apply the idle rate to the selected device: `try camera.lockForConfiguration()`, read `camera.activeFormat.videoSupportedFrameRateRanges` (minimum of all `minFrameRate`, maximum of all `maxFrameRate`), compute the rate with `captureFrameRate`, set both `activeVideoMinFrameDuration` and `activeVideoMaxFrameDuration`, then `unlockForConfiguration()`. On any error log at debug level and continue at the device rate (never fail capture because of rate control).
- Keep a small `setCaptureRate(_ phase:)` helper that re-locks the same device (store the `AVCaptureDevice` next to `activeCameraID`) and applies the phase rate. Call it with `.active` when a sample is requested (`requestSample`, and where `inferenceRequested` is set in `startCapture`) and when the light-only settle begins, and with `.idle` right after the requested frame is processed (in the `defer` path where `keepSessionOpen` stays true, `PresenceCamera.swift:271-289`) and when the settle ends.
- In `single_frame` mode the session already lives for one sample only, so start it at the active rate directly and do not switch.
- `stopCapture` needs no change: the device keeps its last duration only while the session exists, and a new session re-applies the policy.
- Preserve the acquisition watchdog (5 s) and the light settle (5 s). Do not change cadence, `next_sample_in_ms` or any value sent to the core.
- Run the camera queue at background QoS. Change the QoS of `com.telaviva.camera` from `.utility` to `.background` (`PresenceCamera.swift:15`), so the frame callback, the luma assessment and the YuNet inference run on the efficiency cores of Apple Silicon. Keep the queue serial, and do not touch the `CoreBridge` worker (`.userInitiated`, `CoreBridge.swift:18`), the main thread, the DDC path or the event tap. Do not use process-wide `PRIO_DARWIN_BG` or `beginActivity(.background)`: they would throttle DDC I/O and key routing, and can stretch timers.
  - Starvation guard: a background-QoS queue can be deferred under CPU load, so the 5 s acquisition watchdog and the 5 s settle timers must not run on it. Move those two timers (`armCaptureTimeout` at `PresenceCamera.swift:228-257`, the settle timer at `:158-175`) to a small second serial queue `com.telaviva.camera.deadlines` at `.utility`, and hop back to the camera queue for the work. Keep the sample timer on the camera queue.
  - Fallback: if a measured run (task 9) shows watchdog timeouts, a higher camera-status "busy"/"unavailable" rate, or inference latency above 250 ms p95 under load, revert to `.utility` by changing the single QoS constant. Name it (`cameraQueueQoS`) so the revert is one line.
- Resolution stays capped at 640x480: the model input is fixed at 640x640 (`INPUT`, `lib.rs:18`), so nothing larger is ever useful and a 4K webcam already delivers VGA through `.vga640x480`. Do not capture below it (it would be upscaled to 640x640 with no inference saving and less range for small faces). Harden the preset: set `.vga640x480` only when `capture.canSetSessionPreset(.vga640x480)`; otherwise try `.medium`, then `.low`, then leave the device default and log at debug level. The Rust side already accepts any size up to 8192 and letterboxes it, so a fallback format needs no ABI change. Add the picked preset to the existing `start_running` wake log.
- Do not lower the active rate below the idle-to-active transition latency budget: the first frame after raising the rate must still arrive well inside the 5 s watchdog (about 1/2 fps = 0.5 s worst case at the idle rate before the new duration takes effect).

### 4. Cut per-inference work in Rust

In `crates/telaviva-inference/src/lib.rs`:

- `preprocess_bgra`: add a fast path when `resized_w == width && resized_h == height` (identity scale, the normal 640x480 case). Write each row directly: for every pixel store `bgra[...] as f32` into the three channel planes with no interpolation. Output must be bit-identical to the generic path. In the generic path precompute per-column `(x0, x1, fx)` and per-row `(y0, y1, fy)` tables once instead of recomputing per pixel and channel.
- Keep the zero padding semantics exactly (padding rows and columns stay 0.0).
- `decode`: replace the 12 per-call `format!` strings with a `const` array of the 9 required output names (`cls_8`, `cls_16`, `cls_32`, `obj_*`, `bbox_*`; match what `lib.rs:437-447` actually builds) indexed by stride.
- `create_session`: add `.with_intra_op_spinning(false)?` and `.with_inter_op_spinning(false)?` if the pinned `ort 2.0.0-rc.9` exposes them (verify in `~/.cargo/registry/src/*/ort-2.0.0-rc.9/src/session/builder/impl_config_keys.rs`); if only the config-entry API exists, use `with_config_entry("session.intra_op.allow_spinning", "0")` and `("session.inter_op.allow_spinning", "0")`. Leave threads at 1 and `panic = unwind` untouched.
- Add unit tests next to the existing ones (`lib.rs:557-627`): the fast path equals a reference generic implementation on a random 640x480 BGRA frame with a non-trivial stride; a 1280x720 input still takes the generic path and keeps its padding; the output-name table matches the model's output names when the runtime is available (otherwise assert only the table contents).

### 5. Remove snapshot amplification

- `crates/telaviva-core/src/ffi.rs`: make `tv_submit` obtain the revision without building and serializing a full snapshot (read the revision counter from `Core` directly; add a tiny `Core::revision()` accessor if none exists). The callback payload, the revision value and the ABI (`ABI_VERSION = 1`, `include/telaviva_core.h`) must not change.
- `crates/telaviva-core/src/core.rs`: cache the parsed FTL messages per locale (a `OnceLock` or a field built at construction) so `localized_messages()` stops re-parsing on every snapshot. Keep output identical.
- `native/macos/TelaViva/CoreBridge.swift`: remove the redundant `refresh()` after `submit` when the snapshot-ready callback already schedules one (keep the `refreshAgain` coalescing, and keep the post-condition that a snapshot read after `submit` returns reflects that command). Make `CoreSnapshot` `Equatable` (or compare the revision) and skip `@Published` assignment when nothing changed, so `MenuBarController`'s sink does not re-run `camera.apply`, `manualControls.apply`, `ddc.process` and key routing for identical snapshots.
- Extend `native/macos/Tests/CoreBridgeTests.swift` to cover: one command yields exactly one publish; an identical refresh does not publish; a state change after `submit` is visible immediately.
- Run `crates/telaviva-core/tests/ffi_contract.rs` and `core_contract.rs` unchanged; add a core test that two consecutive snapshots with no state change keep the same revision and identical JSON.

### 6. Coalesce sample-timer wakeups

In `PresenceCamera.scheduleSample` replace the `DispatchWorkItem` plus `asyncAfter` with a one-shot `DispatchSourceTimer` on `queue` (`schedule(deadline:leeway:)`, `repeating: .never`), with leeway `min(0.1 * delay, 1.0)` seconds so a 10 s cadence tolerates up to 1 s and the 1 s return cadence up to 0.1 s. Preserve the exact guard semantics: epoch check, generation check, `stopped` check, and cancellation through `sampleTimer`/`sampleTimerEpoch` in `stopCapture`. Keep the 5 s acquisition watchdog and 5 s settle timers precise (no leeway). Change the `sampleTimer` type accordingly and make cancellation resume-safe (a suspended source must not be released).

### 7. Release profile

Add to the workspace `Cargo.toml`:

```toml
[profile.release]
codegen-units = 1
lto = "thin"
```

Do not set `panic = "abort"`: every FFI export relies on `catch_unwind`. Confirm `scripts/build-macos.sh` and `packaging/macos/build-app.sh` still produce a working bundle and that the static libraries link.

### 8. Documentation

- `CHANGELOG.md`, under Unreleased, add a `### Changed` bullet: always-on and return-mode camera now cap the capture frame rate while idle and raise it only for samples and settle; lower per-inference CPU; fewer snapshot publishes; timer wakeup coalescing. State that hardware numbers are in the validation file and that acceptance is pending.
- `adr/0003-camera-lifecycle-and-display-suspension.md` and `ARCHITECTURE.md` (camera lifecycle section near line 119): document the idle and active frame-rate policy, the constants, and that cadence and presence semantics are unchanged.
- Update the always-on and energy rows of the acceptance matrix only if they were actually re-measured; otherwise leave them "Not run".

### 9. Measure again and tune

- Rebuild with `scripts/build-macos.sh`, repeat the exact scenarios of task 1 and fill "After" and "Delta" in `specs/always-on-camera-resource-optimization-validation.md`.
- Check functional behavior on hardware: face present, away then dimmed within about two cycles, return restores brightness within about one return interval, light-only enters and leaves correctly, switching camera restarts the session at the idle rate, display sleep and wake still resume with the 1 s first sample, no acquisition-watchdog timeouts.
- If idle capture still dominates (daemon CPU above budget), tune only `idleFrameRate` (try 1 to 3) and `activeFrameRate` (10 to 30) and re-measure. Do not add new mechanisms in this chore; record the residual and the follow-ups from Notes in the validation file.
- Compare `.background` against `.utility` on the camera queue: run scenario (b) once with each (`cameraQueueQoS`), under both idle and a synthetic CPU load (for example `yes > /dev/null` on all cores for 60 s). Keep `.background` only if CPU or energy impact improves and there are no watchdog timeouts, no `busy`/`unavailable` reports and no missed return intervals. Record the verdict in the validation file.
- Target: present at 10 s cadence at most 1% of one core in the TelaViva PID plus daemons combined as best effort, and in every case no sustained 10%; absent at 1 s cadence at most 5%.

### 10. Run Validation Commands

Run every command in the Validation Commands section and fix any failure before finishing.

## Implementation Status (2026-10-16, branch `cfs_worktree-1`)

| Task | Status | Note |
|---|---|---|
| 1. Baseline on hardware | Pending (manual) | Needs the app, a camera and the camera-daemon sampling. Build the base commit `80f6e0c` for a clean baseline. |
| 2. Frame-rate policy and tests | Done | `CameraLifecycle.swift`, `CameraLifecycleTests.swift`. |
| 3. Frame rate, preset, QoS in `PresenceCamera` | Done | See deviations below. |
| 4. Per-inference work in Rust | Done | `lib.rs`: identity fast path, row/column tables, constant output names, spinning off (`with_intra_op_spinning(false)`, `with_inter_op_spinning(false)`, both present in `ort 2.0.0-rc.9`). Output is bit-identical to the old path, proven by exact-equality tests on four frame sizes. |
| 5. Snapshot amplification | Deferred | Touches `core.rs`, `ffi.rs`, `CoreBridge.swift`, which the `macbook-brightness-follow` work is rewriting in the other working tree. Do it as a separate chore after that branch merges. |
| 6. Sample-timer leeway | Done | One-shot `DispatchSourceTimer`; the previous timer is now cancelled when rescheduled. |
| 7. Release profile | Done | `codegen-units = 1`, `lto = "thin"`, `panic` unchanged. |
| 8. Documentation | Done | `CHANGELOG.md`, ADR-0003 amendment, `ARCHITECTURE.md`. |
| 9. Measure again and tune | Pending (manual) | Includes the `.background` versus `.utility` comparison (`cameraQueueQoS`). |

Deviations from the plan text, decided during implementation:

- Tasks 2 and 3 above describe an idle/active policy with two rates and a `captureFrameRate` clamp. That was superseded twice: first by per-range matching after the crash below, then by a single rate per session after the LED blink below. The shipped design is `sessionFrameRate` (target 2 fps), matched to the nearest rate the camera supports with `chooseFrameRate(target:ranges:)`, applied once right after `startRunning`, only for retained sessions (`continuous_monitoring` and `continuous_return`), and never changed while the session runs. Single-frame sessions are not capped. Return checks on a retained session therefore wait up to one frame interval (up to 0.5 s at 2 fps, and the camera's floor where it is higher), which is accepted for the cut in wakeups.
- `resetCaptureRate()` restores, by value, the frame durations the device reported before the first cap (recorded once per session, validated against the active format) before the session stops, so the cap cannot outlive the session or reach other apps. It does not use `CMTime.invalid`.
- The session preset is set after the input is attached, because `canSetSessionPreset` is only meaningful with an input. The fallback order is VGA, medium, low.
- `disableCaptureRateControl` (user defaults) turns frame-rate control off, as an escape hatch for a camera that misbehaves.

Incident, same day: the first hardware run crashed at startup (SIGABRT, no camera LED). `setCaptureRate` wrote an exact 1/15 s duration to a camera that only offers 15.000015 fps, and `AVCaptureDevice` raised `NSInvalidArgumentException: Not supported - Supported ranges`, which Swift cannot catch; it also read the ranges before `startRunning`, when the active format is still 1080p and the VGA preset has not applied. Reproduced on the Lenovo external camera with a standalone probe. Fixed by choosing the rate per supported range (`chooseFrameRate`), using the device's own duration value at range edges, validating every duration before writing it, and applying the rate after `startRunning`. Lesson for tasks of this kind: a typecheck and unit tests do not exercise AVFoundation, so the camera path needs a hardware run before it is called done.

Second hardware finding, same day: after the crash fix the camera LED blinked on every rate change. Changing the frame rate of a running camera restarts its stream. The first design switched rates at session start, after the first frame, and at each light-only settle, so the LED blinked repeatedly (and also once per single-frame sample). Fixed by setting the rate once per retained session and never again, and by not capping single-frame sessions. One LED blink remains at the start of each retained session, right after the camera turns on. If that is still unwelcome, the alternative is to choose the capture format and durations before `startRunning` (set `activeFormat` and an input-priority preset), which needs a hardware iteration of its own.

Verified in this worktree: `cargo fmt --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo test --workspace --all-targets --locked` (including the pinned YuNet model on the bundled runtime), `tools/tests/test-camera-lifecycle.sh`, `tools/tests/test-core-bridge.sh`, a typecheck of every app source with the `build-app.sh` flags, and `scripts/build-macos.sh` (release build and ad-hoc signed bundle). The chosen durations were also applied, and the originals restored, on all four cameras of this Mac (built-in, Lenovo external with discrete rates, a virtual camera, an iPhone camera) with the real `chooseFrameRate` code and no exception, but on the pre-start 1080p format and without a capture session. Not verified: a real session with the VGA preset (the format after `startRunning` can have different ranges), and the CPU and energy numbers. Note that the built-in camera has a 15 fps floor, so its idle saving is 30 to 15 fps, not 30 to 2.

## Validation Commands

```bash
cd /Users/cassio/Documents/GitHub/telaviva-oled-cfs_worktree-1

# Rust formatting, lints and tests (workspace)
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu

# Core contract tests that guard the unchanged ABI and cadence
cargo test -p telaviva-core --all-targets --locked

# Inference unit tests (fast path equals generic path)
cargo test -p telaviva-inference --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu

# Real model on the bundled ONNX Runtime (after tools/inference/fetch-macos-runtime.sh)
cargo test -p telaviva-inference --locked -- bundled_runtime_executes_pinned_yunet_model_on_cpu

# Swift unit tests: lifecycle and frame-rate policy, camera selection, core bridge, localization
tools/tests/test-camera-lifecycle.sh
tools/tests/test-core-bridge.sh

# Release build of the app and libraries
scripts/build-macos.sh
```

Manual hardware validation (cannot be automated, required for acceptance): the before and after measurements of tasks 1 and 9 recorded in `specs/always-on-camera-resource-optimization-validation.md`.

## Notes

- No profile or measurement exists today, so the 10% is not attributed yet. Task 1 decides it. Expected suspects, in order: always-on idle capture at device frame rate with forced BGRA (lives mostly in AVFoundation and the camera daemons), then the 1 Hz return inference when a monitor is dimmed (this runs with or without always-on, so if the 10% shows only while away it is not an always-on cost).
- Always-on in light-only mode keeps the camera at 100% duty versus about 33% in default mode. This is existing, intended behavior; the frame-rate cap reduces its cost without changing it.
- Auto-exposure risk: a very low idle rate slows exposure convergence. The active rate is applied during the 5 s settle and for every sample for that reason. If dark-room detection regresses on hardware, raise `idleFrameRate` first.
- Rate control is best effort. Some cameras expose a narrow range (a few expose only 30 fps); the policy clamps and falls back to the device rate, so a camera with no range simply sees no saving.
- Deferred follow-ups, each worth its own chore only if task 9 still shows a residual:
  - Capture YUV (`420YpCbCr8BiPlanarFullRange`) while idle and assess light from the Y plane; this needs an inference ABI addition because `tv_inference_detect_bgra` and `tv_inference_assess_light_bgra` are BGRA-only, and the inference ABI has no version constant.
  - CoreML or ANE execution provider (`tv_inference_create_with_provider` exists but Swift never calls it; an earlier attempt failed on this host and energy benefit is unverified).
  - A smaller static model input (needs a re-exported model, new SHA-256 and provenance).
  - Reusing the 4.9 MB input buffer (needs a non-copy ORT input API; `ort 2.0.0-rc.9` moves owned arrays without copy, so reuse is not trivial).
  - Thermal state and Low Power Mode observers that force `single_frame`, and pausing on screen lock. Both change user-visible semantics (screensaver protection is independent of the lock), so they are product decisions.
- The inference crate has no ABI version check; do not add one in this chore.
- Reports from the exploration agents were spot-checked against `PresenceCamera.swift:198-214, 261, 271, 289, 425` and `lib.rs:166-177, 397-429`. Other line references (core cadence, snapshot amplification, ORT spinning defaults inferred from `ort` source, the missing quality module cited in an old plan) come from the reports and must be re-read by the implementer before editing.
- Language: documentation and commit messages en-US; Swift and Rust comments in English; Portuguese UI strings are not touched by this chore.
