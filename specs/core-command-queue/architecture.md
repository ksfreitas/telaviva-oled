# Architecture: Core-Owned Manual-Control Queue and DDC Dispatch

**Feature**: Shared manual-control command queue in the Rust core
**Date**: 2026-10-18
**Branch**: designed on `inference-device-selection`; the branch `core-command-queue` is created at implementation time (owner decision, spec Decisions 6 and 7)
**Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary

A new opaque, clock-injected handle in `telaviva-core` (`TvControls`) owns everything between "the user moved a slider or pressed a key" and "one DDC primitive ran on the wire": drafts, latest-wins coalescing, one write in flight, key step/intent/rebase/mute policy, dispatch of core `pending_actions`, and the DDC retry and pacing policy. The shells (Swift, C++/WinRT) shrink to adapters: they forward events with `now`, execute the effects the handle returns (submit a command to the core, run one DDC primitive, arm one timer, repaint a control), and keep everything that touches the operating system. The design follows the `TvCameraLifecycle` precedent (ADR 0014) and is additive to C ABI version 1.

## Technical Context

**Language/Stack**: Rust 2021 in `crates/telaviva-core` (rlib + staticlib, cdylib built on Windows by `tools/windows/build-rust-dlls.ps1`); Swift (macOS) and C++/WinRT (Windows) adapters.
**Key Dependencies**: `serde`, `serde_json` only (no new crates).
**Storage**: none. State is in memory and rebuilt from snapshots after a restart.
**Testing**: Rust contract tests with an injected `now` (replace ~100 duplicated Swift and C++ cases); shared JSON scenario fixtures; thin adapter tests in each shell; `header_contract.rs` and `check-dll-exports.ps1` for the ABI.
**Platform**: macOS (Apple Silicon) and Windows 11 x64, in the same change.

## Technical Decisions

### Decision 1: One new opaque handle, not `tv_submit`
**What**: `TvControls` with `tv_controls_create`, `tv_controls_step`, `tv_controls_destroy`. One JSON event in, one JSON result out, `now` (monotonic seconds) on every event.
**Why**: Slider ticks and key repeats arrive at input rate. Every accepted `tv_submit` bumps the revision and builds a snapshot (ADR 0014, `specs/snapshot-publish-amplification.md`). The camera handle already proved the shape, the header and DLL checks, and both shell wrappers.
**Alternatives Considered**: Extend `Core` and `tv_submit` (rejected: snapshot amplification, and it couples draft state to the snapshot revision). Generate bindings (rejected, same reasoning as ADR 0014).
**Trade-offs**: Two handles per process; the shell must call `observe` after each snapshot so the handle sees the same world as the core.

### Decision 2: The machine never calls the core; effects carry ready-to-send JSON
**What**: The result's `effects` list contains `submit_command` effects with a `command_json` string (the exact text the shell passes to `tv_submit`) and `submit_result` effects with a `result_json` string (the exact `hardware_result` text). The shell passes them through unchanged and reports the outcome with a `submitted` event.
**Why**: Before this change each shell built `set_brightness`, `set_volume` and `hardware_result` JSON itself (the Windows `CoreManualSink`, now removed, `buildHardwareResult`, and the macOS `CoreBridge.submitManualControl`). One builder in Rust removes that copy and keeps bytes identical on both platforms. It also keeps the handle free of any dependency on `TvCore`'s lock.
**Alternatives Considered**: Structured effect fields that each shell turns into JSON (rejected: a third place to get field names and types wrong).
**Trade-offs**: A JSON-in-JSON string; shells must not re-serialize it.

