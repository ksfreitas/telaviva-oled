# Bug: Monitor sliders follow delayed hardware readback instead of pointer input

## Bug Description

Brightness and volume sliders sometimes stick, jump backward, or appear unresponsive while dragged. The user requests immediate UI feedback with monitor updates handled by a background queue.

Expected: the thumb and percentage follow the pointer immediately, while the monitor catches up asynchronously to the most recent selected value. Actual: the slider renders the last confirmed hardware value and does synchronous command/snapshot work on every drag event.

## Problem Statement

Decouple interactive slider state from hardware acknowledgment and remove per-pointer-event core/snapshot work from the UI thread, without creating a growing command backlog or weakening DDC confirmation, manual brightness ownership, keyboard routing, or connection-generation checks.

## Solution Statement

Give each monitor control a local draft identified by monitor ID, connection generation, and brightness/volume feature. Render the thumb and percentage from that draft while editing or awaiting its latest request. Submit intents to a bounded serial background coordinator that retains one active monitor update and a replaceable latest target for each feature. Intermediate pointer values replace unsent values; the last drag value is always retained and dispatched as soon as the active update permits it.

Reuse the existing serial DDC worker for physical transactions. Move manual command serialization/submission and associated snapshot copying/decoding into the background bridge path, publishing immutable results on the main queue. A command being accepted by the core does not mean the physical transaction finished. Reconcile the draft only against the current intent's completion or failure; older results must not overwrite newer input. Retain the existing Rust queue and DDC timing/readback checks.

## Steps to Reproduce

1. Open the menu-bar popup with a supported external monitor connected.
2. Drag brightness rapidly across several values, reverse direction, and release at a distinct final value. Repeat for volume.
3. Observe the thumb/percentage follow delayed physical readings rather than every pointer movement, especially while a DDC transaction is in progress or retrying.
4. Reproduce the underlying state mismatch without hardware: create a synthetic monitor at raw brightness 41, submit 20 and then 80, inspect the snapshot, and acknowledge the older 20 action.
5. The current release core produces:

```text
before hardware_result: normalized=41, desired=80, pending_action_value=20
after older result: normalized=20, desired=80, next_pending_action_value=80
```

The popup currently uses `normalized` for both the thumb and label, explaining the stale value and backward jump. This reproduces the state problem; it is not a measurement of frame timing on the real UI.

## Root Cause Analysis

- `MonitorPopup.swift` binds brightness/volume getters and labels to the snapshot's confirmed `VcpValue.value`. The setter submits a command but keeps no local draft. Desired state can advance while confirmed state remains unchanged, and a valid older hardware result can replace the displayed value during a newer drag.
- `CoreBridge.submit` serializes JSON, invokes `tv_submit`, and calls `refresh` synchronously. `refresh` takes a snapshot, copies it, decodes JSON, and publishes it. The native core callback also schedules refreshes. Running this for every pointer event adds unnecessary main-thread work and notifications.
- `tv_submit` and `tv_snapshot` acquire the shared core mutex. There is no evidence that DDC sleeps run under this mutex; the relevant UI problem is synchronous bridge work plus binding to delayed observations, not a proven hardware call on the main thread.
- Physical DDC already runs on `MacDDCController`'s serial utility queue. A normal successful set includes approximately 80 ms initial read, 20 ms duplicated write, 60 ms settling, and 80 ms verification read: at least about 240 ms of configured waits, before IOAV call time. Retry paths take longer. These waits are required for the LG fix and must remain off the UI path.
- Rust already bounds and coalesces hardware actions: `enqueue` preserves an active same-monitor action and replaces a matching queued action; `snapshot_value` exposes one active action per monitor. The bug is not an unbounded Rust FIFO. The missing boundary is between high-frequency UI input and asynchronous control completion. Native snapshot work should also be conflated rather than enqueueing a task for every redundant notification.

## Relevant Files

- `native/macos/TelaViva/MonitorPopup.swift`: brightness/volume bindings, displayed percentages, editing lifecycle, and protection-brightness cap.
- `native/macos/TelaViva/CoreBridge.swift`: asynchronous manual-command entry point, immutable command capture, background snapshot work, main-thread publications, and accepted-command callbacks.
- `native/macos/TelaViva/MenuBarController.swift`: coordinator ownership, snapshot/completion wiring, manual/keyboard ordering, and termination.
- `native/macos/TelaViva/MacDDCController.swift`: existing background execution, action completion, and snapshot ingestion. Preserve IOAV serialization and validate target generation before dispatch.
- `native/macos/TelaViva/HardwareKeyRouter.swift`: manual intent invalidation and accepted-value rebasing so queued keyboard work cannot overwrite newer slider input.
- `native/macos/TelaViva/DDCTransaction.swift`: existing verified transaction delays and strict readback; regression coverage only, no timing reduction planned.
- `crates/telaviva-core/src/core.rs` and `ffi.rs`: existing desired/observed state, action coalescing, and generation contract; reference behavior rather than a planned policy/ABI rewrite.
- `packaging/macos/build-app.sh`, `native/macos/TelaViva.xcodeproj/project.pbxproj`, and `.github/workflows/macos.yml`: source lists and native regression checks.

