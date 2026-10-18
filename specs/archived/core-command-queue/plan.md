# Tasks: Core-Owned Manual-Control Queue and DDC Dispatch

**Branch**: `core-command-queue` (created in T001, at implementation time) **Specs**: [spec.md](spec.md) **Architecture**: [architecture.md](architecture.md) **Contracts**: [contracts.md](contracts.md) **Status**: In Progress ⏰ (phases 1 to 7 implemented 2026-10-18; open: T038, T045, T049)

## Assumptions and execution notes

- The four decisions flagged in `architecture.md` (Decisions 4, 6, 7, 8) are accepted as written. If the owner rejects Decision 4, replace T041 with the fallback described there.
- The prerequisite from spec Decision 6 is met: `inference-device-selection` is committed (`64e9c46`, release `7247693`). At the start of implementation the inference-device-selection work had been committed (64e9c46, release 7247693).
- **Story mapping.** The spec's stories are outcomes, so phases follow components and each phase names the stories it delivers. US-001 (fix once) and US-005 (new behavior in the core only) are delivered by every Rust phase and proven by the adapter phases. US-002 (test once): phases 3 to 5 plus T029. US-003 (same on both platforms): phases 3 to 7 and the shared fixtures. US-004 (nothing replays): phases 3 and 5.
- **Tests come first.** The spec asks for them explicitly (FR-014, SC-006). Each port phase writes the ported cases before the code, and the coordinator confirms they fail before implementing.
- **Haiku workers have no shell.** The coordinator runs every build, test, clippy, fmt and parser step. Expect a fix pass after each batch. Rust checks: `cargo test -p telaviva-core`, `cargo clippy --workspace --all-targets`, `cargo fmt --all --check`. Windows checks: the native build and test scripts under `scripts/` and `packaging/windows/`, plus `tools/windows/check-dll-exports.ps1`.
- **macOS cannot be built or run on this Windows machine.** Phase 7 changes are verified by the macOS CI workflow (`.github/workflows/macos.yml`) or a Mac run of `tools/tests/*.sh`. Mark Phase 7 complete only after that run.
- **Port rule (Decision 8).** Reproduce the Windows behavior case by case. Fix only the five listed defects. Everything else flagged in the exploration is pinned by a `known_quirk_` test.
- Source references come from exploration reports. Before porting a rule, the worker or coordinator re-reads the original file; if report and source differ, the source wins.

## Phase 1: Setup ⏳ (~1.5h)

- [x] T001 Create the branch `core-command-queue` from the current HEAD (`git checkout -b core-command-queue`) and confirm the working tree holds only `specs/core-command-queue/`.
- [x] T002 Extract the strict JSON parser (unique keys at any depth, finite numbers, trailing-data rejection, `Fields` helper with unknown-field detection, size check before parsing) from `crates/telaviva-core/src/camera_lifecycle.rs` into new `crates/telaviva-core/src/strict_json.rs`, with its own error enum; add `mod strict_json;` to `crates/telaviva-core/src/lib.rs`.
- [x] T003 Switch `crates/telaviva-core/src/camera_lifecycle.rs` to `strict_json` without changing behavior; run `tests/camera_lifecycle_contract.rs` (44 tests) and the in-module tests, all green.
- [x] T004 In `crates/telaviva-core/src/core.rs` make `map_normalized` `pub(crate)` (visibility only). Add `pub mod controls;` to `lib.rs` and create the empty module files `crates/telaviva-core/src/controls/{mod,model,manual,keys,dispatch,retry}.rs` with one-line doc comments.

**Checkpoint**: ✋ `cargo test -p telaviva-core` unchanged and green; the branch exists.

## Phase 2: Foundational ⏳ (~4h)

⚠️ CRITICAL: blocks every phase below (event model, handle, ABI, test harness).

