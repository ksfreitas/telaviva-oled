# Feature: Camera Lifecycle and Display Sleep Suspension

## Feature Description
Add an opt-in always-on camera mode for macOS presence monitoring, suspend camera monitoring whenever every eligible external display is physically asleep, and give the camera time to settle exposure before each low-light recovery assessment. Show the localized inline status exactly as `Câmera: funcionando` in Portuguese, with lowercase text after the colon. Apply localized inline wording to the other camera statuses too, without lowercasing device names or unrelated standalone labels.

## User Story
As a TelaViva user, I want to keep the camera ready while presence protection is active and have it pause when my external displays sleep, so that presence checks respond predictably while avoiding unnecessary camera use.

## Problem Statement
The macOS adapter currently opens one-frame sessions during ordinary presence checks and retains the session only for return checks. Low-light recovery samples at a 10-second cadence without an explicit exposure-settling period. Camera policy does not observe physical display sleep, so it may keep acquiring frames when all controlled external displays are asleep. An always-on choice must remain subordinate to existing settings, power, monitor eligibility, sleep, shutdown, and camera-error policy.

## Solution Statement
Add a serde-defaulted `always_on_camera` Boolean setting, defaulting to `false`, and expose it in Settings. Keep the core as the source of camera eligibility and generation changes. Add a macOS display-sleep observer that reports per-monitor sleep state to the core, using the existing stable monitor ID to `CGDirectDisplayID` mapping; also query current state at startup and after wake/reconfiguration. Represent state as `unknown`, `awake`, or `asleep` in the core snapshot, and accept a `monitor_display_state` command with `monitor_id`, the existing connection `generation`, and `state`. Suspend acquisition when all eligible external displays are asleep, resume when at least one is awake, and continue when displays have mixed sleep states. Treat brightness zero as unrelated to display sleep. Suppress DDC actions for a known-asleep target through the existing action/ownership flow; do not broaden the hardware-action architecture. Preserve screensaver protection independently.

In always-on mode, retain a healthy camera session and continue the configured inference cadence; do not infer presence merely because the session is running. In the default mode, preserve existing one-frame and return-check session behavior. Keep `next_sample_in_ms` at 10 seconds in light-only mode: wait that retry delay after an assessment, begin a five-second exposure-settling window, then assess a fresh frame. Completed assessments are approximately 15 seconds apart, plus acquisition latency. Ignore warmup frames for both inference and absence counting. Continue requiring two usable-light checks. Measure the exposure window with a monotonic clock. The five-second exposure window is separate from the existing five-second frame-acquisition watchdog: acquisition still fails if no frame arrives within its watchdog, while a delivered warmup frame satisfies initial acquisition but does not satisfy the later light assessment. After settling, arm a fresh bounded frame-acquisition deadline; a frozen stream must not wait forever. Reuse a session already running in always-on mode, applying the same five-second settling window without restarting it. Make all timer completions and camera observations generation-checked so sleep, setting changes, shutdown, or errors invalidate stale work.

## Relevant Files
- `README.md`: current user-facing camera behavior and macOS scope.
- `ARCHITECTURE.md`: product requirements, defaults, and intended camera policy.
- `api.md`: implemented settings, camera snapshot, and command contracts that may need additive fields/statuses.
- `crates/telaviva-core/src/settings.rs`: persisted settings schema with serde defaults and validation.
- `crates/telaviva-core/src/core.rs`: eligibility, camera mode and generation policy, low-light state, monitor action reconciliation, and JSON snapshot.
- `crates/telaviva-core/src/ffi.rs` and `include/telaviva_core.h`: versioned ABI JSON command/snapshot transport; frames remain outside the core.
- `crates/telaviva-core/tests/core_contract.rs`: policy, settings migration, stale-generation, monitor, low-light, and protection contracts.
- `crates/telaviva-core/tests/ffi_contract.rs`: ABI compatibility and JSON transport tests.
- `native/macos/TelaViva/PresenceCamera.swift`: capture session ownership, sample scheduling, watchdog, and inference dispatch.
- `native/macos/TelaViva/MacDDCController.swift`: stable monitor-to-display mapping and per-monitor DDC action dispatch.
- `native/macos/TelaViva/MenuBarController.swift`: native observers, app lifecycle, and shutdown sequencing.
- `native/macos/TelaViva/CoreBridge.swift`: native settings encoding and snapshot decoding.
- `native/macos/TelaViva/SettingsView.swift`: camera preference controls.
- `native/macos/TelaViva/Localization.swift`: localized setting labels and camera status phrases.
- `native/macos/TelaViva.xcodeproj/project.pbxproj`: explicit Swift source references/build phase; update if introducing a lifecycle policy helper or test target.
- `packaging/macos/build-app.sh`: explicit Swift source list; include the lifecycle helper.
- `.github/workflows/macos.yml`: run the native lifecycle tests alongside existing checks.