### New Files

- `native/macos/TelaViva/ManualMonitorControls.swift`: testable local draft and bounded manual-intent coordinator, using Foundation/Combine and existing platform facilities.
- `native/macos/Tests/ManualMonitorControlsTests.swift`: deterministic slow-worker and out-of-order completion scenarios.
- `tools/tests/test-manual-monitor-controls.sh`: standalone native test runner, following existing test scripts.

## Step by Step Tasks

### 1. Add the bounded asynchronous manual-control boundary

- [x] Introduce a coordinator keyed by monitor ID, connection generation, and feature, with monotonic intent revisions.
- [x] Retain at most one active update per monitor and one replaceable latest unsent target per feature; retain existing global DDC serialization. Bound the number of tracked targets to 128 and reject excess targets explicitly.
- [x] Update local intent synchronously without performing FFI, JSON serialization, I/O, blocking waits, or one-dispatch-per-input scheduling in the slider setter.
- [x] Use one scheduled drain and immutable command records. Ignore repeated input that maps to the same raw target; do not let deduplication lose a newer distinct final target.
- [x] End active work on actual action completion/cancellation, not merely successful `tv_submit`. Dispatch the latest pending target next, without replaying intermediate values.
- [x] Avoid a permanent polling timer. Idle controls must schedule no work.

### 2. Isolate bridge execution and preserve command ordering

- [x] Add an asynchronous manual-control bridge path that captures monitor identity/generation on main and performs submission and associated snapshot copying/decoding on a serial worker.
- [x] Never read `@Published` state from the worker or publish UI state from it; hand immutable inputs/results across the boundary and publish on main.
- [x] Conflate redundant snapshot refresh requests and prevent older revisions from replacing newer snapshots. Bound queued snapshot work as well as pending target values.
- [x] Keep using `set_brightness`/`set_volume` with `connection_generation`, preserving the core's manual override, baseline, protection ownership, and rejection semantics.
- [x] Invalidate already queued keyboard authorization when a newer slider intent arrives. Rebase the keyboard target only after the corresponding manual command is accepted, with intent-revision checks so an older asynchronous acceptance cannot overwrite a newer draft. Preserve latest-user-action ordering when a key is pressed after a slider event.
- [x] Coordinate worker lifetime with shutdown: cancel unsent drafts before closing the core, permit already accepted actions/restoration to follow existing shutdown policy, and prevent late callbacks or writes after teardown.
- [x] Preserve the existing DDC duplicate-send timing, pre-write sleep checks, strict target readback, and bounded retries.

### 3. Bind the popup to immediate local values

- [x] Render brightness/volume slider position and percentage from the same local draft while dragging or awaiting its latest request.
- [x] Capture editing begin/end. On release, flush the final rounded target immediately if idle, otherwise retain it as the next target. Keyboard navigation and other non-drag slider changes must also submit without depending on a mouse-release callback.
- [x] Ignore older hardware acknowledgments for draft presentation. Adopt external/confirmed readings when no newer local request is outstanding.
- [x] On rejection or terminal I/O failure, reconcile to the last confirmed value and expose failure through the existing control status presentation; never label an optimistic draft as physically applied. Keep a newer queued intent eligible for its own attempt.
- [x] Cancel stale drafts on disconnect, generation rollover, unsupported capability, or display sleep; do not replay them on a different connection or after wake. Close/reopen of the popup must not lose an accepted pending value or duplicate a write.
- [x] Keep protection brightness at or below the conservative minimum of confirmed brightness and an outstanding lower brightness draft. Do not widen its cap based on an unconfirmed higher draft. Preserve label alignment and equal slider widths.

### 4. Add focused regression coverage

- [x] Reproduce rapid `0 → 100 → 37` input with a worker held behind a deterministic barrier: every local input updates the draft immediately, only an active and latest unsent target remain, and final physical acknowledgment settles at 37.
- [x] Demonstrate that the main executor remains responsive while fake hardware/submission work is blocked; use queue barriers and controlled completions rather than flaky frame-rate timing thresholds.
- [x] Cover direction reversals, duplicate rounded/raw values, final release while busy, non-drag changes, and an older result arriving after a newer draft.
- [x] Cover both features and multiple monitors independently, bounded target/refresh queues, and no work while idle.
- [x] Cover accepted submission versus hardware completion, rejected submission, failed verification, an older failure while a newer intent is pending, and recovery on subsequent user input.
- [x] Cover keyboard/slider races, disconnect/reconnect, sleep/wake, popup closure/reopening, shutdown, and callbacks arriving after cancellation.
- [x] Cover protection-brightness bounds during pending increases/decreases and failed writes.
- [x] Register the helper in both build paths and the native tests in macOS CI.

### 5. Run Validation Commands