- [x] T005 Write failing envelope tests in new `crates/telaviva-core/tests/controls_contract.rs`: a harness that drives `Controls::step` with JSON and a monotonically supplied `now`; cases for unknown `type`, unknown field, duplicate key, non-finite number, missing and mistyped field, input over 64 KiB, `now` going backward (clamped), events after `shutdown`, and state unchanged after every rejection.
- [x] T006 Implement `crates/telaviva-core/src/controls/model.rs`: `Event` enum and parser for every event in `contracts.md` (`configure`, `observe`, `monitor_profile`, `monitor_removed`, `display_power`, `slider`, `key`, `submitted`, `attempt_done`, `tick`, `query_draft`, `shutdown`), array caps (64 monitors, 128 pending actions), `Effect` enum and renderer, result value types, and the constants table.
- [x] T007 Implement `crates/telaviva-core/src/controls/mod.rs`: `Controls::new`, `step(&mut self, &[u8]) -> Result<String, StepError>`, token counter, `now` clamping, `next_deadline` as the minimum over all sub-machines (initially none), shutdown flag. Make T005 pass.
- [x] T008 Add `TvControls` to `crates/telaviva-core/src/ffi.rs`: `tv_controls_create`, `tv_controls_step`, `tv_controls_destroy`, mirroring the camera handle (null checks, 64 KiB limit, poisoned lock to `TV_INTERNAL`, leaked buffer freed by `tv_buffer_free`, `assert_send_sync`).
- [x] T009 Declare the three functions and `typedef struct TvControls TvControls;` in `include/telaviva_core.h`. In `crates/telaviva-core/tests/header_contract.rs` add the exports table entries, prototype strings and `TvControls` to the opaque-handle list. Add FFI round-trip tests to `crates/telaviva-core/tests/ffi_contract.rs` (create, step, destroy, null arguments, size limit, buffer free).
- [x] T010 Document the new ABI in `api.md` (table at the top and a "Controls ABI" section next to the camera lifecycle section, including "pass `command_json` and `result_json` verbatim"). Confirm `tools/windows/check-dll-exports.ps1` needs no change.

**Checkpoint**: ✋ A `configure` event round-trips through the C ABI; `cargo test -p telaviva-core`, clippy and fmt are clean.
**Notes (done)**: envelope tests (T005) were written in parallel with the implementation, not before it, so the fail-first check was skipped for this phase. The model re-parses with `serde_json::Value` after the strict pass because `strict_json::Fields` cannot read arrays or nulls yet.

## Phase 3: Manual queue — slider path (US-003, US-004) ⏳ 🎯 MVP (~5h)

**Goal**: drafts, latest-wins coalescing, one write in flight, safety release and drop-on-change, identical to the Windows `ManualControls`.
**Independent Test**: replaying the ported Windows manual cases and the drag fixture against `Controls` gives the same effects and drafts.

### Tasks
- [x] T011 Port Windows `native/windows/Tests/manual_controls.cpp` cases to `controls_contract.rs::manual_*` (tests first, failing): idle does no work; draft shown at once; rejected submit fails without retry; rapid targets coalesce and latest wins; queued target waits for a core pending action; in-flight until completion; readback within 0.51 clears the draft; completion without readback falls back to observed; stale completion; return to observed is still submitted; duplicate raw is not resubmitted (51 and 52 percent to raw 5 on a 0 to 10 range); 128-draft cap; clamp and non-finite.
- [x] T012 Port the remaining manual cases: sleep drops draft and wake does not replay; asleep monitor ignores targets; disappearing monitor drops drafts; generation rollover; shutdown; snapshot during submission does not release the slot; release only after the 3 s safety window; failed completion marks the draft failed at the observed value; failed completion with a queued target sends the queued one; completion re-drains at 0 ms; completion for an unknown monitor is ignored; completion matches by monitor and feature, not action generation (regression `01bd988`); independence of monitors and features. Map the external-intent cases (14 to 16) to key-target tests in Phase 4.
- [x] T013 Implement `crates/telaviva-core/src/controls/manual.rs`: `Slot`, channel eligibility derived from `observe` exactly as `ManualControls::apply`, `raw_target` (Decision 5), `set_target`, drain selection and the 20 ms and 0 ms deadlines, completion handling, safety release (3 s, newer revision, no pending action), readback clearing, drop on sleep, missing monitor, generation change or ineligibility.
- [x] T014 Wire `slider`, `query_draft`, `submitted` and `tick` for the manual path in `controls/mod.rs`: `submit_command` effects with `command_json` for `set_brightness` and `set_volume` including `connection_generation`, `draft_changed` effects, and the slider result value. A rejected submission follows the Windows rules (failed draft, or send the queued target).
- [x] T015 [P] Add the new cases in `controls_contract.rs`: a completion carrying a different connection generation than the in-flight command is ignored (fixes Windows quirk 1); `observe` with a lower revision is ignored and an equal one accepted.
- [x] T016 Make T011, T012 and T015 pass; run clippy and fmt.

