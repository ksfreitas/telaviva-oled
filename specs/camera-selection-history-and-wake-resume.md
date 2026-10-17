# Chore: Camera selection history and faster presence resume after wake

## Chore Description

Two related camera-lifecycle improvements, both reported by the user who moves the Mac between two locations with different webcams (webcam A at home, webcam B at work).

### 1. Camera selection history with fallback

Today the selected camera is a single optional `uniqueID` (`selected_camera_id`). When the user returns home and webcam B is no longer connected:

- `PresenceCamera.startCapture()` silently falls back to the system default camera, but the stored ID stays dangling.
- The settings picker has no tag matching the stored ID, so it shows "no selection".
- Webcam A, which the user had selected before, is never considered again.

Required behavior:

- The app remembers the cameras the user selected, most recent first (a bounded MRU history).
- At capture time and in the picker, the effective camera is the first history entry that is currently connected. If none is connected, the effective camera is the system default.
- Selecting "Use default camera" clears the history, so the app follows the system default from then on.
- Selecting a device moves it to the front of the history (deduplicated, bounded).
- A temporarily missing camera never rewrites the stored history. When B is reconnected it is used again, because it is still at the front.

### 2. Slow start of presence reading after wake

The investigation (see the confirmed findings below) shows that after `screensDidWake` the first presence sample is not taken immediately. The camera is only sampled after a full `presence_sampling_interval_s` (default 10 s), the timer restarts on every display-state change during wake, and stale state carried across sleep can add one or two more cycles.

Confirmed by code:

- `PresenceCamera.swift:83-93` schedules the first sample at `snapshot.camera.next_sample_in_ms`, which `core.rs:975-987` sets to `presence_sampling_interval_s * 1000` (default 10 s) in face mode.
- `core.rs:369-370` bumps `camera_generation` on every `monitor_display_state`; `PresenceCamera.swift:55-63` cancels and reschedules the pending sample, and `core.rs:551-552` drops an in-flight reading whose generation is stale.
- `low_light` (`core.rs:592`, cleared only after two usable samples at `core.rs:614-619`) is not reset by sleep, so a dark reading before sleep puts the wake path in the `light_only` cycle (about 40 s to the first face reading).
- A `busy` or `unavailable` camera status (`PresenceCamera.swift:150, 193-194`) is sticky; only a camera change or toggling presence protection clears it (`core.rs:514, 627-631`). A camera that is slow to re-enumerate after wake can leave the camera suspended with no retry.
- The ONNX detector is released 30 s after leaving face mode (`PresenceCamera.swift:285`), so the first face frame after a longer sleep pays session creation (`crates/telaviva-inference/src/lib.rs:109-177`).

Required behavior:

- When the camera mode resumes from `suspended` (display wake, power returning to AC, presence protection re-enabled), the first sample is scheduled after a short fixed delay (1 s) instead of a full interval. Subsequent samples keep the configured interval.
- The resume edge clears stale `low_light` and sticky `busy`/`unavailable` status so the first reading starts from a clean state.
- If a face-mode sample's frame is too dark (`insufficient_light`), the first dark result is not reported. The camera stays on for 5 s, frames are discarded while exposure settles, and one sample is taken at the end of the 5 s. Only that final sample is reported: usable light goes through normal face detection, still dark reports `insufficient_light` as today (so `low_light` and the `light_only` cycle only start after the settled sample is also dark). This applies to every face-mode sample, not only the first one after wake.
- The detector is pre-warmed when face mode becomes active so session creation overlaps with camera start.
- Wake-to-reading timestamps are logged so the improvement can be measured on hardware (the wake acceptance row is still "Not run" in `specs/archived/camera-lifecycle-and-display-suspension/validation/macos-acceptance.md:14`).

### Design decisions

- `selected_camera_id` stays and is always the front of the history. The new `camera_history` field is an additive optional settings key: schema 1 and ABI 1 are unchanged, per the convention in `api.md:27` and `adr/0003-camera-lifecycle-and-display-suspension.md:27`. Settings travel as JSON, so no C signature changes.
- The core owns the history ordering (it already owns persistence and validation). Swift owns resolution against connected devices, because only Swift can enumerate `AVCaptureDevice`. Resolution is a pure function so it can be tested with the standalone `swiftc` scripts.
- A fallback is a runtime decision, never persisted.