### Decision 3: Time and timers stay in the shell; the core reports one deadline
**What**: Every result carries `next_deadline` (absolute monotonic seconds, or `null`). The shell keeps one timer armed for it and calls `tick` when it fires. The core owns the delay values of the manual-control and dispatch path (20 ms coalesce, 3 s safety release, 1 s rebase settle, 10 s outstanding expiry, 2 s wake grace, 60 ms settle, 10 ms duplicate gap, 200 ms and 1000 ms backoff). The Windows discovery transport (`DdcTransaction`, `DdcTimingProfile`) still carries its own copy of the transport delays until discovery moves to the core. Only the coalesce drain, the DDC waits (settle, backoff, resubmission) and the wake grace reach `next_deadline`; the in-flight safety release (3 s), the rebase settle and the outstanding-write expiry are evaluated on the next `observe` or `key` event (the Windows ManualControls worked the same way).
**Why**: Matches ADR 0014 and ADR 0020 (thread-pool timers): no thread or clock in the core, deterministic tests. Removing blocking `sleep` calls from the DDC worker is a benefit: Windows runs one worker for all monitors, so one failing monitor delays the others today.
**Alternatives Considered**: Core-owned timer thread (rejected: breaks "core owns no threads" and complicates shutdown). Returning relative delays (rejected: races with a late timer; absolute deadlines are idempotent).
**Trade-offs**: A late timer only delays work; `tick` is safe to call early or twice.

### Decision 4: Key policy moves; hook-thread decision and OS capture stay
**What**: The core owns step arithmetic, fine step, mute memory, held-key accumulation, intents, rebase, outstanding-write and settle tracking, and the per-sequence policy latch (the Windows `KeyIntentRouter::Latch`, the macOS `HardwareKeySequence`). Stay in shells: key capture (event tap, low-level hook), the lock-free swallow decision on the Windows hook thread (`KeyRoute`, `KeyLatch`, `KeyEventQueue`, watchdog), target resolution (macOS pointer position and default output UID, Windows route owner), permissions, followed-brightness pass-through.
**Why**: Spec Decision 5 and ADR 0021 (the Windows hook never calls the core). The policy latch is not the swallow latch, even though both are called "latch".
**Alternatives Considered**: Move the swallow latch too (rejected by the owner). Keep the policy latch in Swift and C++ (rejected: it is where the duplicated `process()` logic lives).
**Trade-offs and review point**: On macOS the event tap currently decides consume-or-pass synchronously inside `HardwareKeySequence`. With the policy latch in the core, the tap makes one synchronous in-memory call, `tv_controls_step`, on a leaf mutex that is never held across I/O and never taken together with `ffiLock`. Windows ignores the returned `consume` flag, as today, because its hook decides from the route alone. ADR 0004's "the event tap never calls the core" needs an amendment for this one call. Fallback if rejected: macOS keeps `HardwareKeySequence` and calls the handle asynchronously, losing the synchronous reservation result (a degradation).

### Decision 5: Normalized values everywhere; one rounding rule
**What**: Drafts, targets and keys are normalized 0 to 100. The machine converts to raw once, `raw = clamp(round_half_away(min + (v / 100) * (max - min)), min, max)`, for both dedupe ("same raw as in-flight") and dispatch. The core keeps `map_normalized` unrounded for its own state (`core.rs:1583`).
**Why**: Before this change the Windows `rawFor` rounded with `llround`, macOS rounded in its own `rawTarget`, and the Windows `DdcController::rawTarget` rounded again (all three are gone from Windows). One rule removes a source of off-by-one drift and the Q8 percent bug (percent computed from a maximum that may default to 100).
**Trade-offs**: Must be pinned by a test against the existing 51 percent and 52 percent to raw 5 case.

### Decision 6: One retry budget, defined once and bounded by test
**What**: A single module, `controls/retry.rs`, defines the budget: at most 3 attempts per DDC primitive (write or read), backoff 200 ms then 1000 ms, permanent errors (`unsupported`, `disconnected`) never retried, a readback mismatch never retried. The core's restore retries (ADR 0006, `MAX_RESTORE_FAILURES` = 3) remain core policy and start new operations. A test pins the product: one stuck monitor costs at most 4 failed read operations, 12 primitive attempts, and about 4.8 s of backoff waiting (ADR 0006's 7 s includes device time).
**Why**: The owner chose scope B and a single budget (spec Decision 1). Collapsing the layers into one smaller budget would shrink the resilience ADR 0006 added for monitors whose DDC is slow after wake, which is its main residual risk, with no hardware evidence either way.
**Alternatives Considered**: One shared episode budget of N attempts (rejected for the resilience reason above). Leave the layers in two places (rejected: the tension the owner asked to remove).
**Trade-offs and review point**: The archived `contracts.md` rule ("at most three total attempts") is amended in ADR 0029 to read "three attempts per DDC primitive; a restore may start at most three further operations". Writes are now retried on Windows (macOS already does; the Windows shell does not). A DDC set is an absolute write, so repeating it is idempotent.

