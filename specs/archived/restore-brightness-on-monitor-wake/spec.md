# Bug: Brightness is not restored when a dimmed monitor wakes after a failed readback

## Bug Description

When an external monitor is dimmed by protection (presence or screensaver) down to a low value, goes to sleep (or is switched off) and is then switched on again, it should get its previous brightness back immediately. In one observed run the monitor came back at 0 % brightness and the app never restored it. Expected: on wake, the app reads the monitor, confirms it still holds the value the app set, and restores the saved baseline. Actual: the monitor stays at the dimmed value until the user changes brightness manually.

## Problem Statement

After a wake, the restore depends on a single DDC readback and a single restore write. If either fails (typically because the monitor's DDC channel is not ready right after power-on), the core permanently forgets that it owns the dimmed value, and nothing ever retries the restore.

## Solution Statement

Keep the restore pending when a wake verification read or a restore write fails, and retry it a bounded number of times (3). Ownership, baseline and the saved raw value are kept during the retries; the error status stays visible. After the bound is reached the current behavior applies (ownership dropped, error reported). Read-before-write is preserved: a restore write is only enqueued after a readback that matches the value the app last set. Dim writes, manual writes and volume writes keep their current failure behavior.

## Steps to Reproduce

1. Protection dims an external monitor to its minimum (presence absent twice, or screensaver active) and the dim write succeeds (`brightness.owned = true`, raw 0, baseline e.g. 70).
2. The display goes to sleep (`display_state` asleep): queued actions are dropped and the presence reason is cleared.
3. The display wakes (`display_state` awake). The core enqueues `read_brightness` (status `verifying_ownership`).
4. The readback fails (`hardware_result` with `ok: false`, e.g. `error: "unsupported"`), because DDC is not ready yet.
5. Observe: `owned` is false, no reasons, no pending actions, the monitor stays at 0 %. No further restore is ever attempted.

## Root Cause Analysis

In `crates/telaviva-core/src/core.rs`:

- `display_state` (about l.344-372) clears `presence_dimmed` and `ownership_checked_target` on every state change and, on wake, sets `wake_ownership_check`; `reconcile_all` then enqueues a verification read when the observed value differs from the baseline.
- `hardware_result` for a failed `read_brightness` (about l.895-904) and for a failed `set_brightness` (about l.931-938) sets `feature.owned = false`.
- `reconcile_all` (about l.748-752) computes a restore target only when a protection reason is active or `owned` is true. After the wake the reasons are empty and `owned` is now false, so the target is `None` and none of the enqueue paths (about l.754-826) runs again.
- The baseline is still stored, but it is unreachable; `shutdown_policy` also restores only owned monitors.

The Swift side makes this likely: after `screensDidWake` it reports `awake` without checking that DDC responds (`MacDDCController.refreshDisplayStatesOnQueue`), and `DDCTransaction.readVCP` gives up after about 1.7 s of retries.

This is established by reading the code, not by a hardware log: the user's run was not instrumented. Other possible contributors (not fixed here, see Notes): a monitor that drops out of discovery and is re-discovered while dimmed captures the dimmed value as its new baseline; an active screensaver reason blocks the restore by design.

## Relevant Files

- `crates/telaviva-core/src/core.rs`: `Monitor` struct (l.37) and its discovery constructor (l.273), `display_state`, `manual_set`, `hardware_result` failure branches, `reconcile_all`.
- `crates/telaviva-core/tests/core_contract.rs`: regression tests next to `wake_read_failure_does_not_issue_a_protection_write` (about l.883); fixtures: `send`, `capture_time`, the `monitor_discovered` fixture with id `screen`, raw 70 of max 100, the dim sequence of `returning_sample_restores_owned_brightness_and_cadence_is_one_second` (about l.1047, 1063-1079), the asleep/awake transitions (about l.831-861).
- `CHANGELOG.md`, `ARCHITECTURE.md` (wake reconciliation paragraph, about l.121): document the behavior.

### New Files

None.

## Step by Step Tasks

### 1. Write the failing regression tests first [x]

In `crates/telaviva-core/tests/core_contract.rs`, after the test at about l.883, add:

- `failed_wake_readback_keeps_ownership_and_retries_the_restore`: dim the monitor to 0 (baseline 70), ack the dim, asleep, awake; fail the wake `read_brightness`; assert the monitor still reports owned, a new `read_brightness` is pending, and no `set_brightness` was issued; complete that read with value 0 and assert `set_brightness` 70 is queued; ack it and assert raw/observed 70, owned, no pending actions.
- `failed_restore_write_is_retried_after_a_new_readback`: same setup, readback ok 0, then fail the `set_brightness` 70; assert ownership is kept, a new read is pending before any new write, then readback 0 and a new set 70 succeed.
- `restore_retries_are_bounded`: fail the wake readback four times in a row; assert after the retries are exhausted that `owned` is false, the status is the error, and no actions are pending (today's behavior).
- `manual_brightness_change_wins_over_a_pending_restore_retry`: during a retry, submit a manual `set_brightness` and assert no restore write for 70 is issued afterwards and the baseline follows the manual value.
- `failed_dim_write_is_not_retried_as_a_restore`: with a protection reason active, a failed dim write keeps today's behavior (owned false, no retry).

Run them and record that the first three fail before the fix.

### 2. Keep the restore pending on failure in the core [x]

In `crates/telaviva-core/src/core.rs`:

- Add `restore_failures: u8` to `Monitor` (initialized to 0 in every constructor/discovery path) and a constant `MAX_RESTORE_FAILURES: u8 = 3`.
- In the failure branch of `hardware_result` for `read_brightness`: if the feature was owned before the failure, `reasons` is empty and `restore_failures < MAX_RESTORE_FAILURES`, keep `owned`, increment `restore_failures`, keep the error status, and do not set `manual_override`; otherwise keep the current behavior.
- In the failure branch for `set_brightness`: apply the same rule only when the action's value equals the baseline (a restore write), `manual_override` is false and `reasons` is empty; otherwise keep the current behavior. `set_volume` is untouched.
- Reset `restore_failures` to 0 on a successful `set_brightness`, a `display_state` change, a manual brightness set, and discovery (the Monitor constructor). Do NOT reset it on a successful read: a restore write that keeps failing while the readbacks succeed would otherwise loop without a bound.
- No change to `reconcile_all`: with `owned` kept, its existing branches re-enqueue the verification read and, after a matching readback, the restore write.

### 3. Documentation [x]

- `CHANGELOG.md`, Unreleased/Fixed: a bullet saying a dimmed monitor that wakes while DDC is not ready is now restored: a failed wake readback or restore write is retried up to three times while ownership is kept.
- `ARCHITECTURE.md`, in the wake reconciliation paragraph: one sentence stating the bounded retry and that a restore write still requires a matching readback.

### 4. Run Validation Commands [x]

## Validation Commands

- Before the fix (after task 1): `cargo test -p telaviva-core --locked --test core_contract -- failed_wake_readback_keeps_ownership_and_retries_the_restore failed_restore_write_is_retried_after_a_new_readback restore_retries_are_bounded` must fail.
- After the fix: `cargo test -p telaviva-core --locked` passes, including the unchanged `wake_read_failure_does_not_issue_a_protection_write`, `returning_sample_restores_owned_brightness_and_cadence_is_one_second`, `external_brightness_change_before_restore_is_preserved`, `disabling_protection_during_an_unacknowledged_dim_does_not_restore_blindly`, `shutdown_checks_an_owned_value_and_does_not_restore_pending_manual_write`.
- `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu` (CI-equivalent), `cargo clippy -p telaviva-core --all-targets --locked -- -D warnings` and `cargo fmt --all -- --check` if the project uses them in CI.
- Swift suites that use the core (`cargo build --release --target aarch64-apple-darwin -p telaviva-core && bash tools/tests/test-core-bridge.sh`) and the other `tools/tests/test-*.sh` scripts still pass.
- Hardware (not run by the plan): dim an LG ULTRAFINE, let it sleep, wake it, and confirm brightness returns; capture a snapshot (`brightness.owned`, `status`, `pending_actions`) if it does not.

## Notes

- Root cause is established from the code, not from a hardware trace. If the monitor still stays dim after this fix, collect a snapshot after wake (`display_state`, `brightness {owned, raw, observed, status, reasons}`, `screensaver.state`, `pending_actions`) before changing code again.
- Not fixed here: (a) re-discovery while dimmed stores the dimmed value as the new baseline (`contracts.md` forbids reusing a baseline after reconnect, so this needs a design decision); (b) `MacDDCController` reports `awake` without waiting for DDC readiness; waiting for a successful `readVCP(0x10)` with a bounded deadline would make the first verification read more likely to succeed and is a possible follow-up if three retries prove too few on hardware; (c) with the screensaver reason active the restore waits for the screensaver to stop, by design; (d) the zero-online-display discovery path in `MacDDCController` does not re-announce monitors when they return.
- The retry bound of three matches the DDC retry bound in `contracts.md`; each retry costs a DDC readback of about 1.7 s in the worst case on the native side.
- Presence protection is cleared on every display-state change, so a woken monitor returns to its baseline even if nobody is present until two new absent samples dim it again; this is existing behavior and is kept.

## Implementation Notes

- Done in one phase; the tests were written first and the four tests that reproduce the bug (`failed_wake_readback_keeps_ownership_and_retries_the_restore`, `failed_restore_write_is_retried_after_a_new_readback`, `restore_retries_are_bounded`, `manual_brightness_change_wins_over_a_pending_restore_retry`) failed before the core change; `failed_dim_write_is_not_retried_as_a_restore` passed before and after (it guards today's behavior).
- Plan change: the first version reset `restore_failures` on a successful readback; that left a read-ok/restore-write-fail loop without a bound, so the reset now happens only on a successful `set_brightness`, a display-state change, a manual brightness set and discovery.
- Test adjustment: the core snapshot lists only the first pending action per monitor, so `manual_brightness_change_wins_over_a_pending_restore_retry` drains the head `read_brightness` before looking for the manual write.
- Verified: `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu` (36 tests in core_contract), `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo fmt --all -- --check`, and the Swift suites core-bridge, hardware-key-router, manual-monitor-controls, ddc-transaction and camera-lifecycle.
- Not verified: behavior on the physical LG ULTRAFINE. The root cause comes from reading the code, not from a hardware trace; if the monitor still stays dim after wake, collect a snapshot as described in Notes.
- Review finding fixed after the first implementation: a failed readback or restore write during shutdown would have taken the retry branch (`hardware_result` is still accepted after `closed`), so the retry is now disabled once shutdown has started; covered by `shutdown_failed_readback_does_not_retry_the_restore`; a mutation check (removing `!closed`) makes that test fail, so it guards the rule.