### New Files
- `native/macos/TelaViva/CameraLifecycle.swift`: small scheduling policy used by the real camera adapter and deterministic tests.
- `native/macos/Tests/CameraLifecycleTests.swift`: fake-clock/session tests without webcam access.
- `native/macos/Tests/CameraLocalizationTests.swift`: exact Portuguese and English inline status assertions.
- `specs/archived/camera-lifecycle-and-display-suspension/validation/macos-acceptance.md`: physical acceptance checklist with unverified results explicitly identified.
- `tools/tests/test-camera-lifecycle.sh`: compile and run those native tests with the macOS SDK.

Keep display-state observation alongside the existing macOS display integration.

## Implementation Plan
### Phase 1: Foundation
Status: [x] Complete.

Implemented additive settings defaults and the generation-checked display-state contract in Rust and Swift. Existing settings files remain readable; older binaries may reject the newly persisted field.
- Add `always_on_camera` to Rust settings and native settings encoding with serde default `false`, preserving older schema-1 settings files. An older binary using `deny_unknown_fields` may reject a file after the new binary persists this field, so downgrade read compatibility is not promised.
- Define core display-sleep input and snapshot behavior, including eligible monitor criteria (stable identity, supported brightness, and usable baseline), startup unknown-state handling, and per-display sleep state.
- Keep always-on subordinate to presence protection enabled, battery suspension, eligible monitor presence, all-eligible-displays-asleep, camera failures, and shutdown.

### Phase 2: Core Implementation
Status: [x] Complete.

Implemented continuous capture policy, per-monitor sleep gating, ownership-checked wake restoration, and the native exposure scheduler. Review added cancellation tokens, monotonic capture timestamps, stale-snapshot rejection, and tests for frozen streams and interrupted recovery.
- Update camera policy so an eligible active session reports `capture_mode: "continuous_monitoring"` when always-on is enabled, while inference remains at the current configured presence/return cadence. Preserve `single_frame`, `continuous_return`, and `stopped` semantics for existing modes.
- Model low-light recovery settling separately from the acquisition watchdog. Keep the retry delay at 10 seconds after the last assessment: at each deadline, ignore frames for five seconds, assess a subsequent frame, and count only valid usable-light results toward the existing two-check recovery threshold.
- Advance camera generations on relevant sleep/wake transitions and setting changes. Ignore old camera status, light assessment, and presence observations after those transitions; reject presence observations whenever effective mode is `disabled` or `suspended`, even when a callback carries the current generation.
- Preserve screensaver state and protection reasons through camera suspension. Do not issue DDC writes for an asleep target; retain desired protection/restoration state and re-evaluate it on wake through current-brightness ownership checks before any write.

### Phase 3: Integration
Status: [x] Complete (implementation and automated validation).

Integrated native display notifications, startup-state reconciliation, the translated setting/status, test harnesses, packaging, and CI. Local Rust/native tests, release packaging, signature verification, and ABI smoke test passed. Physical acceptance remains explicitly pending in the hardware checklist.
- Observe per-display sleep and wake on macOS. Query `CGDisplayIsAsleep` at initial discovery and reconciliation, listen for `NSWorkspace.screensDidSleepNotification` and `screensDidWakeNotification` on `NSWorkspace.shared.notificationCenter`, and reuse the existing `NSApplication.didChangeScreenParametersNotification` refresh path to reconcile display state after reconfiguration. Avoid busy polling.
- Add the always-on toggle and translations in English and Brazilian Portuguese. Compose inline status from localized label and value so every status phrase following the Portuguese label is lowercase (including `Câmera: funcionando`); preserve capitalization for proper names and avoid blanket-lowercasing unrelated text.
- Update API and README behavior documentation. Windows implementation and validation remain deferred.

