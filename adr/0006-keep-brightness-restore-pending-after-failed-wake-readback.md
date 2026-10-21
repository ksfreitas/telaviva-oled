# ADR-0006: Keep the brightness restore pending after a failed wake readback or restore write

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** restore-brightness-on-monitor-wake
**Spec:** [Archived restore-brightness-on-monitor-wake spec](../specs/archived/restore-brightness-on-monitor-wake/spec.md)

## Context

A dimmed external monitor should return to its saved brightness when it wakes. In one observed run, a monitor that protection had dimmed to 0 % came back at 0 % after waking, and the app did not restore it; the brightness stayed low until the user changed it manually. No hardware trace of that run exists, so the cause below comes from reading the code and is not confirmed on hardware.

After a wake, `display_state` in `crates/telaviva-core/src/core.rs` clears the presence reason and sets a wake verification check, and `reconcile_all` enqueues one `read_brightness`. Before this change, a failed wake readback or a failed restore write set `owned` to false. With no reason active and `owned` false, `reconcile_all` had no restore target and enqueued nothing, so the restore was never attempted again. The baseline was still stored, but nothing could reach it. `shutdown_policy` restores only owned monitors, so it did not help either.

The native side makes such a failure likely. `MacDDCController.refreshDisplayStatesOnQueue` reports a display as `awake` from `CGDisplayIsAsleep` alone, without checking that DDC responds, and `DDCTransaction.readVCP` gives up after about 1.7 s of attempts. A monitor whose DDC channel is not ready right after power-on can fail its first readback, and the restore was lost at that point. This trigger is inferred, not confirmed on hardware.

## Decision

While no protection reason is active and the app still owns the dimmed value, a failed wake readback or a failed restore write keeps ownership, the baseline and the error status. The core then retries the restore through the existing `reconcile_all` branches, which read before any write. The retries are bounded by `MAX_RESTORE_FAILURES` = 3, counted in the internal `Monitor.restore_failures` field. Once the bound is reached, the previous behavior applies: ownership is dropped and the error status stays visible. The retry is not used once shutdown has started (`Core.closed`), so quitting never waits on retries; a failed shutdown readback drops ownership exactly as before.

The counter covers only two failure cases:

- A failed `read_brightness` while `owned` is true and no reason is stored (the wake verification read or the read before a pending restore; never once shutdown has started).
- A failed `set_brightness` that is a restore write: its value equals the baseline, `owned` is true, no reason is active and there is no manual override.

Dim writes, manual writes and volume writes keep their current failure behavior.

A restore write is still enqueued only after a readback that matches the value the app last set, as ADR 0001 requires. A failed readback therefore never leads directly to a write. `reconcile_all` is unchanged.

The counter resets to 0 on a successful `set_brightness`, a `display_state` change, a manual brightness set and monitor discovery. A successful readback does not reset it. The counter is not part of the snapshot, so the public interface does not change.

### Design correction: no reset on a successful readback

The first version reset the counter on a successful readback. That allowed an unbounded loop: each readback succeeds, the restore write fails, the counter returns to 0, and the cycle repeats. The reset is therefore limited to the events listed above, so the counter returns to zero only when a write succeeds or the display situation changes.

## Alternatives Considered

- Wait for DDC readiness before reporting `awake` on the native side: deferred. It would make the first verification read more likely to succeed, but it is untested and would block the DDC queue while the display is not ready. It is a possible follow-up if three retries prove too few on hardware.
- Unbounded retry: rejected, because a monitor that never answers would produce endless DDC traffic.
- Restore write without a preceding readback: rejected, because it breaks the read-before-write and ownership rules in ADR 0001 and ADR 0002.
- Reset the counter on a successful readback: rejected, for the reason given in the design correction above.

## Consequences

Three retries add up to about 5 s on the native side, at about 1.7 s per failed readback. Counting the first failure, four failed readbacks take up to about 7 s. A monitor whose DDC takes longer than that to answer after power-on can still stay dimmed. This is the main residual risk. It is not verified on the physical LG ULTRAFINE.

The change keeps the bounded-retry and read-before-write rules in `contracts.md`. One point needs review: the same contract says transient DDC failures get at most three total attempts, with 200 ms then 1 s backoff. Each core retry is a new DDC operation with its own three native attempts and backoff, so the core layer adds attempts on top of that limit. A strict reading of the contract would not allow them. This ADR does not change `contracts.md`.