**Checkpoint**: ✋ All ported manual cases and the new ones are green. The slider path is complete in Rust.
**Notes (done)**: ported cases are unit tests in `controls/manual/tests.rs` (completion only exists once dispatch lands in Phase 5), event-level slider tests are in `tests/controls_contract.rs`. Tests and implementation were written in parallel from one spec, so no fail-first run; two wrong test expectations were corrected against the C++ semantics. The snapshot's `observed` is raw and `normalized` is the percentage; the contract now says `observe.observed` is the percentage (the Windows shell reads the raw field today, correct only for 0 to 100 ranges). The external-intent flow (`adoptExternalIntent`, receipts) is not ported; key intents share the slider queue.

## Phase 4: Key policy (US-003, US-005) ⏳ (~5h)

**Goal**: step arithmetic, policy latch, intents, rebase, outstanding and settle tracking, and mute memory in Rust, for volume, mute and brightness keys.
**Independent Test**: the ported Windows `key_intent_router.cpp` cases and the macOS router cases pass against `Controls`.

### Tasks
- [x] T017 Port Windows `native/windows/Tests/key_intent_router.cpp` to `controls_contract.rs::keys_*` (tests first, failing): step policy and constants; configured step bounds; boundaries consume without write; owner selection requires an eligible observation; no owner passes the whole sequence; held key coalesces repeats; latch decides the whole sequence per key; orphan repeat passes; hold cancelled when the owner leaves; stale generation cancels the hold; rejected submission leaves the release consumed; manual change invalidates a handed-off intent; rebase bounded; external readback rebases; tolerance; step change applies to the next press; own write blocks rebase; settle window; 10 s expiry; completion before acceptance; the 12 mute cases.
- [x] T018 [P] Port the macOS-only key cases from `native/macos/Tests/HardwareKeyRouterTests.swift` that apply to the core policy: brightness keys, `boundaryClampsWithLargeStep`, `setStepsClampsAndFallsBack`, `stepChangeAppliesToNextPress`, `muteRestoreUsesVolumeStep`, `rebaseWaitsForOwnHardwareActions`, `rebaseResumesAfterSettleWindow`, `outstandingActionExpires`, `earlyCompletionLeavesNothingOutstanding`, `observedChangesAndOrphanRepeats`. Skip the cases that test decoding, the event tap, permissions or followed-brightness pass-through (shell side).
- [x] T019 Implement `crates/telaviva-core/src/controls/keys.rs`: constants, `clamped_step`, `stepped` (no rounding), the policy latch, `reserve_step`, `reserve_mute`, `reserve_write`, `flush`, `complete`, `reconcile`, `prune_rebase_state`, bounded maps.
- [x] T020 Wire the `key` and `configure` events in `controls/mod.rs`: key to feature mapping, `owner` handling and per-feature mute-memory clearing on owner change, key intents entering the same slots as slider targets (shared invalidation), `consume` in the result, `draft_changed` for key-driven drafts.
- [x] T021 Add `known_quirk_` tests that pin the preserved behaviors: reconcile erases an intent on an unknown readback; flush erases an intent while a write is in flight; the mute-repeat cancelled flag is inert; clearing `earlyCompleted` whole on overflow; the 128-in-flight stall; the readback clearing a draft while a command is in flight.
- [ ] T022 Add the fix test: a key intent is finished with the real submission outcome, so a dropped write no longer leaves a phantom base (Windows Q2) (no test: the fix holds by construction; the dropped-write path is unreachable through the event API).
- [x] T023 Make T017, T018, T021 and T022 pass; run clippy and fmt.

**Checkpoint**: ✋ Key policy complete in Rust; slider and key targets share slots and invalidate each other as the Windows tests require.
**Notes (done)**: Keys is a pure port of `KeyIntentRouter` generalized to brightness and volume; `action_accepted`/`action_completed` take a `feature` (found while briefing: without it brightness actions were lost). The macOS app has separate brightness and volume step settings, so `configure` now carries `brightness_step` and `volume_step` (the single `step` was removed) plus an optional `volume_owner` that clears the mute memory on change. Key intents are handed to the manual queue synchronously inside the same step with their real outcome (the phantom-base fix); that dropped-write path is unreachable through the event API because 64 monitors times 2 features equals the 128-draft cap, so no event-level test exists for T022 and the fix is covered by construction. Tests and implementation again ran in parallel, so no fail-first run; five wrong test expectations were corrected against the C++ semantics. Windows tests are ported as `keys/tests.rs` (34 router + 5 tracker cases), macOS policy and brightness cases as `keys/brightness_tests.rs`, six `known_quirk_` pins in `keys/quirk_tests.rs` and `manual/tests.rs`.