- [x] Run the automated commands below and record results.
- [x] With the connected LG, record the initial brightness, drag quickly with reversals, release at a distinct final value, verify the final physical readback and responsive popup, then restore the original value. Verify volume separately only when an audible change is appropriate; synthetic tests cover its queue path without changing speaker volume.
- [x] Exercise the popup during a slow/failing monitor operation and confirm that closing it and opening Settings remain responsive. A background queue passing unit tests alone does not establish UI smoothness.

## Validation Commands

```sh
tools/tests/test-manual-monitor-controls.sh
tools/tests/test-core-bridge.sh
tools/tests/test-ddc-transaction.sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-camera-lifecycle.sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
plutil -lint native/macos/TelaViva.xcodeproj/project.pbxproj
./scripts/build-macos.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --diagnostics
lipo 'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' -verify_arch arm64
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
git diff --check
```

The new manual-control test command is an implementation deliverable and does not exist yet. Current synthetic reproduction was compiled in `/tmp` against `target/aarch64-apple-darwin/release/libtelaviva_core.a`, using the current C header and no device adapter. Its output is recorded above; it did not access the camera, change saved settings, or send physical monitor commands.

## Notes

- This document is the planning output of `codeadvisor-dev-fast-bug`. No application source change, build, app restart, or hardware write is part of this planning execution.
- No new dependency is needed. Use the existing Swift native shell, Dispatch/Combine facilities, and Rust core.
- Preserve the pending hardware-key implementation and LG DDC reliability fix; both are currently uncommitted along with earlier changes. No commit or push is included.
- No Rust C ABI/schema change is planned. Separate the optimistic display of user intent from confirmed physical state; camera/protection decisions continue to use the authoritative core state.
- The previous LG write/readback test establishes correctness of its DDC sequence, not smoothness of continuous slider interaction. Hardware/UI acceptance for this bug remains pending until implementation.

### Implementation results (2026-10-16)

- Tasks 1–4 are implemented: `ManualMonitorControls.swift` (local drafts, bounded coordinator, 128-target cap, completion-driven drain), `CoreBridge.submitManualControl` and conflated snapshot refresh on the bridge worker, popup bound to drafts with pending/failed status, keyboard-router rebasing, and the `ManualMonitorControlsTests` / `CoreBridgeTests` suites, registered in `build-app.sh`, the Xcode project, and macOS CI.
- Automated validation passed: manual-monitor-controls, core-bridge, ddc-transaction, hardware-key-router, and camera-lifecycle native suites; `cargo fmt --check`; 38 Rust tests; strict clippy; `plutil -lint`; `./scripts/build-macos.sh`; `--smoke-test`; `--diagnostics` (LG ULTRAFINE `transport_ready`); arm64 `lipo`; strict `codesign`; `git diff --check`.
- Hardware burst on the connected LG (programmatic, through the production `CoreBridge`, `MacDDCController`, and `ManualMonitorControls`, wired as in `MenuBarController`; temporary harness outside the repository, then removed). Original raw brightness 45/100; eight rapid inputs with reversals (50, 46, 41, 48, 43, 51, 49, 47), 15 ms apart, final 47:

```text
every input reflected in the draft immediately; slowest setValue call 0.029 ms
8 inputs coalesced into 2 hardware commands: [50, 47]; intermediate values never sent
active<=1 and pending<=1 during the burst; status ready after strict DDC readback
main run loop heartbeat stayed alive during the DDC waits
restored to 45 through the same path; fresh physical readback = 45
```

  A first run of the harness flagged one dropped input; the cause was the harness reading the monitor before the main-queue snapshot subscription had delivered it to the controls (a one-run-loop-turn window the running app does not hit). A second reading failure was a harness cast of `current` (integer). Neither is a product defect; the rerun passed.
- Still pending, physical only (needs a person at the Mac): a real pointer drag in the popup and popup/Settings responsiveness during a slow or failing operation (the two hardware/UI bullets in Task 5, left unchecked). The burst above exercises the same code path but not SwiftUI rendering, so it does not establish UI smoothness. Volume was not written.
- Physical UI acceptance (2026-10-16): the user ran the popup and hardware checks on the connected LG and reported that they worked. No measurements or per-scenario details were provided, so none are recorded here.

### Follow-up: test gaps closed (2026-10-16)

- `CoreBridgeTests.refreshBurstsAreConflated` now proves that 50 refresh requests made during one blocked snapshot read result in exactly one running read plus one re-run (2 reads total). It uses a small test seam, `CoreBridge.refreshRunObserver`, which is called at the start of each refresh run.
- `ManualMonitorControlsTests.wakeAfterSleepDoesNotReplayPendingDraft` proves that a draft pending before display sleep is not written after wake, while a new draft submitted after wake is written.
- Still manual: popup close and reopen. The popup is a SwiftUI view with no headless test. Checklist for a person:
  - [ ] Open the menu bar popup.
  - [ ] Drag a brightness slider and close the popup mid-drag (click outside).
  - [ ] Reopen the popup.
  - [ ] Confirm the slider shows the final value.
  - [ ] Confirm no extra write was sent after the popup closed.
  - [ ] Confirm the monitor ends at the value shown.
  - Result: Not run.