### Decision 7: Rejected submissions are bounded, not re-run (refines spec Decision 2)
**What**: When the shell reports that `tv_submit` rejected a `hardware_result`, the machine keeps the stored result and resubmits it, at most 3 times with the same 200 ms and 1000 ms backoff, and does not re-run the hardware write. After the bound it marks the action locally completed, drops it, and emits a `draft_changed` effect; the failed status is read with `query_draft`.
**Why**: Spec Decision 2 asked for "re-offer of a rejected hardware_result" (Windows). The exploration found that Windows implements it by clearing `completed_`, which re-runs the DDC write on every later snapshot with no cap and no backoff (a core range-check rejection at `core.rs:1096` would loop forever on the wire). macOS drops the result and can leave a manual slot stuck. Resubmitting the stored result keeps the intent of the decision and removes both failure modes.
**Trade-offs**: A deliberate behavior difference from both shells; covered by new tests.

### Decision 8: Faithful port, with fixes only where unifying the layers removes the cause
**What**: The Rust port reproduces the Windows behavior case by case (the Windows manual-control (34), key-router (34) and tracker (5) cases, and the dispatch and retry behavior of the controller and transaction tests; the C++ discovery and transport tests stay in C++). It also fixes exactly these defects, because they exist only at the seams between components that are now one:
- The completion is checked against the connection generation of the in-flight command, so a late completion cannot release a newer in-flight command (Windows quirk 1, macOS stuck slot). There is no completion token.
- A key intent is finished with the real hand-off outcome (the `accepted` result of `set_target`), not a constant `true` (Windows Q2: a dropped write leaves a phantom base). This path is unreachable through the event API today, because 64 monitors x 2 features = 128 = `MAX_DRAFTS`, so the slot cap never rejects a hand-off; it holds by construction (`hand_off_key_intents` in `controls/mod.rs`).
- No intended fix is missing from this list; every other defect is preserved as a known quirk.
- The slider and key observer callbacks disappear; the machine sees both in one place (removes the callback-order race, Windows quirk 6, and the slider-versus-readback race, Q11).
- Percent comes from the observed range of the feature, not a captured maximum (Q8).
- The re-offer loop is bounded (Decision 7).
Everything else the exploration flagged is preserved and recorded as a known quirk with a test marked `known_quirk_`: reconcile deleting intents on unknown readback, flush erasing an intent while a write is in flight, the 128-in-flight stall, the inert mute-repeat flag, the early-completion map clearing whole on overflow, the readback clearing a draft while a command is still in flight.
**Why**: Equivalence first makes the port reviewable against the existing tests; fixing a quirk and moving it in the same change makes failures impossible to attribute.

### Decision 9: Strict JSON, shared with the camera lifecycle
**What**: Extract the strict parser (unique keys at any depth, finite numbers, no trailing data, unknown fields rejected, size cap before parsing, validate-then-mutate) from `camera_lifecycle.rs` into `src/strict_json.rs` and reuse it. Invalid input returns `TV_INVALID_ARGUMENT` or `TV_SIZE_LIMIT` and leaves state unchanged.
**Why**: One tested parser, identical rejection behavior across handles.
The `StepError` type now lives in `strict_json.rs`; `camera_lifecycle.rs` and `controls/mod.rs` re-export it (one definition).
**Trade-offs**: A refactor of `camera_lifecycle.rs`; its 44 contract tests guard it.