## Phase 5: DDC dispatch and retry budget (US-001, US-004) ⏳ (~6.5h)

**Goal**: the core decides which pending action runs, when each DDC primitive runs, how it retries, and what result it reports.
**Independent Test**: a fake shell driving `run_attempt` effects reproduces the ported controller and transaction cases, including the retry ceiling.

### Tasks
- [x] T024 Port Windows `native/windows/Tests/ddc_transaction.cpp` and macOS `DDCTransactionTests.swift` to `controls_contract.rs::retry_*` (tests first, failing): write, 60 ms settle, then read; duplicate set only for manufacturer `GSM` (case-insensitive, `DEL` and empty are false); mismatch reported as `io` with the observed value and no retry; permanent errors (`unsupported`, `disconnected`) never retried; read retries after 200 ms then 1000 ms and reports the last error; default profile values. Add the changed rule: a transient write error is retried up to three attempts.
- [x] T025 Implement `crates/telaviva-core/src/controls/retry.rs`: attempt state machine for one primitive, the single budget constants, profile lookup, error classification.
- [x] T026 Port Windows `native/windows/Tests/ddc_controller.cpp` dispatch cases to `controls_contract.rs::dispatch_*` (failing first): one action per monitor; the same id in two snapshots runs once; asleep monitor runs nothing; wake grace holds at 1999 ms and runs at 2000 ms; disconnected, ambiguous and unsupported results; result for `read_brightness` reports the raw value; failed set reports no value; percent comes from the observed range of the feature (fixes Q8); shutdown drops late events. Add the new cases: a rejected result is resubmitted at most three times with 200 ms then 1000 ms backoff and the write never re-runs (Decision 7).
- [x] T027 Implement `crates/telaviva-core/src/controls/dispatch.rs`: ingest `pending_actions` from `observe`, running and completed bookkeeping, hold rules, the single execution slot, `run_attempt` effects, `result_json` construction matching `core.rs` `hardware_result`, the `submitted` handling for results, bounded resubmission. Handle `monitor_profile`, `monitor_removed` and `display_power` in `controls/mod.rs`.
- [x] T028 Add an end-to-end test in `crates/telaviva-core/tests/controls_contract.rs` that drives a real `Core` (`submit_json`, `snapshot_value`) together with `Controls`: slider to `set_brightness` to pending action to `run_attempt` to `attempt_done` to `hardware_result` to cleared draft. Add the ceiling test: a monitor that never answers costs at most 12 primitive attempts and about 4.8 s of backoff waiting across the core's three restore retries.
- [x] T029 Add the shared scenario fixtures `crates/telaviva-core/tests/fixtures/controls/*.json` (drag, held key, external change, sleep and wake, disconnect, rejected result) and a replay test that asserts each expected effect sequence.
- [x] T030 Make T024, T026, T028 and T029 pass; run clippy and fmt; run the whole workspace tests.

