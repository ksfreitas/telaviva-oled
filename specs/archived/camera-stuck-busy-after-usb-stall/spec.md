# Bug: Camera stays "busy" after a stalled stream until the app is restarted

## Bug Description

Out of nowhere the camera status shows "busy" ("ocupada") and presence protection stops working. Quitting and reopening the app fixes it. It has happened before.

- Expected: a camera that stops delivering frames is retried on its own, a few times with growing waits, and presence protection resumes as soon as the camera answers again.
- Actual: the first failure suspends the camera for good. No sample is taken again until an unrelated event happens (display wake, battery to AC, presence switched off and on, lid change, or a camera connect or disconnect).
- The label is also wrong. "busy" suggests another app holds the camera, but the shells report it whenever no frame arrives in time or the start fails with the device present.
- Nothing about the failure reaches the logs. The shell writes camera events at `.debug` (not persisted by macOS), the Windows log has no camera lines, and the core ring has no camera status line.

Evidence from the 2026-10-23 macOS unified log (app pid 87078):

- 12:48:00 capture session starts and runs about 1h36 without trouble.
- 14:15 and 14:24 `UVCAssistant` reports about 80 `isoch-stream status: e00002ed` errors (kIOReturnNotResponding) within seconds, with MJPEG decode errors, for the USB camera "Integrated Camera" (`VendorID_5967`).
- 14:24:33 the app's own watchdog calls `stopRunning` and reports `busy`.
- No session starts again until 17:30. No `presence_dim` after 13:35.
- The correlation between the USB errors and the app's session is by timing only. The app does not log which camera it used.

## Problem Statement

`busy` and `unavailable` are terminal for the camera until an external event clears them. The core has no clock and the shells arm no timer after such a report, so a transient stall (USB glitch, camera held briefly by another app, camera plugged in late) disables presence protection indefinitely. The status is mislabeled and invisible in the logs.

## Solution Statement

Put the retry policy in the shared core, once for both platforms, and keep the shells as thin timers:

1. The core counts consecutive `busy` or `unavailable` reports and exposes a new additive snapshot field `camera.retry_in_ms`: 30 s, then 60 s, then 300 s, staying at 300 s. It is null in every other state, including `permission` and `error`, which need user action.
2. Each shell arms one one-shot timer from `retry_in_ms` and, when it fires, sends the existing `camera_status` command with status `unknown` and the current generation (the same message the shells already send on a device change). The core then leaves `suspended`, sets `resume_pending` so the next sample is taken after 1 s, and the normal sample path probes the camera again. A failed probe reports `busy` again and the next, longer wait applies.
3. The counter resets when an accepted sample sets the status to `ready` and on every real resume event (`begin_camera_resume`).
4. The core records the transitions in its diagnostics ring (`camera_failure`, `camera_retry`), which both shells already drain into the unified log (macOS) and `telaviva.log` (Windows). The lines carry no device identifier.
5. The text for `cameraStatus.busy` changes to "no image" (en-US) and "sem imagem" (pt-BR) on both platforms.

No new setting, no new dependency, no new network use. ABI version stays 1: `retry_in_ms` is an additive snapshot field, like `tv_diagnostics_drain`.

## Steps to Reproduce

Unit level (deterministic):

1. Send `camera_status` `busy` with the current generation to a core with presence protection on. The snapshot shows `camera.mode == "suspended"` and `next_sample_in_ms == null`, and nothing else changes with the passage of time.

Real hardware:

1. Run the app with presence protection on and an external USB webcam selected.
2. Make the stream stall: unplug and replug the USB hub or cable only far enough to cause isochronous errors, or load the USB bus. Watch `/usr/bin/log stream --info --predicate 'process == "UVCAssistant"'` for `e00002ed`.
3. About 5 s later the footer shows "Camera: busy" and capture stops. Wait several minutes with the camera healthy: it never recovers. Quit and reopen the app: it recovers.

Note that `log` in zsh is a builtin, so use `/usr/bin/log` to read the logs.