Six regression tests in `crates/telaviva-core/tests/core_contract.rs` cover the change: `failed_wake_readback_keeps_ownership_and_retries_the_restore`, `failed_restore_write_is_retried_after_a_new_readback`, `restore_retries_are_bounded`, `manual_brightness_change_wins_over_a_pending_restore_retry`, `failed_dim_write_is_not_retried_as_a_restore` and `shutdown_failed_readback_does_not_retry_the_restore`. According to the spec's Implementation Notes, the first four failed before the core change, and the fifth passed both before and after it, since it guards the unchanged dim path. The sixth test was reworked after a mutation check showed that its first version also passed without the guard; it now fails when the `!closed` condition is removed from `hardware_result`. The same notes record the workspace tests, clippy, fmt and the Swift suites as passing. Those results were not re-run for this ADR.

[ADR 0033](0033-wait-for-ddc-readiness-after-wake.md) adds a 30 s wake readiness window inside the DDC retry, so the residual risk above applies only to a monitor whose DDC takes longer than that window after wake. The restore counter and the rules of this ADR are unchanged.

## Non-Obvious Constraints

- The snapshot lists only the first pending action per monitor. A test that looks for a later action must drain the head `read_brightness` first; `manual_brightness_change_wins_over_a_pending_restore_retry` does this.
- Presence protection is cleared on every display-state change, so a woken monitor returns to its baseline. It is dimmed again only after two new absent samples. This is existing behavior and is kept.
- An active screensaver reason blocks the restore by design. The restore waits until the screensaver stops.
- Retries are disabled once shutdown has started (`Core.closed`). A failed read or restore write queued by `shutdown_policy` drops ownership as before, so quitting never waits on retries. This is covered by `shutdown_failed_readback_does_not_retry_the_restore` in `crates/telaviva-core/tests/core_contract.rs`.
- A monitor that is re-discovered while dimmed captured the dimmed value as its new baseline. The [2026-10-21 amendment](#amendment-2026-10-21-restore-a-dim-after-reconnect) resolves the same-id, same-level case; the other cases keep this behavior.
- If the monitor still stays dim after this fix, collect a snapshot after wake before changing code again. Take `display_state`, `brightness` (`owned`, `raw`, `observed`, `status`, `reasons`), `screensaver.state` and `pending_actions`.

## Amendment 2026-10-21: restore a dim after reconnect

**Context.** A monitor that protection had dimmed could disconnect, or be discovered again in place, and return at the dimmed value. Discovery then took that value as the monitor's new baseline, so the dim was never undone and no later event restored it.

**Decision.** For each monitor id, the core parks the previous baseline (rounded) and the dimmed raw value when the monitor disconnects or is replaced while the app owns a value that differs from the baseline. When the same id is discovered again with stable identity and supported brightness, the parked entry is consumed:

- If the reading is within 0.5 raw units of the dimmed value, the parked baseline is restored through the normal ownership path, which reads first and then writes. The log line `dim_resumed monitor=<id> baseline=<n>` is written.
- If the monitor returns at another level, nothing is restored and that level becomes the baseline.
- An entry is never used for another id.
- An ambiguous or unsupported rediscovery leaves the entry parked.

**Scope.** The entries are runtime only and are never persisted. External monitors have no on-disk record, so a quit before the monitor returns loses the dim. Shutdown discards every entry. The core keeps at most 16 entries; when a new one would exceed that, the first id in key order is dropped, not the oldest. An entry lasts until a stable, supported discovery of its id, shutdown, or eviction. A resumed monitor starts with no presence reason, so if the user is still away it dims again after two absent checks.

**Supersedes.** For the same-id, same-level case only, this amendment supersedes two lines of the archived [contracts.md](../specs/archived/external-monitor-control-and-oled-presence-protection/contracts.md): line 33, "Never reuse a baseline after reconnect or application restart", and line 56, which asks to "read a fresh baseline" on reconnect. The restart half of line 33 still holds: no baseline is reused after an application restart.

**Status.** Hardware checks are pending for every monitor. This amendment does not claim a physical restore. The Windows DDC path uses the same core and applies once a Windows build includes it.