### Decision 10: `core.rs` is nearly untouched
**What**: Only `map_normalized` becomes `pub(crate)` (`core.rs:1583`); the raw-range helper stays private. No controls code calls it: `manual.rs` (`raw_target`) repeats the formula, and `dispatch.rs` only rounds the raw value the core already published, so the formulas must stay equal to `core.rs`. The core's `pending_actions` queue, coalescing, generations and `hardware_result` handling stay as they are (spec Decision 4).
**Why**: The working tree has uncommitted edits to `core.rs` and `settings.rs` from `inference-device-selection`. Keeping this feature in new files makes the sequencing in spec Decision 6 cheap.

### Implementation decisions recorded after the design

- **External-intent flow not ported.** `adoptExternalIntent`, receipts and early-completion buffers were not ported. Key intents share the slider queue, so a key-driven draft needs no separate adoption path. Only macOS used the flow, with receipts; Windows had no production caller.
- **Separate step settings and volume owner.** `configure` carries `brightness_step` and `volume_step` separately, plus an optional `volume_owner`. macOS has two step settings. The volume owner is needed to clear the mute memory when the output changes without a key press.
- **Normalized observation.** `observe.observed` is the normalized percentage (the snapshot field `normalized`). The Windows shell previously read the raw `observed` as a percentage, which is correct only for 0 to 100 ranges.
- **One execution slot.** The dispatcher holds one execution slot across monitors. A monitor that is only waiting (settle, backoff, grace) does not hold it.
- **Bounded result resubmission.** A rejected `hardware_result` is resubmitted after 0.2 s, then 1.0 s, then 1.0 s. A fourth rejection drops the action locally.
- **macOS adapter deferred.** The owner deferred the macOS adapter to be implemented on a Mac (2026-10-18).

## Architecture Overview

**Before.** Three queue layers with a copy per shell: (1) UI drafts and a coalescing drain (`ManualControls.cpp` / `ManualMonitorControls.swift`), (2) key intents, rebase, mute and latch (`KeyIntentRouter.cpp` / `HardwareKeyRouter.swift`), (3) the core's bounded `pending_actions`. A DDC controller per shell (`DdcController.cpp` / `MacDDCController.swift`) pulls pending actions from snapshots, retries inside blocking `sleep` calls on one worker, and posts `hardware_result`.

**After.**

```
 UI / key capture (shell) ──events + now──▶  TvControls (Rust, sans-IO)
        ▲                                      │  drafts, coalescing, in-flight, keys,
        │ draft_changed                        │  rebase, grace, DDC attempts, retry budget
        │                                      ▼
 shell executor ◀──── effects ────────────  { submit_command | run_attempt |
   tv_submit(command_json)                      submit_result | draft_changed }
   one DDC primitive on the worker              + next_deadline
   one timer for next_deadline
        │
        └──── attempt_done / submitted / tick / observe ───▶ TvControls
```

The core (`Core`, `tv_submit`, `tv_snapshot`) is unchanged and remains the only owner of protection policy, action generations, saved brightness and restoration ownership. `TvControls` sits beside it and reads its published state through `observe`.

### Component Structure

**New Files/Modules (Rust, `crates/telaviva-core/src/`)**
- `strict_json.rs`: shared strict parser extracted from the camera lifecycle (Decision 9).
- `controls/mod.rs`: `Controls` state machine, `step(&mut self, &[u8]) -> Result<String, StepError>`, effect list, deadline computation.
- `controls/manual.rs`: slots, drafts, latest-wins queue, one in flight, safety release, readback clearing, 128-draft cap.
- `controls/keys.rs`: step and fine step, policy latch, intents, rebase, outstanding and settle tracking, mute memory.
- `controls/dispatch.rs`: takes `pending_actions` from `observe`, one action per monitor, hold during wake grace or sleep, running and completed bookkeeping, result submission and bounded resubmission.
- `controls/retry.rs`: attempt budget, backoff, permanent versus transient errors, per-manufacturer timing profile (`GSM` duplicates the set, others do not), wake grace.
- `controls/model.rs`: event and effect types, JSON parsing and rendering.
- FFI additions in `ffi.rs`: `TvControls`, `tv_controls_create`, `tv_controls_step`, `tv_controls_destroy`.