## Step by Step Tasks
### 1. [x] Extend settings compatibly
Add the `always_on_camera` Boolean to Rust `Settings` (serde default false), its core update-settings parser, native `CoreSettings` JSON, SettingsView state/load/save, and both locale strings. Confirm omitted fields in prior schema-1 JSON retain all prior preferences.

### 2. [x] Add display sleep state to core policy
Add the `monitor_display_state` command (`monitor_id`, matching connection `generation`, and `state` = `unknown`/`awake`/`asleep`) and a `display_state` field on each monitor snapshot. Reject commands whose monitor connection generation is stale. Derive camera suspension from the set of eligible external displays (stable identity, supported brightness, known baseline): suspend only when every eligible display is confirmed asleep; any awake eligible display permits monitoring; unknown state must not trigger a false all-asleep suspension. Exclude brightness value from sleep determination. Keep no-eligible-monitor suspension and existing battery/error precedence. Report `camera.mode: suspended`, `capture_mode: stopped`, null next-sample delay, and `display_status: suspended_display_sleep` for display-sleep suspension. Reject unknown monitor IDs, invalid states, and stale connection generations. Serialize native sleep observations on the display queue so an older query cannot overwrite a newer transition.

### 3. [x] Gate monitor actions during sleep
Prevent dispatch and creation of brightness writes for a known-asleep target, including protection dimming and restoration. On wake, refresh physical brightness and use the existing ownership-check sequence before restoring or applying the desired target. Ensure screensaver protection remains active and is reconciled independently for awake monitors.

### 4. [x] Implement always-on and exposure lifecycle
Adapt `PresenceCamera` scheduling to retain a healthy session only when the setting and core camera mode allow it; preserve existing default lifecycle when disabled. In always-on mode, do not stop/restart a healthy session solely because mode changes between `face` and `light_only`, capture mode changes between `continuous_monitoring` and another active mode, or unrelated settings change while the selected camera and eligibility remain unchanged. Retarget future sample requests to the current camera generation, while results already in flight retain their captured generation and are rejected if stale. A `present` result must not close an eligible always-on session. Turning always-on off restores the existing single-frame/continuous-return policy. At each light-only scheduled verification, discard warmup frames for five seconds and assess only a later frame. Keep the 5-second acquisition watchdog independent. Reuse an already-running session during settling, and invalidate timers, in-flight completion callbacks, and observations on display sleep, disable, errors, camera replacement, and shutdown.

### 5. [x] Integrate macOS sleep/wake observation
Use the display mapping in `MacDDCController` to report each monitor's sleep state. Reconcile on startup, existing `NSApplication.didChangeScreenParametersNotification`, workspace display sleep, and wake. Ensure each asleep target is excluded from action dispatch even when another external monitor remains awake.

### 6. [x] Add deterministic contract and native lifecycle coverage
Add core settings default/old-file compatibility, eligibility, mixed-display, all-asleep, zero-brightness, stale-generation, screensaver-independence, suspended-observation rejection, and asleep-write/restoration tests. Add a small pure-Swift lifecycle scheduling helper and standalone test script (`tools/tests/test-camera-lifecycle.sh`) with controllable clock/session fakes for warmup, watchdog separation, cadence, two usable-light checks, session reuse, timer cancellation, stale callbacks, and shutdown. Compile `CameraLifecycle.swift` and `native/macos/Tests/CameraLifecycleTests.swift` with `swiftc` in that script; assert against the production scheduling policy rather than a duplicated implementation. Wire the helper into explicit Xcode project references and the packaging script source list, and run the test script in `.github/workflows/macos.yml`. Tests must not require a physical camera or monitor.

### 7. [x] Update contracts and acceptance checklist
Document the additive setting and sleep command/snapshot semantics in `api.md`, user-visible behavior and defaults in `README.md`, and manual macOS validation for camera continuity, camera permission/errors, sleep/wake and reconfiguration, mixed display states, DDC suppression on asleep displays, wake ownership checks, low-light recovery, battery suspension, shutdown, and localized status. State that `CGDisplayIsAsleep` reflects the OS-reported state and may not detect a monitor manually powered off while its connection remains logically active.

### 8. [x] Run Validation Commands
Run the repository formatting, Rust workspace, lint, and macOS native build checks listed below. Execute pure policy and fake-session lifecycle tests in CI; use the manual hardware checklist only on supported macOS hardware, reporting results as unverified until run.

