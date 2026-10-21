# ADR-0032: Reopen DDC ports after a topology change instead of keeping the first transport

**Status:** Accepted
**Date:** 2026-10-18
**Feature:** ddc-stale-handle-after-monitor-connect
**Spec:** [Archived bug plan](../specs/archived/ddc-stale-handle-after-monitor-connect/spec.md)

## Context

On Windows, connecting a second monitor while TelaViva OLED was running left the first monitor's card at "Sem resposta" (en-US "No response"). Brightness and volume changes failed until the app was restarted, and the flyout refresh button did nothing.

`DdcController::refreshOnExecutor()` opened a DDC port only for catalog entries reported as added. `diffCatalogs` compares only the monitor id and its stable flag, so a monitor that stays connected is neither added nor removed, and its port was never touched again. The port forwards to a dxva2 physical-monitor handle from `GetPhysicalMonitorsFromHMONITOR`. Those handles are tied to the `HMONITOR`, which Windows changes when a display is added, so the old handle is inferred to fail on every call; the exact Win32 error was not captured. Every retry from the core ([ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md)) repeated the same primitive on the same dead handle, so the core's retry policy could not recover. Topology change, resume and the refresh button all call `DdcController::refresh()`, which was a no-op for known ids.

macOS has the same shape: `MacDDCController.discoverDisplays` keeps the cached IOAV service of a known display across refreshes, and `runAttempt` reported every failure as `io` with no recovery. That path was not reproduced.

## Decision

- **Reopen the transport silently; never report the monitor to the core again.** A new `monitor_discovered` for a known id makes the core bump the connection generation, drop pending actions, reset `last_presence` and take the current raw brightness as the baseline (unless the core holds a dim for that id at the same raw level; see the ADR 0006 amendment). That could store a presence-dimmed value as the baseline. The core already clears the "unavailable" status on the next successful hardware result, so the shells only have to supply a working transport. No core, ABI or snapshot change.
- **Windows, on refresh:** `DdcController::refresh()` reopens the port of every known external monitor that is still in the catalog, active and stable (`reopenKnownPorts`). Ambiguous and internal entries are skipped. A failed open keeps the old port, so a transient failure cannot turn a working monitor into an unsupported one.
- **Windows, on a failed attempt:** `DdcController::run()` reopens the port once and repeats the same primitive when it fails with anything other than `unsupported` (`reopenPort`, which re-enumerates the catalog to find the entry). `unsupported` is a real answer from the monitor and never reopens. There is no loop and no sleep; the core still owns retries and backoff. A trace line is written when a reopen happens and a trace sink is set.
- **macOS, on a failed attempt:** a failed write or read marks the display's cached IOAV service as stale and schedules one discovery pass, which replaces the service. Discovery otherwise keeps the cached service. No new `monitor_discovered` is sent for a known display. Marks of removed displays are dropped in `publishDisconnectedDisplays`.
- `run()` is no longer strictly "one primitive": a failure other than `unsupported` can cost a second primitive on a reopened port. The `DdcController.h` comments and the test `ddc_controller_run_failing_write_retries_once_on_reopened_port_without_sleep` record the new contract.

## Alternatives Considered

- **Report the monitor as newly discovered on every refresh.** Rejected: it resets the connection generation, pending actions and baseline in the core (see Decision), and the baseline risk is specific to OLED protection.
- **Compare `hmonitor` and reopen only when it changed.** Not adopted: it misses a handle that goes stale with the same `HMONITOR`, and it adds comparison state to every record. Reopening costs one `GetPhysicalMonitorsFromHMONITOR` call per monitor on topology, resume, manual refresh and startup.
- **Reopen only lazily, inside `run()`.** Not adopted alone: after a topology change the card would keep failing until the user acted, and the refresh button would stay useless.
- **Store a `CatalogEntry` in each `MonitorRecord` for reopening.** Not adopted: `run()` enumerates the catalog when it reopens, so the field would be unused.
- **Make the shell read the monitor after a reopen to clear the status at once.** Out of scope: it needs a core contract change.

## Consequences

- DDC control recovers by itself after another monitor is connected, on a refresh, and on the next attempt, without restarting the app. The first attempt after a topology change can cost one extra primitive plus a catalog enumeration, only when the first attempt fails.
- The "Sem resposta" status clears with the next successful hardware result from the core, not at the moment of the reopen.
- Six new Windows tests cover the retry, `unsupported`, a failed reopen, refresh without rediscovery, a kept old port and ambiguous or internal entries. One existing test changed its expected write count from one to two. The Windows suite passed with 625 tests, 0 failed, on 2026-10-18. `CHANGELOG.md` has an entry under Unreleased.
- The owner ran the manual validation before this was finalized and reported it as passed. No per-step record was kept, so this record does not show whether the macOS path was exercised. The macOS change was reviewed by reading the diff only: it was not compiled or run here, and IntelliJ reported no problems on any of the four code files, which does not prove them clean for C++ and Swift.

## Non-Obvious Constraints

- Two machines that share one monitor and both run TelaViva compete for the same DDC bus. That is a hardware limit. The recovery only lets each app heal after the other finishes; it does not remove the contention.
- A reopen can fail while Windows is still rebuilding the display topology (`GetPhysicalMonitorsFromHMONITOR` may return nothing for a moment). The old port is kept and the next failed attempt or refresh tries again.
- On macOS, a monitor that stays unresponsive makes each failed attempt schedule a discovery pass (one at a time). Only the core's backoff limits how often that happens.
- Reopening runs on the single DDC executor thread, because dxva2 is not thread-safe and `Dxva2PortProvider` keeps its map without a lock. Do not call `open()` from another thread.
