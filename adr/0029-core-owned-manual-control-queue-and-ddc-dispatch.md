# ADR-0029: Core-owned manual-control queue, key policy and DDC dispatch behind a clock-injected handle

**Status:** Accepted for the core and the Windows shell; the macOS adapter is pending (owner decision, 2026-10-18)
**Date:** 2026-10-18
**Feature:** core-command-queue
**Spec:** [Feature spec](../specs/core-command-queue/spec.md) (the folder moves to `specs/archived/` when the macOS adapter lands)

## Context

The logic that turns a slider move or a hardware key into DDC writes existed twice: drafts, latest-wins coalescing and one write in flight (`ManualMonitorControls.swift` and `Platform/ManualControls.cpp`), key steps, intents, rebase and mute memory (`HardwareKeyRouter.swift` and `Platform/KeyIntentRouter.cpp`), and DDC retry and pacing (`DDCTransaction.swift` and `Platform/DdcTransaction.cpp`). The same queueing bugs were fixed on macOS and then fixed again on Windows, each copy had its own tests, and the copies had already drifted: a 20 ms coalescing timer on Windows only, a 3 s in-flight safety release on Windows only, a 0.51 percent readback tolerance on Windows against raw equality on macOS, and different handling of a rejected `hardware_result`. On Windows a rejected result cleared the completed set and re-ran the DDC write on every later snapshot, with no cap and no backoff.

The queue also had three layers: UI drafts in the shell, key intents in the shell, and the core's bounded `pending_actions`.

The earlier decisions point the other way. [ADR 0001](0001-native-shell-and-rust-core.md) gives serial DDC execution to the native adapters. [ADR 0004](0004-external-monitor-hardware-keys-and-fluid-controls.md) and [ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) put draft coalescing and debounce in the shell, and [ADR 0021](0021-windows-volume-key-hook-and-route.md) says the Windows hook never calls the core. [ADR 0014](0014-camera-lifecycle-in-the-core.md) is the precedent for moving shell policy that already takes time as a parameter into the core behind an opaque handle, with the shell keeping timers and devices. It also rejects sending high-rate events through `tv_submit`, because every accepted command bumps the revision and republishes the snapshot.

## Decision

- **A second opaque handle, `TvControls`.** `tv_controls_create`, `tv_controls_step` and `tv_controls_destroy` are additive to C ABI version 1. One JSON event goes in and one JSON result comes out. Every event carries the shell's monotonic `now`; the core reads no clock and owns no thread or timer. The code is in `crates/telaviva-core/src/controls/` (`model`, `manual`, `keys`, `dispatch`, `retry`) with the strict JSON parser shared with the camera lifecycle in `strict_json.rs`. The contract is in [api.md](../api.md).
- **The core owns the policy.** The manual-control draft, latest-wins coalescing (20 ms) and one write in flight per monitor and feature; the 3 s in-flight safety release; the key step, fine step, mute memory, held-key accumulation, intents, rebase against external changes, outstanding-write and settle tracking and the per-sequence key policy; the dispatch of the core's pending actions with one DDC primitive executing at a time across all monitors; and the DDC retry and pacing policy.
- **The shells keep the device side.** Key capture (the event tap and the low-level hook), the swallow decision on the Windows hook thread, device discovery, running one DDC primitive on the worker, and the single timer armed from the returned `next_deadline`. The core's `consume` answer is traced only on Windows, where the hook decides from the route; macOS is meant to use it.
- **The result is a list of effects.** `submit_command` and `submit_result` carry the exact JSON text that the shell passes to `tv_submit` (`command_json`, `result_json`), so both shells send identical bytes; `run_attempt` asks for one DDC primitive; `draft_changed` asks the UI to repaint. The shell reports each outcome back with `submitted` and `attempt_done`.
- **One retry budget, defined once.** At most 3 attempts per DDC primitive (write, duplicate write, read), with a 0.2 s then 1.0 s backoff; `unsupported` and `disconnected` are permanent; a readback mismatch is never retried; a 60 ms settle follows the last write; the duplicate write is sent only for the `GSM` manufacturer profile; a 2 s grace holds actions after a display wakes. Writes are retried, as the macOS code already did.
- **Rejected result submissions are bounded.** The stored result is resubmitted at most 3 times (0.2, 1.0 and 1.0 s) and the write is never re-run; after the next rejection the action is dropped locally and the draft is marked failed.
- **Percentages are normalized.** `observe` carries each channel's normalized percentage (the snapshot's `normalized` field) and the raw range. The Windows shell used to read the raw `observed` value as a percentage, which is only correct for 0 to 100 ranges.
- **Faithful port with a few fixes.** The Windows behavior is reproduced case by case. A completion is checked against the connection generation of the in-flight command, a key intent is finished with the real outcome of its hand-off, the observer callbacks that raced are gone, and the unbounded re-offer loop is replaced by the bounded resubmission. Other quirks of the originals are kept and pinned by `known_quirk_` tests so a later change is deliberate.
- **The Windows shell uses the handle.** `ControlsDriver` in `native/windows/Platform` drives it, `DdcController` runs one primitive per effect, and `ManualControls`, `KeyIntentRouter` and `VolumeActionTracker` were removed. `DdcTransaction` and `DdcTimingProfile` stay for device discovery and the `--probe-ddc` diagnostic.