## Root Cause Analysis

- Shell watchdog, `native/macos/TelaViva/PresenceCamera.swift:415-444` (and `:459-464`) and `native/windows/Platform/CameraLifecycleDriver.cpp:530-555` (and `:250-251`): no frame within 5 s stops capture and reports `busy`.
- Core, `crates/telaviva-core/src/core.rs:985-988`: `busy`, `unavailable`, `permission` and `error` make `camera_mode()` return `suspended`. In that mode the snapshot's `next_sample_in_ms` is null (`core.rs:1862`), and presence observations are dropped (`core.rs:840-847`).
- Shells, `PresenceCamera.swift:198-201` and `CameraLifecycleDriver.cpp:172-175`: when the mode is not `face` or `light_only` they stop capture and arm nothing. The shells schedule only from `next_sample_in_ms`.
- The core reads no clock (ADR 0014), so it cannot retry by itself, and nothing else in the shells does either.
- The only exits are the events listed in `core.rs` `begin_camera_resume` callers (`:250`, `:547`, `:805`, `:1174`) and the shells' camera-change handlers (`PresenceCamera.swift:646-668`, `CameraLifecycleDriver.cpp:181-205`). A USB stream error that does not disconnect the device triggers none of them. On Windows the device watcher exists only while a session is open (`MediaCaptureBackend.cpp:444-470`), so it cannot fire after a busy, and a camera plugged in while the status is `unavailable` is not noticed.
- Observability: camera lines are `wakeLog.debug` (`PresenceCamera.swift:617`), the Windows log has none, and ADR 0034 lists no camera event.

## Relevant Files

- `crates/telaviva-core/src/core.rs`: state machine. Add the failure counter, the backoff constants next to `RESUME_FIRST_SAMPLE_MS` (`:21`), the `retry_in_ms` snapshot field (`:1862`), the resume flag on `busy`/`unavailable` to `unknown`, and the diagnostics lines. Writers of the status: `camera_status` command (`:255-286`), `presence_observation` (`:812-935`, success at `:850-852`, unavailable at `:910-916`), `begin_camera_resume` (`:968-978`).
- `crates/telaviva-core/tests/core_contract.rs`: regression tests next to `resume_clears_low_light_and_busy_status_but_keeps_permission` (`:2700`).
- `crates/telaviva-core/src/diagnostics.rs`: ring format, only read to match line style.
- `native/macos/TelaViva/CoreBridge.swift:327`: add `retry_in_ms` to `CameraSnapshot`.
- `native/macos/TelaViva/PresenceCamera.swift`: arm and cancel the retry timer in `apply()` (`:148-210`), reusing `resendUnknownStatusIfNeeded` (`:663-668`); cancel it in `stopCapture` callers that shut down and in `shutdown()` (`:684`).
- `native/macos/TelaViva/Localization.swift:5-6`: `cameraStatus.busy` text.
- `native/macos/Tests/CoreBridgeTests.swift` (around `:239`): decode test for the new field.
- `native/windows/Platform/JsonCodec.h:110` and `JsonCodec.cpp:915`: parse `retry_in_ms` into `camera.retryInMs`.
- `native/windows/Platform/CameraLifecycleDriver.cpp` and `.h` (`onSnapshot`, `:165-180`; `onDevicesChanged`, `:181-205`; `shutdown`): the retry timer through the existing scheduler, reusing the "send unknown" code at `:198-201`.
- `native/windows/TelaViva/Strings/en-US/Resources.resw:463-465` and `pt-BR/Resources.resw:463-465`: `cameraStatus.busy` text.
- `native/windows/Tests/camera_lifecycle_driver.cpp` (`:345`, `:470-486`) and `native/windows/Tests/json_codec.cpp` (`:175-198`): driver and codec tests.
- `api.md:38`, `api.md:218`, `ARCHITECTURE.md:215` and the matching section style at the end of `ARCHITECTURE.md`: document the field and the retry.
- `adr/0034-local-diagnostic-logs.md`: add the two events to the event list (`:22`, `:46`: event names are the contract).
- `docs/user-guide.md`: the camera status section (`:26-28`, `:62-69`) and the diagnostic log section (`:227-230`).
- `CHANGELOG.md`: entry under Unreleased.