**New Files (native)**
- `native/windows/Platform/ControlsApi.h/.cpp`: `IControlsApi` seam (implemented by `CoreControlsApi`) over the three exports, serialized by one mutex, plus JSON encode and decode through the existing `JsonCodec` (`Windows.Data.Json`), not the `rawField` text search the camera driver uses, because effects are arrays.
- `native/windows/Platform/ControlsDriver.h/.cpp`: pure adapter. Turns UI, key-consumer, snapshot and executor callbacks into events, runs effects after unlocking, owns the one timer through `IScheduler`.
- `native/windows/Platform/ControlsTypes.h` and `IAttemptRunner.h`: shared control types and the seam that runs one DDC primitive.
- `native/macos/TelaViva/ControlsBridge.swift` (planned, not written yet): the Swift equivalent over `JSONSerialization` and `Codable`, fault handling as in `CameraLifecycle.swift` (a non-OK status stops the handle).
- `tools/tests/test-controls-bridge.sh` (planned, not written yet) and a CI step, mirroring `test-core-bridge.sh`.

**Modified Files**
- `crates/telaviva-core/src/lib.rs`: `pub mod controls; mod strict_json;`.
- `crates/telaviva-core/src/camera_lifecycle.rs`: use `strict_json` (no behavior change).
- `crates/telaviva-core/src/core.rs`: `pub(crate)` on `map_normalized` only.
- `include/telaviva_core.h`, `crates/telaviva-core/tests/header_contract.rs` (exports table, prototype strings, opaque handle list), `api.md` (new ABI section), `CHANGELOG.md`.
- Windows: `AppController.cpp` (wire the driver, delete `ManualCompletionGate`, `CoreManualSink`, the volume tracker plumbing, `runKeyConsumer` policy calls), `DdcController.cpp` (reduced to discovery, display state, the port and "run one primitive"). Drafts reach the flyout through the flyout state built in `AppController.cpp`, so `MonitorCard.xaml.cpp` does not change. `DdcTransaction.cpp` and `DdcTimingProfile.cpp` are not reduced to one primitive: they stay for device discovery reads and the `--probe-ddc` diagnostic, so discovery retry timing stays duplicated until it moves to the core.
- macOS: `ManualMonitorControls.swift`, `HardwareKeyRouter.swift` (decode, tap, permissions only), `CoreBridge.swift`, `MacDDCController.swift`, `DDCTransaction.swift` (framing, checksum seed fallback and `writeTwice` stay inside one primitive), `MenuBarController.swift`, `MonitorPopup.swift`.

**Deleted after equivalence is proven**: `Platform/ManualControls.*`, `Platform/KeyIntentRouter.*`, `Platform/VolumeActionTracker.*`, the Swift `ManualCommandAuthorization`, receipts and early-completion buffers, `HardwareKeyPolicy`, `HardwareKeySequence`, and the router's intent and rebase code. `Platform/DdcTransaction.*` and `Platform/DdcTimingProfile.*` are excluded (see Modified Files). The Windows files listed above were removed with `git rm` (their cases were ported to the Rust tests); the Swift code is removed when the macOS adapter lands.

### Component Relationships

- `observe` is the only way the handle learns about monitors (generation, display state, identity status, per-feature supported, observed, min, max) and `pending_actions` (id, generation, monitor, kind, value). Eligibility is derived exactly as `ManualControls::apply` does today.
- `slider` and `key` events create or replace the single queued target for a (monitor, feature). A key intent and a slider target share the slot, so a slider move invalidates a handed-off key intent and the reverse, as in the Windows tests.
- Dispatch consumes `pending_actions` after the core has accepted a `submit_command`; the core's own queue still coalesces and bounds them (128, one per monitor exposed, at most 8 in the snapshot).
- Retry owns the timing of each primitive; dispatch owns which action runs; manual owns what the user sees.

### Data Flow