## Alternatives Considered

- **Send the events through `tv_submit`.** Rejected: slider ticks and key repeats arrive at input rate, and each accepted command would build and publish a snapshot.
- **Leave the queue in the shells and port every fix.** Rejected: it is the duplication this decision removes.
- **A core-owned timer thread.** Rejected: it breaks the rule that the core owns no thread and complicates shutdown. Returning one absolute deadline keeps the core deterministic under test.
- **One shared episode budget of N attempts for the per-primitive retries and the restore retries of [ADR 0006](0006-keep-brightness-restore-pending-after-failed-wake-readback.md).** Rejected: it would shrink the resilience that ADR added for monitors whose DDC is slow after wake, with no hardware evidence either way. The two layers stay, and the product is bounded and pinned by a test.
- **Keep re-offering a rejected result by clearing the completed set.** Rejected: it re-runs the write without limit.
- **Move the per-key swallow latch of the Windows hook into the core.** Rejected by the owner; the hook thread must decide without calling the core ([ADR 0021](0021-windows-volume-key-hook-and-route.md)).

## Consequences

- The queueing rules exist once in Rust, with unit tests, contract tests, four end-to-end tests that drive a real core, and scenario fixtures that the shell adapters are meant to replay. The Windows native suite keeps adapter tests (614 tests) and the ported cases live in Rust.
- **The macOS adapter is not written.** The macOS shell still runs its own Swift queue (`ManualMonitorControls`, `HardwareKeyRouter`, and the retry in `DDCTransaction`). The owner will implement it on a Mac. Until then the repository carries the handle and the Windows adapter, which does not meet the rule that a change lands on both platforms at once; the owner scoped the macOS work on 2026-10-18.
- **ADR amendments.** ADR 0001 now gives the core the manual-control queue, the key policy and the DDC retry policy, while the native adapters keep device access, discovery and the execution of single primitives. ADR 0021's router decisions (the step, rebase guard and latch as a C++ port, the duplicated router logic, the unwired outstanding-action inputs) no longer describe Windows; the hook, the lock-free route and the container-id ownership stay. ADRs 0004, 0005 and 0008 describe the macOS shell and remain true for it until its adapter lands; the macOS event tap will then make one synchronous call into the handle to get `consume`, which amends ADR 0004's rule that the tap never calls the core. If that call proves too slow, the macOS shell keeps `HardwareKeySequence` and calls the handle asynchronously.
- **Behavior changes that need hardware checks.** There are no blocking sleeps on the DDC worker, writes are retried on Windows, the duplicate write is limited to `GSM` (macOS duplicated every write), and a rejected result no longer re-runs the write. None is verified on a monitor yet; the cases are in the Windows hardware checklist.
- **The archived rule of three total attempts** is read as three attempts per DDC primitive, with the core's restore retries starting at most three further operations. One monitor that never answers costs at most four failed read operations, 12 primitive attempts and about 4.8 s of backoff waiting.
- Device discovery still reads through `DdcTransaction` with its own retries on Windows, so that retry timing stays duplicated until it moves to the core.

## Non-Obvious Constraints

- `now` is monotonic seconds from the shell clock and is clamped so it never moves backward. An event is validated completely before any state changes; an invalid event returns `TV_INVALID_ARGUMENT` or `TV_SIZE_LIMIT` and leaves the state as it was.
- Each handle has one leaf mutex. A shell must never hold another lock that a step could wait on, and on macOS must not take its core lock for a handle call. The Windows hook thread never calls the handle; the key consumer thread does.
- `observe.observed` is the normalized percentage and `min`/`max` are the raw range. A pending action's `value` is the raw target, rounded by the machine.
- The in-flight safety release, the rebase settle window and the outstanding-write expiry have no deadline of their own; they are evaluated on the next `observe` or `key` event, as in the Windows original. `next_deadline` covers the 20 ms drain, DDC waits and the wake grace.
- Sixty-four monitors with two features each equal the 128-draft cap, so the "write dropped by the draft cap" path cannot be reached through the event API.
- `keys_enabled: false` clears the volume owner, and a `volume_owner` in the same event is then ignored. A change of the volume owner clears all remembered mute levels.
- A monitor without a `monitor_profile` event runs with the default timing, and an action for a monitor that is not in the latest `observe` fails with `disconnected`.
- The Windows driver ignores a non-OK ABI status instead of stopping the handle, so a dropped event costs one missed update; the contract documents this.