### New Files

- `adr/0036-camera-failure-retry-in-the-core.md`: records the decision (retry policy in the core, shells only obey `retry_in_ms`), the backoff, why `permission` and `error` are excluded, and the label change. Follow the format of `adr/0035-*.md`. It also corrects the stale line in `adr/0022-windows-presence-camera-shared-mediacapture.md:28` ("busy is shown as 'could not start'"), which never matched the shipped text.

## Step by Step Tasks

Execution order: the core first, then both shells in parallel (different files), then docs. Edits to the same file go to one agent.

### 1. Core: failure counter, backoff and snapshot field

- In `core.rs` add `camera_failures: u32` to the core struct, initialized to 0.
- Add `const CAMERA_RETRY_MS: [f64; 3] = [30_000.0, 60_000.0, 300_000.0];` next to `RESUME_FIRST_SAMPLE_MS`.
- Add a small helper `note_camera_failure(&mut self)` that increments `camera_failures` (saturating) and pushes the `camera_failure` diagnostics line. Call it where the status becomes `busy` or `unavailable`: the `camera_status` command (`:274`) and the `presence_observation` unavailable path (`:910-916`), only when the new status is `busy` or `unavailable`.
- Reset `camera_failures` to 0 where an accepted sample sets the status to `ready` (`:850-852`) and in `begin_camera_resume`.
- In the `camera_status` command, when the previous status was `busy` or `unavailable` and the new one is `unknown`, set `resume_pending = true` and push the `camera_retry` line, so the next sample comes after `RESUME_FIRST_SAMPLE_MS`. Leave every other transition as it is.
- In `snapshot_value`, add `"retry_in_ms"` to the `camera` object: `CAMERA_RETRY_MS[min(camera_failures - 1, 2)]` when the status is `busy` or `unavailable`, presence protection is enabled and the core is not closed; otherwise null. `permission` and `error` stay null.
- Diagnostics lines (event names are the contract, no device identifiers): `camera_failure status=busy failures=2 retry_in_ms=60000` and `camera_retry failures=2`.
- Conventions: comments in English, match the surrounding density, no refactoring of adjacent code.

### 2. Core regression tests

In `core_contract.rs`, using the helpers of the neighbouring tests:

- `busy_report_exposes_a_growing_retry_delay`: three `camera_status busy` reports in a row (each followed by `unknown` to model the retry) give `retry_in_ms` 30000, 60000, 300000, then 300000 again.
- `retry_clears_busy_and_samples_after_one_second`: `busy`, then `camera_status unknown` with the current generation gives mode `face`, `retry_in_ms` null and `next_sample_in_ms` 1000.
- `unavailable_observation_also_schedules_a_retry`: a `presence_observation` with status `unavailable` and camera status `unavailable` gives `retry_in_ms` 30000.
- `accepted_sample_resets_the_retry_backoff`: after two failures, a present sample, then a new `busy` gives 30000 again. Also assert that a resume event (display wake) resets it.
- `permission_and_error_do_not_schedule_a_retry`: both give `retry_in_ms` null and stay `suspended`.
- `retry_is_null_when_presence_is_off_or_battery_suspended`.
- A diagnostics test that `camera_failure` and `camera_retry` appear in `tv_diagnostics_drain` output without any camera identifier.
- Existing `resume_clears_low_light_and_busy_status_but_keeps_permission` must still pass unchanged.

### 3. macOS shell: retry timer, decode, label