**Checkpoint**: ✋ The Rust machine is complete. `cargo test --workspace`, clippy and fmt are clean. From here the shells only adapt.
**Notes (done)**: `Operation` (retry.rs) is one primitive state machine with a per-primitive budget of 3 attempts and backoff 0.2 s then 1.0 s, a 60 ms settle after the last write and the GSM-only duplicate write; `Dispatch` (dispatch.rs) holds one execution slot across monitors (a monitor that is only waiting does not hold it), re-offers nothing and resubmits a rejected result at most 3 times (0.2, 1.0, 1.0 s) without re-running the write. The Windows quirk where a rejected result re-ran the write on every snapshot is gone. Four end-to-end tests drive a real `Core` (slider and key to the wire, rejected submission, and the stuck-monitor ceiling of at most 12 read attempts and 4.8 s of backoff across the core's three restore retries). Seven scenario fixtures live in `tests/fixtures/controls` and replay through `tests/controls_fixtures.rs`; their `expect` blocks were generated with `TELAVIVA_UPDATE_FIXTURES=1` and reviewed by hand. A small wart found here (after drafts are dropped, `next_deadline` kept the stale drain deadline until the next `tick`) was fixed later: the deadline is cleared when no queued target remains. Unit tests keep the ported manual, key and DDC cases in `manual/`, `keys/`, `retry/` and `dispatch/` test modules.

## Phase 6: Windows adapter (US-001, US-002, US-003) ⏳ (~7h)

**Goal**: replace `ManualControls`, `KeyIntentRouter`, the controller's retry logic and `VolumeActionTracker` with a thin driver over the handle.
**Independent Test**: the Windows native tests and the packaged app drag a slider and hold a volume key with the same effects as the fixtures.

### Tasks
- [x] T031 Add `native/windows/Platform/ControlsApi.h` and `ControlsApi.cpp`: an `IControlsApi` seam and the real implementation over `tv_controls_*` with one mutex, empty-string on ABI error, buffer freed after copy (as `CoreLifecycleApi`). Add `native/windows/Tests/controls_api.cpp` with a fake.
- [x] T032 Extend `native/windows/Platform/JsonCodec.h/.cpp` to encode every event and decode `Result` and `Effect` arrays through `Windows.Data.Json`. Add cases to `native/windows/Tests/json_codec.cpp` (round trip of each event, effects with `command_json`, `null` handling).
- [x] T033 Add `native/windows/Platform/ControlsDriver.h/.cpp`: methods for each event, effects executed after the driver lock is released, one timer through `IScheduler` armed from `next_deadline`, `submitted` and `attempt_done` reporting, shutdown. Add `native/windows/Tests/controls_driver.cpp` using `FakeScheduler`, `FakeClock` and a fake API.
- [ ] T034 Reduce `native/windows/Platform/DdcTransaction.cpp` to one primitive per call (one `writeVcp`, one `readVcp`) and `native/windows/Platform/DdcController.cpp` to discovery, display state, the port, and "run one attempt"; keep the shell's `monitor_discovered`, `monitor_display_state` and `monitor_disconnected` submissions; forward `monitor_profile`, `monitor_removed` and `display_power` to the driver. Update `native/windows/Tests/ddc_transaction.cpp` and `ddc_controller.cpp` to the new shape. (partial: DdcController is reduced; DdcTransaction.cpp stays for discovery and --probe-ddc, so ddc_transaction.cpp stays)
- [x] T035 Wire `native/windows/TelaViva/Host/AppController.cpp`: slider handlers call the driver; the key consumer calls the driver `key` event and ignores `consume` (the hook latch decides); every published snapshot calls `observe`; `configure` on step changes; remove `ManualCompletionGate`, `CoreManualSink`, `BridgeSubmitter` use for manual commands and the `VolumeActionTracker` plumbing; keep the hook thread, `KeyRoute` and `KeyLatch` untouched; fix the shutdown order.
- [x] T036 Update `native/windows/TelaViva/MonitorCard.xaml.cpp` and the flyout code to read drafts through `queryDraft` and to repaint on `draft_changed`; keep the 300 ms release hold-off in the UI. (no change needed: drafts reach the flyout through buildFlyoutState and the draft observer)
- [x] T037 Delete `native/windows/Platform/ManualControls.*`, `KeyIntentRouter.*`, `VolumeActionTracker.*`, `DdcTimingProfile.*` and their tests (`manual_controls.cpp`, `key_intent_router.cpp`, `volume_action_tracker.cpp`) only after the Rust cases that replace them pass; update `Platform.vcxproj` and `Tests.vcxproj` if sources are not globbed. (done for the nine files; `DdcTimingProfile.*` stays because discovery and `--probe-ddc` use it; the project files use globs)
- [ ] T038 Build and run the Windows native tests and `tools/windows/check-dll-exports.ps1`; run the app and exercise a slider drag and a held volume key on a real monitor, recording the results. (partial: the solution builds, 614 native tests and the DLL export check pass; packaging and the hardware run are pending)

**Checkpoint**: ✋ Windows runs entirely on the handle. No coalescing, rebase, step or retry code remains in C++.
**Notes (partly done)**: `ControlsApi`, `ControlsDriver`, the `JsonCodec` builders and `parseControlsResult`, the reduced `DdcController` (`run` executes one primitive, `onSnapshot` only records generations, `setListener` breaks the construction cycle with the driver) and the rewired `AppController` are written; the full app compiles and links (`TelaViva.exe`) and `tests.exe` runs 614 tests, 0 failed after the replaced tests were removed (`controls_api` 5, `controls_driver` 17, `json_codec_controls` 16, `ddc_controller` 18). **Done**: (T037) the nine replaced files (`Platform/ManualControls.*`, `KeyIntentRouter.*`, `VolumeActionTracker.*` and `Tests/manual_controls.cpp`, `key_intent_router.cpp`, `volume_action_tracker.cpp`) were removed with `git rm`; nothing referenced them except their own tests. **Still open**: (T038) packaging was blocked because a TelaViva.exe was running from `dist\TelaVivaOLED`, and no hardware run has been done. `DdcTransaction` and `DdcTimingProfile` are NOT removed: discovery reads and the `--probe-ddc` diagnostic still use them (discovery retry timing stays duplicated until it moves to the core). `AppController` frees `controls`/`ddc`/`controlsApi` after the bridge (the snapshot thread uses them until the bridge joins). The adapter reads the snapshot's `normalized` percentage; the old code treated the raw `observed` as a percentage, correct only for 0 to 100 ranges. T036 needed no UI change: the flyout reads drafts through `buildFlyoutState`.

## Phase 7: macOS adapter (US-001, US-002, US-003) ⏳ (~7h, verified on macOS or CI only)

**Goal**: the same replacement in Swift; the event tap, decode, permissions and pointer targeting stay.
**Independent Test**: the new `tools/tests/test-controls-bridge.sh` and the existing Swift scripts pass on a Mac or in CI.

### Tasks
- [x] T039 Add `native/macos/TelaViva/ControlsBridge.swift` (create, step, destroy, `JSONSerialization` encode, `Codable` decode, fault handling like `CameraLifecycle.swift`), `native/macos/Tests/ControlsBridgeTests.swift`, `tools/tests/test-controls-bridge.sh`, and a step for it in `.github/workflows/macos.yml`. (done; also `native/macos/TelaViva/ControlsDriver.swift`, the effect executor and the one timer, split out as the Windows `ControlsApi` and `ControlsDriver` are.)
- [x] T040 Reduce `native/macos/TelaViva/DDCTransaction.swift` to one primitive each (framing, write-twice and the checksum seed fallback stay inside one primitive) and `native/macos/TelaViva/MacDDCController.swift` to discovery, sleep handling and "run one attempt" on the `com.telaviva.ddc` queue. Update `native/macos/Tests/DDCTransactionTests.swift`. (done; `readVCPOnce` and `writeVCPOnce` are the one-attempt primitives. The retrying `readVCP` and `writeVCP` stay for discovery and the probe only, as on Windows, so discovery retry timing stays duplicated until it moves to the core.)
- [x] T041 Reduce `native/macos/TelaViva/HardwareKeyRouter.swift` to decode, the tap, permissions, pointer and output targeting, and followed-brightness pass-through; the tap calls the handle `key` event synchronously and uses `consume`. Remove `HardwareKeyPolicy`, `HardwareKeySequence` and the intent, rebase and mute code. Adapt `native/macos/Tests/HardwareKeyRouterTests.swift` and `MediaKeyProbeTests.swift`. (done; owner resolution stays in the shell as `HardwareKeyOwnerResolver`, and the tap swallows exactly when the core answers `consume`.)
- [x] T042 Reduce `native/macos/TelaViva/ManualMonitorControls.swift` to a UI binding over `query_draft` and `draft_changed`; remove `ManualCommandAuthorization`, the receipts and the early-completion buffer; adapt `native/macos/TelaViva/MonitorPopup.swift` and `native/macos/Tests/ManualMonitorControlsTests.swift`. (done; the binding also sends `slider`.)
- [x] T043 Wire `native/macos/TelaViva/CoreBridge.swift` and `native/macos/TelaViva/MenuBarController.swift`: `observe` after each snapshot, an effect executor that runs `submit_command` and `submit_result` through the existing `ffiLock` path and `run_attempt` on the DDC queue, one `DispatchSource` timer armed from `next_deadline`, `display_power` from the workspace notifications, `configure` from settings. (done; `observe` runs for every snapshot before the terminating guard, so shutdown restoration keeps working; `CoreBridge.submitJSON` passes `command_json` and `result_json` verbatim to `tv_submit`; `controls.shutdown()` runs just before `ddc.shutdown()`.)
- [x] T044 Add the new Swift sources to the `SOURCES` array in `packaging/macos/build-app.sh` and to `native/macos/TelaViva.xcodeproj/project.pbxproj`; delete the replaced Swift code and tests only after the Rust cases that replace them pass. (done; removed `ManualCommandAuthorization`, the receipts and early-completion buffer, `HardwareKeyPolicy` stepping and clamping, `HardwareKeySequence`, and the intent, rebase and mute code, with their tests; the cases live in the Rust contract and unit tests.)
- [ ] T045 Run `tools/tests/*.sh` and the macOS workflow; record the result. If no Mac is available, leave this phase open and say so. (partial: the 16 local test scripts pass on a Mac; the macOS CI run and the hardware checks are pending.)

**Checkpoint**: ✋ macOS runs on the handle, verified by CI or a Mac run.
**Notes (partly done)**: the Swift side of the handle is written: `ControlsBridge.swift` (handle wrapper and result decoding; a failed step stops the handle, as `CameraLifecycle` does), `ControlsDriver.swift` (effect executor and the one timer armed from `next_deadline`), and the reduced `DDCTransaction`, `MacDDCController`, `HardwareKeyRouter`, `ManualMonitorControls`, `CoreBridge` and `MenuBarController`. `MenuBarController` builds one driver and sends `configure` (brightness and volume steps, `keys_enabled`, volume owner) when they change. The driver ignores a non-finite slider value and replaces a non-finite step with 5, so the bridge never faults on input it cannot serialize. Decision 4 is implemented: the event tap makes one synchronous call into the handle's leaf lock. The app sources type-check with the `build-app.sh` flags and link against the core library with the controls symbols resolved. **Done on a Mac (Apple Silicon, 2026-10-18)**: all 16 `tools/tests/test-*.sh` scripts pass. **Not done**: the full app packaging (`scripts/build-macos.sh` downloads ONNX Runtime; a stale `target/release/libtelaviva_inference.a` lacks a symbol, unrelated to this change), the macOS CI run, and every hardware check (slider drag, held volume key, mute and restore, external change, wake from sleep with slow DDC, a non-GSM monitor, the LG UltraFine). No macOS validation-report template exists (T048 covered the Windows checklist only). The macOS primitive keeps `writeTwice`, so the duplicate-write behavior on macOS is unchanged; limiting it to GSM is an open decision.

## Final Phase: Polish ⏳ (~5h)

- [x] T046 [P] Update `ARCHITECTURE.md` (the line that says native Swift device queues replace the Rust worker modules, the DDC and key sections), `api.md`, `docs/user-guide.md` where behavior changed (write retry, resubmission), and the `CHANGELOG.md` `Unreleased` section. (ARCHITECTURE.md, api.md, CHANGELOG.md, AGENTS.md, docs/user-guide.md)
- [x] T047 [P] Prepare the decision record for `/codeadvisor-dev:finalize`: supersede or amend ADR 0001, 0004, 0005, 0008 and 0021, and state the amended "three attempts per operation, at most three restore operations" reading of the archived `contracts.md` rule, the macOS tap call, the GSM-only duplicate set and the bounded resubmission. (done: [ADR 0029](../../../adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md) amends ADR 0001 and supersedes the router decisions of ADR 0021 for Windows; ADRs 0004, 0005 and 0008 stay macOS-scoped and were amended for the macOS adapter on 2026-10-18)
- [x] T048 [P] Update the Windows hardware checklist and the macOS validation report template with the cases this change must be verified on: slider drag, held volume key, mute and restore, external change while idle, wake from sleep with slow DDC, a non-GSM monitor and the LG UltraFine (`GSM`). (Windows checklist only: specs/archived/windows-version/validation/hardware-checklist.md; no macOS template exists)
- [ ] T049 Final verification: `cargo test --workspace`, clippy, fmt check, `version_consistency`, header contract, Windows native tests and DLL export check, macOS CI. Record every result and every step that could not run.

## Dependencies & Execution Order

```
Phase 1 Setup
   └─▶ Phase 2 Foundational (event model, handle, ABI)
          └─▶ Phase 3 Manual queue ──▶ Phase 4 Key policy ──┐
          └─▶ Phase 5 Dispatch and retry (needs Phase 2 only; shares effects with 3) ─┤
                                                                                     ▼
                                                      Phase 6 Windows adapter ┐
                                                      Phase 7 macOS adapter   ├─▶ Final Phase
                                                                              ┘
```

- Phases 3 and 4 share slots, so run 3 then 4. Phase 5 touches different files (`dispatch.rs`, `retry.rs`) and can run in parallel with 3 and 4 once Phase 2 is done, but both edit `controls/mod.rs`: serialize the `mod.rs` wiring tasks (T014, T020, T027).
- Phases 6 and 7 are independent of each other and can run in parallel once Phase 5 is done. Each needs the ABI from Phase 2 and the full Rust machine.
- Deletion tasks (T037, T044) wait for the Rust cases that replace the deleted tests.
- Same-file groups for the workers: T006 and T007 (model and mod), T013 and T014, T019 and T020, T025 and T027, T033 and T035.

## Implementation Strategy

- **MVP**: Phases 1 to 3 (about 10.5h) give a working slider queue in Rust behind the ABI, testable on its own.
- **Core complete**: Phases 1 to 5 (about 22h) give the whole machine with no shell change; shells still run their old code, so this is safe to land first.
- **Then the shells**, Windows first (it can be verified locally), macOS second (needs a Mac or CI). Landing the core and each adapter as separate commits keeps review manageable and lets the old shell code serve as the reference until each phase is verified.
- **Total estimate**: about 40h of work, excluding hardware verification time and waiting for CI.
- **Risk to watch**: timing behavior on real monitors (no blocking sleeps, writes now retried on Windows, GSM-only duplicate write in the core (the macOS packet duplication inside one primitive is unchanged)). Hardware checks in T038, T045 and T048 are required before calling the feature done.

## Progress Tracking

Status 2026-10-18: Phases 1 to 7 are implemented; Phase 6 is done except T038 packaging and hardware; Phase 7 is implemented, with the local macOS test scripts passing on a Mac and the macOS CI run and hardware checks (T045) pending; open items are T038, T045 and T049 (final verification: the Rust and Windows results below, the macOS local scripts, macOS CI pending). T047 (decision record) is done as ADR 0029; the spec folder is archived (`specs/archived/core-command-queue/`, and the ADR's Spec link updated) only after T038, T045 and T049 are closed; T049 ran for Rust and Windows on the tree merged with main (fmt, clippy -D warnings, all Rust tests including 184 controls unit tests and 41 contract tests, 614 Windows tests, DLL exports) and is still open for macOS CI.

⏳ Not Started · ⏰ In Progress · ✅ Completed — update as you go, including a Notes line under each phase.

### Gaps found by the final audit

Resolved on 2026-10-18 (each with a test):

- (a) The key policy's accepted-action tracking now ignores a snapshot older than the last one applied, like the manual queue and the dispatcher.
- (b) The 0.51 tolerance and the default step have one definition each.
- (c) The dispatcher's profile map is bounded: at 64 entries a new profile evicts that of a monitor absent from the latest snapshot.
- (d) `Controls` mirrors the key policy: `keys_enabled: false` clears the volume owner and a `volume_owner` in the same event is ignored.
- (g) A drain deadline is cleared when no queued target remains (after `observe` drops drafts and when a drag ends on the in-flight value).
- (h) The nine replaced Windows files were removed with `git rm`.
- (j) The README version sentence matches the crate (0.1.1).

Kept on purpose:

- (e) The Windows driver ignores a non-OK ABI status instead of stopping; the contract documents it. A dropped event costs one missed update, and stopping would disable the controls.
- (f) The in-flight safety release, rebase settle and outstanding expiry have no deadline and run on the next `observe` or `key` event, as in the Windows original (documented in contracts.md).

- (i) ADR 0021's router decisions (hook posts to a bridge queue; the step, rebase guard and latch are a C++ port of the Swift router; the router logic is duplicated in Swift and C++; outstanding-action inputs are not wired) are superseded for Windows by [ADR 0029](../../../adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md), and ADR 0001 is amended by it. ADRs 0004, 0005 and 0008 are macOS-scoped and are amended for the macOS shell (see their amendment notes).
- (k) Two ADRs carried the number 0027 after a merge; the Windows toggle alignment ADR was renumbered to 0028 and the new ADR is 0029.

Open: nothing in the audit list. The macOS CI run and hardware checks (T045), the Windows packaging and hardware run (T038), the final verification (T049) and the final archive of this folder remain.