## Relevant Files

- `crates/telaviva-core/src/settings.rs`: `Settings` struct (`selected_camera_id` at lines 35-36, default at 75), `validate()` (lines 95-102, `deny_unknown_fields` at line 9). Add `camera_history` with serde default, validation and a bound constant.
- `crates/telaviva-core/src/core.rs`: `update_settings` (437-521, camera handling at 488-498 and 510-518) maintains the history. `display_state` (344-372), `reconcile_camera` and `camera_mode` (648-664), camera cadence (975-987), and camera status reset points (514, 627-631) hold the resume logic.
- `crates/telaviva-core/tests/core_contract.rs`: existing settings tests (20-133, 1015-1044) and the place for new history and resume tests.
- `native/macos/TelaViva/CoreBridge.swift`: `CoreSettings` (295-321) must decode `camera_history` and must stop sending `selected_camera_id` unless the user changed it.
- `native/macos/TelaViva/SettingsView.swift`: picker (101-105), device list (line 26, not observed), `load` (167), `save` (170-182). Show the effective camera, observe device connect/disconnect, send the selection only when the user changes the picker.
- `native/macos/TelaViva/PresenceCamera.swift`: `startCapture` (146-151) uses the resolver and the history; `apply` (50-93) sample scheduling; `ensureDetector` (263-274); detector release (276-297). Add connect/disconnect handling and wake timing logs.
- `native/macos/TelaViva/CameraLifecycle.swift`: pure lifecycle policy. Add the face-mode dark-frame settle (reuse the existing `settling = 5` and `.discardWarmup` path at lines 62-82, currently light-only). The resume delay itself stays in the core snapshot.
- `crates/telaviva-inference/src/lib.rs`: read-only reference for the `insufficient_light` result (`MIN_MEAN_LUMA = 18.0`, lines 211-216); no change expected.
- `native/macos/Tests/CameraLifecycleTests.swift`, `tools/tests/test-camera-lifecycle.sh`: pattern for standalone `swiftc` tests; extend the script to compile the new selection file and its test.
- `native/macos/Tests/CoreBridgeTests.swift`, `tools/tests/test-core-bridge.sh`: bridge tests, extend for the camera selection round-trip.
- `api.md` (repository root, line 27 documents additive settings keys): document `camera_history` and the resume sample delay.
- `adr/0003-camera-lifecycle-and-display-suspension.md`: camera lifecycle contract; add a short amendment about history fallback and resume sampling (or a new ADR, following ARCHITECTURE.md process conventions).
- `CHANGELOG.md`: Unreleased entries.
- `include/telaviva_core.h`, `crates/telaviva-core/src/ffi.rs`: read-only reference. ABI version stays 1; do not edit.

### New Files

- `native/macos/TelaViva/CameraSelection.swift`: pure, dependency-free resolver `resolveCamera(history:connected:) -> String?` returning the first history ID present in `connected`, or nil for the system default. Also a helper that produces the picker value.
- `native/macos/Tests/CameraSelectionTests.swift`: standalone tests for the resolver, compiled by `tools/tests/test-camera-lifecycle.sh` (or a sibling script following the same pattern).

## Step by Step Tasks

### Step 1: Add `camera_history` to the Rust settings

- In `settings.rs`, add `pub const MAX_CAMERA_HISTORY: usize = 8;` and `#[serde(default)] pub camera_history: Vec<String>` to `Settings`; default is an empty vec in the `Default` impl.
- In `validate()`, reject any entry that is empty or longer than 512 bytes, any duplicate, and a length above `MAX_CAMERA_HISTORY`.
- In `load()`, normalize legacy files: a file with `selected_camera_id` set and an empty history gets `camera_history = [selected_camera_id]` (in memory, persisted on the next store). Do this before validation so old files do not hit the corrupt-file backup path.
- Invariant to enforce and test: `selected_camera_id == camera_history.first()`; both empty/`None` means system default.

### Step 2: Maintain the history in `update_settings`

- In `core.rs` (488-498), when the payload contains `selected_camera_id`:
  - `null`: set `selected_camera_id = None` and clear `camera_history`.
  - a string: remove it from the history if present, insert at index 0, truncate to `MAX_CAMERA_HISTORY`, and set `selected_camera_id` to it.