- `CoreBridge.swift:327`: add `var retry_in_ms: UInt64?` to `CameraSnapshot`.
- `PresenceCamera.swift`: add a `retryTimer` and its epoch, on the camera queue. In `apply()`, before the `guard self.mode == "face" || ...` early return:
  - when `retry_in_ms` is non-nil and no retry timer is pending, or the generation changed, arm a one-shot `DispatchSource` timer (same style as `scheduleSample`) for that delay;
  - when it fires on the same generation and epoch, clear it and call `resendUnknownStatusIfNeeded()`;
  - when `retry_in_ms` is nil, cancel the timer;
  - an unrelated snapshot with the same pending retry must not postpone the timer.
- Cancel the retry timer in `shutdown()`.
- Keep the `.debug` lines as they are; the new visibility comes from the core ring.
- `Localization.swift`: `cameraStatus.busy` becomes "no image" (en-US) and "sem imagem" (pt-BR). Check for tests or docs that quote the old text (`CameraLocalizationTests.swift` does not assert it).
- `CoreBridgeTests.swift`: a snapshot JSON with `retry_in_ms` decodes to the value, and a snapshot without it decodes to nil.

### 4. Windows shell: retry timer, decode, label

- `JsonCodec.h/.cpp`: add `std::optional<double> retryInMs` to `camera` and parse `retry_in_ms` next to `next_sample_in_ms`.
- `CameraLifecycleDriver`: add a retry timer id member, like `sampleTimer_`. In `onSnapshot`, with the same rules as macOS (arm when non-null and not pending or generation changed, cancel when null, no postponing). The callback, under the driver lock, re-checks the generation and sends `camera_status unknown` through the same path used by `onDevicesChanged` (`:198-201`), then clears the timer id. Cancel it in `shutdown` and wherever `cancelPolicyTimers` runs on a generation change.
- `Strings/en-US/Resources.resw` and `pt-BR/Resources.resw`: `cameraStatus.busy` to "no image" and "sem imagem". Keys must stay identical across locales (`strings_keys.cpp`).
- Tests: `json_codec.cpp` parses `retry_in_ms`; `camera_lifecycle_driver.cpp` with the fake scheduler: after a busy snapshot with `retryInMs`, advancing the clock sends exactly one `camera_status unknown`, a repeated identical snapshot does not move the deadline, a snapshot with null cancels it, and `shutdown` cancels it. Do not rely on the test at `:470-486`, which exercises a device-change path the real backend cannot reach after a busy.

### 5. Docs and decision record

- `api.md:38`: add `retry_in_ms` (null unless the status is `busy` or `unavailable`) and state that the shell sends `camera_status unknown` when it elapses. `api.md:218` stays correct.
- `ARCHITECTURE.md`: new section with the date of the change, in the style of the existing ones, plus a pointer next to `:215`.
- `adr/0036-camera-failure-retry-in-the-core.md` as described under New Files; add `camera_failure` and `camera_retry` to `adr/0034-local-diagnostic-logs.md`.
- `docs/user-guide.md`: say that a camera that stops answering is retried after 30 seconds, then 1 minute, then every 5 minutes, that "no image" can mean a stalled USB camera, and add the two events to the diagnostic log description.
- `CHANGELOG.md`: under Unreleased, "Fixed": the camera is retried after a failure on macOS and Windows; "Changed": the status text "busy" is now "no image". Mark hardware checks as pending.

### 6. Run the Validation Commands

Orchestrator only: run every command below, review the full diff and compare it with this plan.

## Implementation Status

Done on 2026-10-23, with one change to the plan: steps 1-2, 3 and 4 ran in parallel instead of core first, because the shells depend only on the JSON field name `camera.retry_in_ms`.