**Slider drag (US-003).** The UI thread sends `slider {monitor, feature, value}`; the result returns the draft immediately and arms `next_deadline = now + 20 ms`. On `tick`, the machine emits one `submit_command` (latest value only). The shell calls `tv_submit(command_json)` and sends `submitted {token, accepted}`. The core publishes a snapshot; the shell sends `observe`. The machine sees the listed action, emits `run_attempt {write}`; the worker runs one primitive and sends `attempt_done`. The machine waits 60 ms (deadline), emits `run_attempt {read}`, compares the readback with the target, then emits `submit_result {result_json}`. After the shell reports `submitted`, the machine clears the in-flight state, and a queued newer target drains with no debounce (0 ms), as in Windows test 27.

**Held key (US-003, US-005).** `key {down}` returns `consume` and, if reserved, a target in the same queue. Repeats accumulate into the pending target; the machine never rebases while its own write is outstanding or inside the 1 s settle window, which is the fix behind Windows commit `073be42`, now stated once.

**Sleep, disconnect, generation change, shutdown (US-004).** `observe` showing asleep, a missing monitor or a new generation drops slots and queued targets with no replay. `display_power {on:false}` clears grace; `{on:true}` arms the 2 s grace and holds dispatch. `shutdown` stops everything; late `attempt_done` and `submitted` events are ignored.

## Implementation Approach

### User Story Mapping

**US-001 (fix once)** and **US-005 (new behavior in the core only)**: `controls/*`; adapters contain no queue logic. Testing: a bug becomes a failing core test first.

**US-002 (test once)**: port each existing case to `crates/telaviva-core/tests/controls_contract.rs` (manual, keys, dispatch, retry), table-driven with an injected `now`; add `tests/fixtures/controls/*.json` scenario scripts (events and expected effects) that the Rust test and both shell bridge tests replay. The shell suites keep only seam tests: effect execution, timer arming, JSON round trip, fault handling.

**US-003 (same behavior on both platforms)**: the scenario fixtures assert the same sequence of effects for drag, held key, external change, sleep and wake, disconnect. Adapter smoke tests replay one fixture through the real library on each platform.

**US-004 (nothing replays)**: ports of the sleep, generation rollover, disappearing monitor and shutdown cases, plus a late-completion-after-rollover case that the Windows suite lacks.

### Port Map (existing test to new home)

- Windows `manual_controls.cpp` (34 cases) to `controls_contract.rs::manual_*`.
- Windows `key_intent_router.cpp` (34 cases) and `volume_action_tracker.cpp` (5 cases), and macOS router and sequence cases, to `controls_contract.rs::keys_*`. The macOS brightness-key cases are new coverage for the generalized feature.
- Windows `ddc_transaction.cpp` (13 cases) stays in C++, because it still tests the discovery transport. The macOS `DDCTransactionTests.swift` cases move to `controls_contract.rs::retry_*`. Cases pinned on both sides (60 ms settle, 200/1000 backoff, GSM duplicate) become one.
- Windows `ddc_controller.cpp` (18 cases; the discovery cases stay in C++) cases about grace, asleep, one-per-monitor, completed bookkeeping to `controls_contract.rs::dispatch_*`.
- New: bounded resubmission, the 12-attempt ceiling, write retry, generation-guarded completion, `known_quirk_*` cases.

## Integration Points

- `Core` and `tv_submit`: unchanged contract; the shell still submits `set_brightness`, `set_volume`, `hardware_result`, `monitor_*` events exactly as today, now built by the handle for the first three.
- Shell discovery stays in the shells (`DdcController::refreshOnExecutor`, `MacDDCController` discovery). They tell the handle about a monitor with `monitor_profile {monitor_id, manufacturer}` and `monitor_removed {monitor_id}`.
- Windows: `IScheduler` (thread-pool timers), `IClock.steadySeconds()`, `ThreadExecutor` as the single DDC worker. macOS: `com.telaviva.ddc` serial queue, `CMClockGetHostTimeClock`.
- Build: Windows globs `Platform/*.cpp`, so new files compile automatically; macOS needs entries in `packaging/macos/build-app.sh` `SOURCES` and `TelaViva.xcodeproj`. `tools/windows/check-dll-exports.ps1` validates the DLL against the header in both directions.

