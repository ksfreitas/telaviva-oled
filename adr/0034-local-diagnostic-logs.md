# ADR-0034: Keep local diagnostic logs of brightness decisions and DDC attempts

**Status:** Accepted
**Date:** 2026-10-19
**Feature:** brightness-stays-at-zero-after-slow-monitor-wake
**Spec:** [Archived spec](../specs/archived/brightness-stays-at-zero-after-slow-monitor-wake/spec.md)

## Context

A field occurrence of the slow-wake bug in [ADR 0033](0033-wait-for-ddc-readiness-after-wake.md) could not be traced afterwards. Neither app kept a persistent record of brightness decisions. macOS wrote the relevant messages at debug level, and the system does not persist debug messages in the unified log. Windows wrote errors only, unless `TELAVIVA_LOG_LEVEL` was set before launch. Core decisions such as dropping ownership are not part of the snapshot, so nothing outside the core recorded them.

The same gap hides the two failure paths that ADR 0033 leaves open: a monitor rediscovered while dimmed, and a display sleep during a dim write. Telling the paths apart needs the order of events, not only the final state.

The README privacy statement says that device identifiers are not logged. Any log must keep that statement true.

## Decision

- **Always on, local and bounded.** The core keeps the last `MAX_DIAGNOSTIC_LINES` = 256 lines in a ring (`crates/telaviva-core/src/diagnostics.rs`). A full ring drops its oldest line and counts it, and the next drain starts with `diagnostics_dropped count=N`.
- **One additive C ABI function.** `tv_diagnostics_drain(TvCore *core, TvBuffer *out_buffer)` returns the pending lines as a UTF-8 JSON array of strings and empties the ring. It is additive to ABI version 1, which stays `1`, as the controls handle is. The shells drain after each snapshot refresh and once at shutdown.
- **Monitors appear only as hashed ids.** Each line names a monitor as `id-xxxxxxxx`, the FNV-1a 32-bit hash of its identifier. It is the hash that the Windows `shortId()` already uses (`native/windows/Platform/Diagnostics.cpp`). The test vectors are `"abc"` to `id-1a47e90b` and `""` to `id-811c9dc5`. Core and shell lines therefore correlate, and no serial number, EDID data, name or connection path is written. No camera data, presence image or setting other than brightness numbers is logged.
- **Core lines.** Each line is `event monitor=id-xxxxxxxx key=value ...`, and the sinks add timestamps. The events are `monitor_discovered`, `monitor_disconnected`, `display_state`, `presence_dim`, `presence_clear`, `dim_enqueued`, `restore_enqueued`, `read_result`, `baseline_adopted`, `ownership_verified`, `restore_failure` (with `failures=N/3`), `ownership_dropped` (with `reason`), `manual_set` and `follow_target`. Presence samples are not logged, only dim and clear transitions, and `follow_target` is logged only when its raw target changes.
- **Shell lines, with the same names on both platforms.** `app_start` (with the version), `display_sleep`, `display_wake`, `display_power` (sent to the controls handle), the display-change refreshes `screen_parameters_changed`, `resume_refresh` and `topology_refresh`, `discovery_added` and `discovery_removed` (hashed id, and the brightness read as raw/max or `read_failed`), `attempt_failed` (operation, VCP code, error and elapsed ms), `attempt_recovered` (the first success after failures), and `port_reopened` or `service_marked_stale`. Each attempt line is written where the primitive runs (`MacDDCController.runAttempt`, `DdcController::run`).
- **macOS sink.** `os.Logger` with subsystem `com.telaviva.oled` (the bundle identifier) and categories `brightness` (core lines) and `ddc` (shell lines), at `.notice`, so the unified log persists them. macOS manages retention and rotation.
- **Windows sink.** The existing `Diagnostics` file `%LOCALAPPDATA%\TelaVivaOLED\telaviva.log` rolls to `telaviva.log.1`, which keeps one previous generation. A `Notice` level sits between `Error` and `Info` and is the default. `TELAVIVA_LOG_LEVEL` set to `error`, `info` or `debug` changes it. `kMaxLogBytes` is 1 MiB, so at most about 2 MiB are kept.
- **Windows debug trace.** The reopen trace `ddc reopened port for <monitorId>` now writes `shortId(monitorId)` instead of the raw id. This also removes the EDID serial from that trace, which the raw id contained.

## Alternatives Considered

- **Windows Event Log.** Rejected: an event source must be registered under HKLM, which needs administrator rights that the per-user zip install does not have.
- **ETW.** Rejected: it records nothing without a running collector.
- **A macOS file under `~/Library/Logs`.** Rejected: nothing cleans it up automatically, so the app would need its own rotation code, which the unified log already provides.
- **Logging from the shells only, from snapshots.** Rejected: core decisions such as `ownership_dropped` are not in the snapshot, and two formatters, one in the core and one per shell, would drift apart.

## Consequences

- The statement "Device identifiers are not logged" stays true. The user guide and the README say so next to the hashed-id statement. A hashed id is derived from an identifier, so it is a correlation key and not anonymization: anyone with the logs and a list of candidate identifiers can match them against the 32-bit hashes. The logs stay on the device, and nothing sends them.
- The Windows debug trace no longer writes the raw monitor id.
- The bug plan estimates a few hundred lines a day. That is not measured. Within the limits, the 256-line core ring and the 2 MiB Windows file cap bound the storage, and macOS keeps the lines for the period that the system decides.
- The events separate the three paths of ADR 0033: `monitor_disconnected` and `monitor_discovered` around a wake (a rediscovered monitor), `restore_failure` with `failures=3/3` followed by `ownership_dropped` (the slow-wake bug), and `dim_enqueued` without a `read_result` before `display_state` asleep (a sleep during a dim write).
- Not verified on hardware: the drain, the `log show` output on a Mac and the Windows rotation at 1 MiB have not run on a physical machine. The hardware checks in the bug plan are pending.

## Non-Obvious Constraints

- A line reaches a sink only after a drain. A shell that stops draining loses older lines to the ring, and the `diagnostics_dropped` line shows how many were lost.
- No field may carry a serial number, name, connection path, camera data or presence image, not even in a debug trace. Identifier-bearing text must go through the hash.
- The event and field names are the contract for anyone reading the logs. Renaming one breaks the `log show` command and the grep patterns in the user guide and in the hardware checks of the bug plan.
- macOS writes these messages with public privacy. That is acceptable only because a line carries nothing but event names, enumerated reasons, the hashed id and numbers.