- [x] 1. Core: failure counter, backoff and snapshot field (`core.rs`). `camera_retry` is logged on every busy/unavailable to unknown transition, so a device-change `unknown` logs it too, and the core sets `resume_pending` on that path as well.
- [x] 2. Core regression tests: 7 new tests in `core_contract.rs`, all passing.
- [x] 3. macOS shell: decode, retry timer, label. Verified by a decode check of `30000.0` into `UInt64?` (the core emits an `f64`) and by building the app.
- [x] 4. Windows shell: codec, driver retry timer, label, 5 driver tests and 2 codec tests. Reviewed by reading only: nothing could be compiled on this Mac. The cancel for the retry timer lives in `onSnapshot`, not in `cancelPolicyTimers`, because `stopCapture` runs on every suspended snapshot and would postpone the retry.
- [x] 5. Docs: ADR 0036, amendments to ADR 0034 and ADR 0022, `api.md`, `ARCHITECTURE.md`, user guide, CHANGELOG.
- [x] 6. Windows tests (`tests.exe`) pass (777 tests, 0 failed, 2026-10-27), and the manual hardware check from Validation Commands was marked complete by the owner on 2026-10-27, with no values recorded.

## Validation Commands

Reproduce before the fix (core test fails to compile or fails on the missing field) and after (passes):

- `cargo test -p telaviva-core --test core_contract -- busy_report_exposes_a_growing_retry_delay retry_clears_busy_and_samples_after_one_second unavailable_observation_also_schedules_a_retry accepted_sample_resets_the_retry_backoff permission_and_error_do_not_schedule_a_retry`
- `cargo fmt --all -- --check`
- `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu`
- `cargo clippy --workspace --all-targets --locked -- -D warnings`
- `cargo test -p telaviva-core --test version_consistency`
- macOS shell (needs `cargo build --release --target aarch64-apple-darwin -p telaviva-core` first): `tools/tests/test-core-bridge.sh`, `tools/tests/test-camera-lifecycle.sh`, `tools/tests/test-camera-lifecycle-bridge.sh`, then `./scripts/build-macos.sh`.
- Windows shell (on Windows only): `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1`, which runs `tests.exe`. The Windows edits cannot be compiled on this Mac, so they stay unverified until CI or a Windows run passes; say so in the final report.
- Manual on the Mac, after building: start the app, make the camera stall as in Steps to Reproduce, and confirm with `/usr/bin/log show --last 10m --predicate 'subsystem == "com.telaviva.oled"'` the `camera_failure` line, then `camera_retry` after about 30 s and a restored "Camera: functioning". Before the fix, the same stall never recovers.

## Notes

- Verified claims: the core and shell code paths above were read directly (`core.rs:255-286`, `:1811-1869`, `PresenceCamera.swift:148-210`, `CameraLifecycleDriver.cpp:165-180`). The Windows and macOS findings on test coverage come from agent reports and were not all re-checked.
- Gap: `PresenceCamera.swift` is not compiled by any Swift test (the suites are standalone `swiftc` binaries), so the macOS timer is covered only by the manual check. The timer logic is kept deliberately small. Extracting it into a testable type would be a larger change and is out of scope.
- The hardware stall itself (USB isochronous errors) is not something the app can prevent. The fix makes the app recover from it; it does not stop it.
- Out of scope, found while reading: `error` is cleared only by a camera change, presence off or battery (`core.rs:797`, `:941`), and the macOS shell never sends `unknown` for it; the comment at `core.rs:966-967` and `api.md:218` say `permission` and `error` are kept, but `core.rs:938-941` clears them on battery or presence-off; the Windows driver ignores a stream that only produces warmup frames (`CameraLifecycleDriver.cpp:237-245`); and an ABI fault is shown to the user as "busy" (`CameraLifecycle.swift:102-112`). Worth separate tickets.
- Backoff values (30 s, 60 s, 300 s) are a proposal. Each retry costs one camera session start and a few frames, so the 5-minute floor keeps the LED and battery cost small.
- Hardware verification was pending on both platforms when this was written, as with the other recent changes; do not claim it in the changelog. The owner marked the Windows hardware check complete on 2026-10-27.
- Execution follows the global rule: Haiku agents edit, the orchestrator verifies. Plan for 1 agent for step 1 and 2 (same files), then 2 in parallel for steps 3 and 4, then 1 for step 5, each announced before it is launched.