## Technical Constraints

- The core reads no clock and owns no thread or timer (ADR 0014). `now` is monotonic seconds from the shell; wall-clock time never enters.
- Windows hook thread must not call the core (ADR 0021). The hook keeps its atomic route and lock-free latch. The key consumer thread calls `tv_controls_step`.
- macOS: never take `ffiLock` for a handle call; the handle's own mutex is a leaf lock.
- ABI version stays 1; the change is additive and checked by `header_contract.rs`.
- Inputs are capped (64 KiB per event, as the camera handle); an `observe` for 64 monitors is well under that.
- Every collection stays bounded (128 drafts, intents, in-flight, outstanding and mute entries; 64 early completions; 256 remembered action ids), including the dispatcher's profile map: it holds at most one profile per monitor (64). At the cap a new `monitor_profile` evicts the profile of a monitor that is absent from the latest snapshot, and is ignored (default profile) when every stored profile belongs to a listed monitor; `monitor_removed` also drops one.
- Application version, tests and docs follow AGENTS.md: both platforms in one change, en-US and pt-BR strings unchanged by this feature, `CHANGELOG.md` entry, hardware claims only from hardware checks.
- The file and line references in this document and in the plan come from exploration reports. The plan phase must re-read each source file before porting a rule; where a report and the source disagree, the source wins.

## Risks & Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Moving retry and pacing changes real DDC timing and ordering (no more blocking sleeps; other monitors can interleave) | Flicker, lost writes or bus contention on specific monitors | Keep at most one primitive executing at a time (the shells already use one worker); make the delay values identical to today's; run the Windows hardware checklist and the macOS validation report before declaring done |
| Behavior changes in Decisions 6 and 7 (write retry on Windows, GSM-only duplicate set on macOS, bounded resubmission) | Hardware-specific regressions (macOS duplicates every set today) | List each change in the ADR and CHANGELOG; keep `duplicate_set` as profile data so a monitor can be added without code; verify on the LG UltraFine (manufacturer `GSM`) and one non-GSM monitor |
| The macOS tap makes a synchronous call into the core | Tap latency or timeout disable | Leaf mutex, no I/O, no `ffiLock`; time the call in a test; documented fallback in Decision 4 |
| Port drift: the Rust port differs subtly from the C++ original | Same bugs, new place | Port case by case from the tests, replay shared fixtures, keep the C++ tests alive until the Rust cases pass, then delete |
| Large diff touching both shells | Review and merge risk | Phase the plan: Rust machine and tests first; Windows adapter; macOS adapter; deletions last |
| Overlap with uncommitted `inference-device-selection` work | Merge conflicts in `core.rs`, `Platform/` | Spec Decision 6: finish and commit that branch first; this design keeps `core.rs` changes to one visibility edit (`map_normalized`, now `pub(crate)`) |
| Handle and core disagree on world state (late `observe`) | Stale decisions | Revision-ordered `observe` (stale ones ignored, equal accepted, as `ManualControls::apply` does); safety release covers lost commands |
| JSON string inside JSON (`command_json`) mangled by a shell | Rejected commands | Contract test on each shell passes the string through the real library; document "pass verbatim" in `api.md` |
| macOS adapter pending | The repository carries the handle and the Windows adapter while macOS still runs its own Swift queue; AGENTS.md requires both platforms in one change | Owner scoped the macOS work to a Mac session; finalize and release after it |

## Open Questions

None block the plan. Four decisions deviate from, or extend, what the owner already approved and are flagged for review before `/codeadvisor-dev:plan`:

1. Decision 4: one synchronous call from the macOS event tap into the handle.
2. Decision 6: the retry budget is defined once with a pinned worst case instead of one shared shrunken budget; writes are retried on Windows.
3. Decision 7: rejected result submissions are resubmitted at most three times, not re-run.
4. Decision 8: which exploration findings are fixed and which are kept as known quirks.

## Next Steps

1. Review this architecture and the four flagged decisions.
2. Finish and commit `inference-device-selection` (spec Decision 6).
3. Run `/codeadvisor-dev:plan`.