- When the payload sets the same ID that is already at the front, treat it as a no-op (no `camera_changed`, no generation bump).
- Do not accept `camera_history` directly from the payload (read-only for clients) so Swift cannot desynchronize it from `selected_camera_id`. Reject or ignore the key, matching how other read-only keys are handled in this function.
- Keep `core.rs:510-518` behavior: a change of the effective front resets `camera_status` to `unknown` and bumps `camera_generation`.
- The effective camera can change at runtime without a settings change (device connects or disconnects). That is handled in Swift in Step 6, not in the core.

### Step 3: Resume-from-suspended sampling in the core

- In `core.rs`, track the previous camera mode. When `camera_mode` transitions from `suspended` to `face` or `light_only` (after `display_state`, power change, or presence-protection re-enable), set a `resume_pending: bool`.
- On that edge also: set `low_light = false`, and if `camera_status` is `busy` or `unavailable`, reset it to `unknown`. Do not reset `permission` (needs user action) or `error`.
- In the snapshot cadence code (975-987), when `resume_pending` and the mode is `face`, report `next_sample_in_ms = RESUME_FIRST_SAMPLE_MS` (new constant, 1000). For `light_only` keep its existing fixed cadence. Clear `resume_pending` when a camera report is accepted for the current generation.
- Do not change `presence_sampling_interval_s`, its range, or the steady-state cadence.

### Step 4: Rust regression tests

In `crates/telaviva-core/tests/core_contract.rs`:

- Settings: default has an empty history; validation rejects duplicates, empty entries, oversized entries and more than 8 entries; legacy file with only `selected_camera_id` loads with a one-entry history and is not backed up as corrupt.
- `update_settings`: selecting A then B gives history `[B, A]`; reselecting A gives `[A, B]`; selecting `null` clears history and selection; selecting the current front is a no-op (generation unchanged); the history is capped at 8; a payload with `camera_history` does not change it; persisted file round-trips the history.
- Resume: after all eligible displays sleep and then wake, the snapshot reports `next_sample_in_ms == 1000` once, then the configured interval after an accepted report; `low_light` and `busy` status are cleared on the resume edge; `permission` status is not.

### Step 5: Pure Swift resolver and its tests

- Create `CameraSelection.swift` with `resolveCamera(history: [String], connected: Set<String>) -> String?` (first history ID that is connected, else nil).
- Create `Tests/CameraSelectionTests.swift` covering: front connected, front missing and second connected, none connected (nil, system default), empty history (nil), duplicates ignored.
- Extend `tools/tests/test-camera-lifecycle.sh` (or add a sibling script with the same structure) to compile and run it, and add the script to the `native-arm64` job in `.github/workflows/macos.yml` if a new script was created.

### Step 6: Use the history in the Swift shell

- `CoreBridge.swift`: add `camera_history: [String]` to the decoded settings (default empty so old snapshots decode). Change `CoreSettings.json` so `selected_camera_id` is included only when the caller marks it as changed (for example an explicit `cameraSelectionChanged` flag or a dedicated `selectCamera(_:)` command wrapper), so unrelated saves never re-send it and never promote a fallback camera.
- `PresenceCamera.swift` `startCapture()`: enumerate connected devices (same `DiscoverySession` types as the picker, or `AVCaptureDevice(uniqueID:)` per history entry), call `resolveCamera`, and use the result; fall back to `AVCaptureDevice.default(for: .video)` only when it returns nil. Keep the existing `unavailable` report when no device can be opened.
- `PresenceCamera.swift`: observe `AVCaptureDevice.wasConnectedNotification` and `wasDisconnectedNotification`. When the active device disconnects, or a device that outranks the active one in the history connects, stop the current capture and let the existing `apply` path restart it, so a hub reconnect or an unplug is picked up without waiting for a settings change. On a disconnect also clear the sticky `busy` status path by re-evaluating through the normal snapshot flow; do not add a new core command unless the existing flow cannot do it (if it cannot, add a `camera_status` sender for `unknown` using the existing command at `core.rs:182-212`).
- `SettingsView.swift`: make the camera list `@State` refreshed on connect/disconnect notifications (it is currently a computed property and never refreshes). The picker shows the effective camera from `resolveCamera` (or "Use default camera" when nil), so it never shows a blank selection. Selecting a device or the default option is the only path that sends `selected_camera_id`.

