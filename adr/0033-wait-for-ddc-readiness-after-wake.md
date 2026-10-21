# ADR-0033: Wait for DDC readiness after a display wake inside the controls dispatcher

**Status:** Accepted
**Date:** 2026-10-19
**Feature:** brightness-stays-at-zero-after-slow-monitor-wake
**Spec:** [Archived spec](../specs/archived/brightness-stays-at-zero-after-slow-monitor-wake/spec.md)

## Context

After a display sleep and wake, an external monitor that protection had dimmed to 0 % could stay at 0 %. The slider showed 0, and nothing raised it until the user moved it. On macOS the core's wake read gave up after about 7 seconds: with `MAX_RESTORE_FAILURES` = 3, the fourth failed read dropped ownership, so no later event restored the baseline. The camera then stayed suspended as `suspended_low_brightness`, so presence protection could not recover it either. The bug was reported once, on a work monitor with the lid closed. The code path is traced in the [bug plan](../specs/archived/brightness-stays-at-zero-after-slow-monitor-wake/spec.md). The time that the monitor takes to answer DDC after wake has not been measured on hardware.

[ADR 0006](0006-keep-brightness-restore-pending-after-failed-wake-readback.md) keeps the restore pending after a failed wake readback. It named this case as its main residual risk: a monitor whose DDC takes longer than about 7 seconds to answer after power-on can still stay dimmed. It deferred a readiness wait on the native side.

The budget is counted in attempts, not in time. Each core read is one controls operation of at most three attempts with a 0.2 s then 1.0 s backoff ([ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md)), so a failed read ends after about 1.2 s plus I/O time, however long the monitor needs to answer. Windows has the same path: the same 2 s grace and the same three-attempt budget. The port reopen after a failed attempt ([ADR 0032](0032-reopen-ddc-ports-after-topology-change.md)) adds no wait.

## Decision

- `display_power(on)` opens a wake readiness window of `WAKE_READY_WINDOW_SECS` = 30 s, in addition to the existing 2 s `WAKE_GRACE_SECS` hold. `display_power(off)` and `shutdown` close it.
- An operation that starts while the window is open records the window's end. While `now` is before that end, a transient failure (`io`, or a successful read without a value) does not consume the per-primitive budget of `ATTEMPTS_PER_PRIMITIVE` = 3. The primitive is retried every `WAKE_READY_RETRY_SECS` = 2 s. Permanent errors (`unsupported`, `disconnected`) and a readback mismatch keep their current behavior.
- The attempt count is capped at `ATTEMPTS_PER_PRIMITIVE` - 1 inside the window, so the first attempt after the window still runs with the normal budget. After the window, the normal budget (3 attempts, 0.2 s then 1.0 s backoff) applies to the attempts that remain, so the operation finishes as it does today.
- The rule lives in the shared controls dispatcher (`crates/telaviva-core/src/controls/retry.rs` and `dispatch.rs`), so macOS and Windows get it together, with no change to either shell's logic. Each failed retry still reopens the DDC port, as [ADR 0032](0032-reopen-ddc-ports-after-topology-change.md) requires.
- The core receives one `hardware_result` per wake read: a success as soon as the monitor answers, or a failure only after the window. `MAX_RESTORE_FAILURES`, the read-before-write rules in `core.rs` and the ownership rules are unchanged.

## Alternatives Considered

- **Raise `MAX_RESTORE_FAILURES`.** Rejected: the bound stays counted in attempts and unpaced. The failed reads would repeat at about 1.2 s intervals, and a monitor slower than the new total would still lose the restore.
- **A native readiness probe in each shell.** Rejected: it would be written once for macOS and once for Windows, and the two copies would drift. That is the duplication that [ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md) removed for the queue. [ADR 0006](0006-keep-brightness-restore-pending-after-failed-wake-readback.md) also noted that a wait before `awake` would block the DDC queue while the display is not ready.
- **Unbounded retry.** Rejected: a monitor that never answers would receive DDC traffic for as long as the display stayed on.

## Consequences

- A monitor that answers within 30 s after wake is restored by the wake read, and the restore counter is not reached. A monitor that never answers costs about 15 extra attempts per waiting operation (30 s at 2 s), and only in the 30 s after a wake.
- Any operation that starts in the window waits for an answer, including a manual slider write. A manual write to a monitor that does not answer stays pending until the window ends, instead of failing in about 1.2 s. The normal budget then applies.
- The figures in ADR 0006 (about 7 s) describe the behavior only outside this window. The limit of three attempts per primitive in ADR 0029 holds outside it too.
- Not verified on hardware. If the work monitor takes longer than 30 s to answer, raise `WAKE_READY_WINDOW_SECS`, not `MAX_RESTORE_FAILURES`.
- The window does not fix two other paths. A monitor rediscovered while dimmed captures the dimmed value as its new baseline (ADR 0006), and a display sleep during an in-flight dim write is ignored by the core. Rediscovery at the dimmed level under the same id is resolved by the [ADR 0006 amendment](0006-keep-brightness-restore-pending-after-failed-wake-readback.md#amendment-2026-10-21-restore-a-dim-after-reconnect). Rediscovery at another level or under another id, and a display sleep during an in-flight dim write, remain open. The bug plan lists those as owner decisions.

## Non-Obvious Constraints

- The window is decided when an operation starts. An operation that starts before `display_power(on)` keeps the normal budget even if it runs into the window. An operation that starts in the window keeps the window's end until it finishes. An operation that starts after `display_power(off)` gets no window.
- `display_power` is not per monitor, so the window applies to the waiting operations of every monitor at once.
- The window's end is a point on the shell's monotonic clock, as passed in `now`, so it bounds time, not the number of attempts.
- The regression tests are `read_keeps_retrying_inside_the_wake_ready_window`, `read_falls_back_to_the_normal_budget_after_the_window`, `permanent_errors_end_at_once_inside_the_window`, `readback_mismatch_stays_final_inside_the_window` and `operation_without_window_keeps_three_attempts` in `crates/telaviva-core/src/controls/retry/tests.rs`; `wake_window_keeps_a_failing_read_running_until_the_monitor_answers`, `display_power_off_closes_the_wake_window`, `operation_started_after_the_window_uses_the_normal_budget` and `shutdown_clears_the_wake_window` in `crates/telaviva-core/src/controls/dispatch/tests.rs`; and `slow_monitor_wake_read_is_reported_once_after_it_answers` in `crates/telaviva-core/tests/controls_contract.rs`. This ADR does not record their results.