## Testing Strategy
### Unit Tests
- Rust settings default false, accept explicit true/false, and load old schema-1 settings without data loss.
- Core camera mode keeps all existing disabling/suspension rules above always-on; no eligible display suspends; all eligible asleep suspends; one awake among several permits monitoring; brightness zero does not mean asleep.
- Stale camera/display generations do not change presence or release camera suspension; current-generation observations are also rejected while mode is `disabled` or `suspended`.
- Low-light recovery stays light-only until two usable-light checks, keeps a 10-second retry delay before the five-second settling window, ignores warmup frames, and does not count them as presence or absence.
- Per-display sleep never queues or dispatches a DDC set for that display, while an awake display and screensaver policy continue normally; wake uses a fresh read/ownership check before a write.

### Integration Tests
- ABI JSON accepts the additive command/settings fields and returns the expected camera mode, generation, and per-monitor sleep state without changing ABI version 1; snapshot consumers ignore or decode additive fields safely.
- A pure-Swift scheduler helper plus fake capture session and controllable clock verify always-on cadence, no session restart for eligible mode/capture-mode changes, five-second warmup, independent five-second acquisition timeout, suspension cancellation, wake resumption, and no stale result after cancellation. Run through `tools/tests/test-camera-lifecycle.sh` and the macOS CI workflow.
- Native localization verifies exact inline Portuguese text `Câmera: funcionando` and appropriate English translation.

### Edge Cases
- A healthy continuously running camera enters low light: wait five seconds before assessing without restarting its session.
- A camera starts during low light: the five-second warmup does not extend or replace its acquisition watchdog; timeout remains a camera failure.
- One of multiple eligible displays sleeps or wakes while another remains awake; only the sleeping target's DDC writes are withheld.
- Display state changes during a pending action or exposure warmup; old generations cannot apply a stale observation or restoration.
- Camera permission denial, device busy/error, battery state unknown/on-battery, no eligible monitor, settings toggle-off, app shutdown, or display reconnection cancels session/timers and leaves screensaver handling intact.
- A monitor reports brightness zero while awake; camera policy continues.
- OS sleep notifications and display reconfiguration arrive in either order or duplicate; reconciliation is idempotent and does not use polling.

## Acceptance Criteria
- Existing schema-1 settings files load unchanged with always-on disabled; new installations default to always-on disabled.
- When enabled, always-on operates only while all existing core eligibility rules permit monitoring and inference cadence remains configured as before.
- When every eligible display is asleep, monitoring and camera acquisition suspend; when any eligible display is awake, monitoring resumes. Brightness zero does not suspend the camera.
- No DDC write is issued to an asleep display, including a stale restoration; after wake, current brightness is checked before applying an owned target.
- Screensaver protection remains independently represented and active while camera monitoring is suspended.
- Low-light recovery starts a five-second warmup at each scheduled check, ignores those frames, assesses a later frame, retains the two-usable-check threshold and 10-second retry delay before settling, and keeps the five-second capture watchdog independent.
- Sleep, disable, battery suspension, errors, camera replacement, and shutdown cancel timers and reject stale in-flight camera results, including observations received while mode is suspended.
- Portuguese inline camera status reads exactly `Câmera: funcionando`.
- No new runtime dependency is required; Windows remains explicitly deferred.
- Unit, ABI, native fake-session, and documented macOS hardware acceptance checks meet their stated scope; physical monitor/camera support claims are limited to actually verified configurations.

## Validation Commands
```sh
rustup target add aarch64-apple-darwin
tools/inference/fetch-macos-runtime.sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo build --workspace --release --target aarch64-apple-darwin --locked
tools/tests/test-camera-lifecycle.sh
TELAVIVA_CORE_LIB="$PWD/target/aarch64-apple-darwin/release/libtelaviva_core.a" \
TELAVIVA_INFERENCE_LIB="$PWD/target/aarch64-apple-darwin/release/libtelaviva_inference.a" \
  packaging/macos/build-app.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
```

Run `tools/tests/test-camera-lifecycle.sh` in macOS CI and locally. Before packaging, prepare the Apple Silicon target and pinned ONNX Runtime as described in `README.md`; the full workspace suite includes the bundled-model integration test. The native script builds and runs the lifecycle and localization harnesses. Manual camera and physical-display sleep/wake checks require suitable macOS hardware and must be reported separately from automated results.