### Step 7: Pre-warm the detector and add wake timing logs

- `PresenceCamera.swift`: when `apply` sees the mode become `face` (and `detector == nil`), call `ensureDetector()` on the camera queue so ONNX session creation overlaps with camera start. Keep the 30 s release timer unchanged.
- Add `os.Logger` (subsystem `br.com.codebit.telaviva`, category `wake`) timestamps: `screensDidWake` (in `MacDDCController.swift`), each applied `next_sample_in_ms`, `startRunning`, first frame, detector ready, and the accepted presence report. Log at debug level, no PII (device IDs are not logged, only timings and state names).

### Step 8: Dark first frame keeps the camera on for 5 s

- `CameraLifecycle.swift`: extend the pure policy so that a face-mode frame assessed as `insufficient_light`, when no dark settle is active for this session, starts a 5 s settle (`settlingUntil = now + 5`) and tells the caller to keep the session open instead of reporting. While `now < settlingUntil`, frames return `.discardWarmup` (existing path). The first frame at or after the deadline is assessed and its result is final for this sample.
- `PresenceCamera.swift`: for single-frame face samples, do not `stopCapture()` or `report` after the first dark result (`captureOutput`, `defer` at line 229 and the report at 299-305); schedule the end-of-settle check on the camera queue like the light-only path (`PresenceCamera.swift:121`). Report only the final result. Make sure the 5 s acquisition watchdog (`armCaptureTimeout`, 171-198) does not report `busy` because of the settle: it only fires when no first frame was received, and the settle starts after one was, but verify and use the existing `frameTimeout` for a stalled stream during the settle.
- Keep the core unchanged for this step: it only ever sees the final result, so `low_light` (`core.rs:592`) is set only when the settled sample is also dark. If the generation changes during the settle, the existing stale-report drop (`core.rs:551-552`) and session stop apply.
- Add tests in `Tests/CameraLifecycleTests.swift`: first dark frame starts a 5 s settle and is not reported; frames before the deadline are discarded; a usable frame at the deadline proceeds to normal face assessment; a dark frame at the deadline is reported as `insufficient_light`; a second sample later starts a fresh settle; light-only behavior is unchanged.

### Step 9: Swift bridge tests

- `Tests/CoreBridgeTests.swift`: add a camera case through the real core: select A, select B, assert `camera_history == [B, A]`; select default, assert both cleared; confirm a settings save that does not touch the camera leaves the selection and history unchanged.

### Step 10: Documentation

- `api.md`: document `camera_history` (read-only, MRU, max 8, additive to schema 1, ABI 1), the 1 s first sample on resume, and the 5 s dark-frame settle before `insufficient_light` is reported; note that an older binary resets a settings file containing the new key (same caveat as `api.md:27`).
- `adr/0003-camera-lifecycle-and-display-suspension.md`: add an amendment for history fallback and resume sampling, following the ADR conventions in `ARCHITECTURE.md`.
- `CHANGELOG.md`: Unreleased entries for both changes (en-US).

### Step 11: Run Validation Commands

Run every command in the Validation Commands section and fix anything that fails before finishing.

## Validation Commands

Reproduce before (on `HEAD`, with a connected camera) and after:

- Fallback: `cargo test --workspace --locked -p telaviva-core camera_history` (new tests fail before the change, pass after).
- Resume: `cargo test --workspace --locked -p telaviva-core resume` (new tests fail before, pass after).
- Format: `cargo fmt --all -- --check`
- Lint: `cargo clippy --workspace --all-targets --locked -- -D warnings`
- Full Rust tests: `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu`
- Coverage of the core (as in CI): `cargo +stable llvm-cov -p telaviva-core --all-targets --locked`
- Swift resolver and lifecycle: `tools/tests/test-camera-lifecycle.sh`
- Swift bridge (needs the release static library): `cargo build --release --target aarch64-apple-darwin -p telaviva-core && tools/tests/test-core-bridge.sh`
- App build: the one-command Apple Silicon build documented in `packaging/macos/` and `README.md:45-56`.

Manual hardware checks (not automatable here; record results in the validation folder, do not claim them as done until run):

- With webcams A and B: select A, select B, disconnect B, reopen settings and confirm the picker shows A and capture uses A; disconnect A too and confirm the system default is used and shown; reconnect B and confirm B is used again without reselecting; select "Use default camera" and confirm `settings.json` has `selected_camera_id: null` and an empty `camera_history`.
- Dark frame: cover the lens, wait for a face sample, and confirm in the `wake` logs that the camera stays on about 5 s, only one result is reported at the end, and `low_light` is set only if the lens is still covered at the end. Uncover it during the 5 s and confirm normal face detection is reported instead.
- Wake: put the display to sleep, wake it, and read the `wake` log category (`log stream --predicate 'category == "wake"'`). Compare wake-to-accepted-report against the previous build; expected first sample about 1 s after the awake snapshot instead of about 10 s.

## Notes

- This was filed as a bug request but reclassified by the user as a chore; the plan follows the chore format.
- Not an ABI or schema bump: an additive optional settings key in schema 1. Because `Settings` uses `deny_unknown_fields`, a binary older than this change will back up and reset a settings file written by the new one. This is the same accepted trade-off as the key-step settings.
- Dark frames: taking the first face sample 1 s after wake means the camera may deliver a still-dark frame during auto-exposure. Step 8 handles this by keeping the camera on for 5 s and sampling at the end, so a cold-start dark frame no longer flips the app into the `light_only` cycle (`inference/src/lib.rs:211`, `MIN_MEAN_LUMA = 18.0`). This also closes the gap with the archived architecture spec, which says quality should account for exposure settling. Cost: in a genuinely dark room each face sample now holds the camera for about 5 s before reporting; the camera-on time per sample rises accordingly, which matters for the battery goal in `AGENTS.md`.
- Other hypotheses from the investigation that are not addressed here because they are unconfirmed or unmeasured: DDC read backoff in front of the awake report (`DDCTransaction.swift:23-37`, up to about 3.4 s for a new display), `CGDisplayIsAsleep` lagging at the wake edge with no retry, system sleep without a prior `screensDidSleep`, and ONNX runtime initialization cost. Revisit them with the Step 7 logs.
- Swift UI files (`PresenceCamera.swift`, `SettingsView.swift`) have no automated test target. Their behavior is covered by the pure resolver tests, the bridge test, and the manual checks above.
- `AGENTS.md` asks for implementation to be delegated to Luna agents (`gpt-6-luna`), while the user's global rules require Haiku subagents. Resolve which applies at implementation time.
- No new dependencies.

## Implementation Notes

- Resume triggers are events, not a `suspended` to `face` mode transition: an eligible display going from asleep to awake, power returning from battery to AC while battery suspension is on, and presence protection being re-enabled. A sticky `busy` status itself keeps the mode suspended, so a suspended-to-face edge would never fire.
- On a trigger the core clears `low_light`, resets `busy`/`unavailable` camera status to `unknown` (not `permission` or `error`), and reports `next_sample_in_ms` = 1000 for face mode until the first accepted camera report. `light_only` keeps its cadence.
- `update_settings` bumps the camera generation only when the settings actually differ. This is needed for the reselect-front no-op.
- Swift decodes `camera_history` as an optional array (`[String]?`), following the `brightness_key_step` convention. `CoreSettings.json` no longer sends `selected_camera_id`; only `CoreBridge.selectCamera(_:)` does.
- Dark settle applies to `single_frame` face samples only, via `CameraLifecycle.beginDarkSettle(now:)`.
- The `busy`/`unavailable` re-send on device change uses the existing `camera_status` command, because `presence_observation` rejects `unknown`.
- The detector pre-warm runs after the sample timer is scheduled.
- Swift test files live in `native/macos/Tests`; the `native/macos/TelaViva/Tests` paths in the Relevant Files and Steps were wrong and are corrected.
- The new resolver is registered in `packaging/macos/build-app.sh` and the Xcode project.
- Automated checks that passed: `cargo fmt`, clippy, workspace tests, `tools/tests/test-camera-lifecycle.sh`, `tools/tests/test-core-bridge.sh`, `scripts/build-macos.sh`.
- Manual hardware checks (two webcams, wake timing logs, dark frame with covered lens) are still PENDING and not verified.