## Clarification Needed
- Should always-on capture keep the camera session open during normal face monitoring even when the configured inference cadence is long? Default: yes; frames between scheduled inferences are discarded, and inference cadence is unchanged. The core reports this as `continuous_monitoring`.
- If macOS reports the display state as unknown after startup or reconnection, should the app suspend immediately? Default: no; do not classify unknown as asleep, and only suspend when every eligible display is confirmed asleep. Continue applying per-target DDC dispatch safety when that target's sleep state is known asleep.
- Does the five-second exposure delay occur at the next low-light check? Default: yes; wait the existing 10-second retry delay, keep capture active for five seconds, then assess a fresh frame. No face inference runs during settling.
- Should always-on remain active with no eligible external monitor? Default: no; this feature targets eligible external monitors only.
- Does manual monitor power-off need a separate hardware-sensing mechanism when macOS keeps the display logically awake? Default: no; document this platform detection limit and defer extra sensing.

## Notes
- Implementation follows this standalone fast-feature plan in place; no duplicate `plan.md` is introduced. The low-light timing wording was normalized before implementation: a 10-second retry delay followed by five seconds of settling, rather than conflicting claims of a 10-second completed-assessment interval.
- Apple documents [`CGDisplayIsAsleep`](https://developer.apple.com/documentation/coregraphics/cgdisplayisasleep(_:)) for querying a display's sleep state and [`NSWorkspace.screensDidSleepNotification`](https://developer.apple.com/documentation/appkit/nsworkspace/screensdidsleepnotification) through `NSWorkspace.shared.notificationCenter`; also observe the corresponding wake notification and CoreGraphics display reconfiguration.
- macOS-reported display sleep may not reveal a physical power-off if the connection remains logically active. Validate actual behavior on supported display/connection combinations.
- Keep the plan bounded to macOS. Windows implementation and validation remain deferred.
- `always_on_camera` is additive to settings schema version 1 and does not change the C ABI version; camera frames continue to stay outside the core ABI. The new binary remains compatible with old settings files, while older binaries may reject the newly serialized field.

## Implementation Results

- Completed all eight implementation tasks with Luna agents, coordinated and reviewed by the primary agent. No new dependency was added.
- Rust: 34 tests passed (25 core contracts, 4 FFI contracts, 4 inference unit tests, 1 pinned-model CPU integration test). Formatting and strict clippy passed.
- Native: deterministic lifecycle and localization harnesses passed; Xcode project syntax and the Apple Silicon release bundle build passed. The final executable passed `--smoke-test`, and `codesign --verify --deep --strict` succeeded.
- Bundle: `build/macos/TelaViva OLED.app`. Local signing is ad hoc; distribution signing/notarization was not performed.
- Privacy/input review found no high-confidence vulnerabilities in the changed paths. Frames remain local, are not persisted or sent through the core ABI, and stale/canceled observations are rejected.
- [Physical macOS acceptance checklist](validation/macos-acceptance.md): not run when the implementation finished. It was run in part on 2026-10-18, as described in the section below. No power-consumption claim is made.

## Acceptance Run 2026-10-18

- Automated re-run on macOS 27.0.1 (Apple M4 Pro): `cargo fmt --check`, the full `cargo test --workspace --all-targets --locked`, strict clippy, the release build, the three native lifecycle scripts (`test-camera-lifecycle.sh`, `test-camera-lifecycle-bridge.sh`, `test-core-bridge.sh`), packaging, `--smoke-test` and `codesign --verify` all passed.
- Physical run with a Dell S3225QC: ten of the sixteen scenarios passed, one passed in part and five were not run. The results and the reasons are in the checklist.
- Corrections found while verifying: the checklist row for brightness zero expected monitoring to continue, which [ADR 0011](../../../adr/0011-camera-brightness-activation-threshold.md) superseded (the camera suspends as low brightness), and this plan names `native/macos/Tests/CameraLifecycleTests.swift`, which no longer exists because the lifecycle moved into the core ([ADR 0014](../../../adr/0014-camera-lifecycle-in-the-core.md)); its cases live in `crates/telaviva-core/tests/camera_lifecycle_contract.rs`.
- Open observation: a low-light report with a bright window behind the user, not diagnosed.
