# Bug: DDC stops working after another monitor is connected, until the app restarts

## Bug Description

On Windows, with a monitor already controlled by TelaViva OLED (for example a DELL S3225QC), connecting a second monitor makes the first monitor's card show "Sem resposta" (en-US "No response"). Brightness and volume changes then fail with "Não foi possível aplicar; exibindo valor confirmado" (cannot apply; showing the confirmed value). The state does not clear: the refresh button in the flyout does nothing useful, and only quitting and restarting the app brings DDC back.

Expected: after the topology changes, the app reopens its DDC access to the monitors that are still connected, so controls work again without a restart. The reporter does not know whether macOS is also affected, so the plan covers both platforms, as AGENTS.md requires.

## Problem Statement

A monitor that stays connected across a topology change keeps the DDC transport it opened earlier, and that transport can become invalid. Nothing ever replaces it, because the only code that opens a transport runs when a monitor id is first discovered. On Windows the stored transport is a dxva2 physical-monitor handle; on macOS it is a cached IOAV service object.

## Solution Statement

Make the shells reopen the transport of a monitor that is still present, without reporting it to the core as a new monitor. A new `monitor_discovered` would bump the connection generation, drop pending actions, reset the baseline and could save a dimmed value as the baseline. The core already retries failed attempts and clears the "unavailable" status on the next successful result, so the shells only need to supply a working transport. Two triggers, both on the existing single DDC executor thread:

1. Windows `DdcController::refresh()` (topology change, resume, manual refresh and startup) reopens the port of every known, stable, external monitor that is still in the catalog. A failed reopen keeps the old port.
2. Windows `DdcController::run()` reopens the port once and retries the same primitive in the same attempt when it fails with `Io` or `Disconnected`. `Unsupported` is a real answer from the monitor and never triggers a reopen. This covers a handle that goes stale between topology events, and the case where the monitor's `HMONITOR` is not ready at the debounced topology event.
3. macOS marks the cached IOAV service as stale when an attempt fails and replaces it on the next discovery pass, which the failed attempt schedules once.

## Steps to Reproduce

1. Windows 11 with one external DDC monitor connected and TelaViva OLED running. Move the brightness slider and confirm it applies.
2. Connect a second monitor (any DDC monitor, or an additional display on another output) while TelaViva stays open.
3. Open the flyout and move the first monitor's brightness or volume slider. The card shows "Sem resposta" and the "Não foi possível aplicar" message.
4. Press the refresh button, or wait. The state does not recover. Quit and restart the app: DDC works again.

Related variant from the report: if two machines share one monitor and both run TelaViva, the second machine's DDC traffic can also invalidate the first machine's handle. The same recovery applies, but bus contention between two running apps is outside this fix.

## Root Cause Analysis

- `native/windows/Platform/DdcController.cpp` `refreshOnExecutor()` opens a port only for `diff.added` entries (line 167: `record.port = ports_.open(entry)`). `diffCatalogs` (`native/windows/Platform/DisplayCatalog.cpp:294`) compares only `identity.id` and the stable flag. A monitor that stays connected has the same id before and after the topology change, so it is neither added nor removed, and its `MonitorRecord.port` is never touched again.
- The port forwards to a `PhysicalMonitor` handle from `GetPhysicalMonitorsFromHMONITOR` (`native/windows/TelaViva/Host/Dxva2PortProvider.cpp:60`). Those handles are tied to the `HMONITOR`, and Windows changes `HMONITOR` values and display-driver state when a display is added. The old handle then fails on every call. `Dxva2DdcPort::mapLastError` maps these failures to `Io` (or `Disconnected`), which the core reports as `io` and the flyout shows as "Sem resposta" (`AppController.cpp:427`, `channelFailed`).
- Every later `DdcController::run()` reuses `found->second.port`, so each retry from the core hits the same dead handle (`DdcController.cpp:321`). The core's retry logic cannot help, because it only repeats the shell's primitive.
- `onTopologyChanged`, `onResume` and the flyout refresh button all call `ddc->refresh()` (`AppController.cpp:1107`, `1108`, `1413`). That function is a no-op for known ids, so manual refresh cannot recover either.
- macOS has the same shape: `MacDDCController.discoverDisplays` keeps `prior.service` when the monitor key is unchanged ("routine display refreshes must not reset ownership", lines 237-244), and `runAttempt` reports every failure as `io` with no recovery (lines 447-472). It is not reproduced on macOS and there is no macOS hardware in this plan, so the macOS change is limited and must be verified on hardware.

## Relevant Files

- `native/windows/Platform/DdcController.cpp`: add `reopenKnownPorts` to `refreshOnExecutor()` and a reopen-and-retry path in `run()`. Store the last `CatalogEntry` in `MonitorRecord` so a reopen has the entry.
- `native/windows/Platform/DdcController.h`: update the `refresh()` and `run()` comments to describe the reopen behavior.
- `native/windows/TelaViva/Host/Dxva2PortProvider.cpp` and `.h`: no logic change expected. `open()` already releases the previous handle for the id and tolerates a returned port that still holds its own reference. Re-check the comment on `open()` after the change.
- `native/windows/Tests/ddc_controller.cpp`: new regression tests and a stale-handle fake.
- `native/macos/TelaViva/MacDDCController.swift`: stale-service flag in `runAttempt`, replace-on-stale in `discoverDisplays`.
- `CHANGELOG.md`: bullet under Unreleased.
- Read-only references: `native/windows/Platform/DisplayCatalog.cpp`, `native/windows/Platform/Dxva2DdcPort.cpp`, `native/windows/Platform/SessionEvents.cpp`, `crates/telaviva-core/src/core.rs` (`discover`, why a silent reopen is preferred), `crates/telaviva-core/src/controls/dispatch.rs`.

### New Files

None.

## Step by Step Tasks

Implementation edits go to Haiku agents with an exact before/after briefing, per the repository rules. The orchestrator verifies, builds and runs the tests.

### 1. Windows: remember the catalog entry per monitor

In `DdcController.cpp`, add `CatalogEntry entry;` to `MonitorRecord`. In the `diff.added` loop, set `record.entry = entry` before `ports_.open(entry)`. Comments in English, matching the surrounding density.

### 2. Windows: reopen known ports on every refresh

In `refreshOnExecutor()`, after the `removedIds` loop and before or after the `diff.added` loop, iterate `current`. For each entry that is external (`!isInternalPanel`), `active`, has a stable identity and is not in `diff.added` (compare by `identity.id`), and whose record exists and is not ambiguous:

- call `ports_.open(entry)` with the lock released;
- if it returns a port, take the lock, re-check `stopped_` and that the record still exists, then replace `record.port` and `record.entry`;
- if it returns `nullptr`, keep the old port (a transient failure must not turn a working monitor into "unsupported").

Do not submit anything to the core and do not call the listener. Skip the reopen when `catalog_.enumerate()` did not list the monitor.

### 3. Windows: reopen once and retry inside `run()`

In `run()`, after the first primitive fails with `DdcError::Io` or `DdcError::Disconnected` (not `Unsupported`):

- enumerate the catalog (the executor thread is the one that may do it), find the entry with the same `identity.id`, and call `ports_.open(entry)`;
- when a port is returned, store it in the record under the lock if the record still exists and `stopped_` is false, then run the same primitive once on the new port and return that outcome;
- when the entry is missing or the open fails, return the original error unchanged.

At most one reopen per `run()` call. The core owns retries and backoff, so there is no loop and no sleep here. Add a trace line through `trace_` when a reopen happens, only if the sink is set, using the same "build the string only when a sink exists" rule as the rest of the file.

### 4. Windows: regression tests

In `native/windows/Tests/ddc_controller.cpp`:

- Extend the fakes: `PortState` gets `int openCount = 0;` and `int validFrom = 0;`. `FakePortProvider::open` increments `openCount` and hands the new `FakePort` its serial. `FakePort` fails with `DdcError::Io` for reads and writes when its serial is below `validFrom`. Add `FakePortProvider::invalidate(id)` that sets `validFrom = openCount + 1`. Existing tests keep their behavior because `validFrom` stays 0.
- `ddc_controller_run_reopens_port_after_stale_handle_and_retries_once`: discover a monitor, call `ports.invalidate(kIdA)`, run a brightness write, expect `ok`, expect `ports.opened` to contain the id twice, and expect the stale call and the new call in `callsFor`.
- `ddc_controller_run_does_not_reopen_on_unsupported`: make the channel unreadable, run a read, expect `unsupported` and a single open.
- `ddc_controller_run_returns_original_error_when_reopen_fails`: invalidate, set `withoutHandle` for the id, expect the original error and no crash.
- `ddc_controller_refresh_reopens_known_port_without_rediscovery`: discover, invalidate, call `refresh()` again with the same catalog, expect no second `monitor_discovered` in `submitter.sent` (count with `countOf`), expect a second open, then a write succeeds with no extra open.
- `ddc_controller_refresh_keeps_old_port_when_reopen_returns_null`: discover, then set `withoutHandle` and refresh, and expect the old port still serves a write.
- `ddc_controller_refresh_does_not_reopen_ambiguous_or_internal_entries`: expect no `open` for those ids on a second refresh.

### 5. macOS: replace a stale IOAV service

In `MacDDCController.swift`:

- add `private var staleServiceKeys = Set<String>()`, guarded by the DDC queue like `displays`;
- in `runAttempt`, when the primitive fails and the display is awake, insert `request.monitorID` and schedule one `discoverDisplays()` on the queue unless one is already pending (use a `Bool` flag cleared at the start of the pass);
- in `discoverDisplays`, where the code keeps `prior.service`, take `transport.service` instead when the key is in `staleServiceKeys` and `transport.service != nil`: release `prior.service`, then remove the key from the set. Keep the existing release of an unused new service in the other branch.
- remove the key from `staleServiceKeys` in `publishDisconnectedDisplays` cleanup so it cannot leak.

No new `monitor_discovered` is sent for a monitor that was already known, because the `previousTransport == nil` check at line 258 stays as is. State in the Notes that this path is not reproduced and is unverified on hardware.

### 6. Changelog

Add a bullet under `Unreleased` in `CHANGELOG.md`, in the file's existing style, for example: "Windows and macOS: DDC control recovers by itself after another monitor is connected or the display topology changes, without restarting the app." Do not touch version numbers.

### 7. Run the Validation Commands

The orchestrator runs every command below, reads the diff and checks the Haiku reports against the files.

### Implementation status

Tasks 1 to 6 are implemented. Deviations from the plan:

- Task 1: `MonitorRecord` did not get a `CatalogEntry` field, because `run()` enumerates the catalog when it reopens and the field would be unused.
- Task 3: the reopen trigger is "any failure other than `unsupported`" (checked on the outcome's error name), which is equivalent to `Io` or `Disconnected`.
- Task 4: the existing test `ddc_controller_run_failing_write_is_attempted_once_without_sleep` fixed the old "no retry" contract. It is now `ddc_controller_run_failing_write_retries_once_on_reopened_port_without_sleep` and expects two writes, one open at discovery and one reopen, and no sleep.
- Task 5: stale marks are cleaned with `formIntersection(present)` in `publishDisconnectedDisplays`.

Automated checks run: `scripts\build-windows.ps1 -Configuration Debug -NoZip` (625 tests, 0 failed) and `cargo test -p telaviva-core --test version_consistency` (3 passed). Not run: the macOS build and tests (no Mac here), and every hardware step of the Validation Commands, which are left to the owner.

## Validation Commands

Before the fix, to reproduce (hardware step, Windows):

1. Run the current build, connect a second monitor, and confirm the first card shows "Sem resposta" and stays so after pressing refresh.
2. As a code-level reproduction without hardware, add the `invalidate` fake and `ddc_controller_run_reopens_port_after_stale_handle_and_retries_once` first, and run the Windows tests to see it fail on the unfixed controller.

After the fix:

- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Configuration Debug -NoZip`: builds the core and shell and runs `tests.exe`. All new tests must pass, and the last output line must show no failures.
- `cargo test -p telaviva-core`: unchanged core, must stay green (guards the contract that a silent reopen needs no core change).
- `cargo test -p telaviva-core --test version_consistency`: no version drift.
- macOS, on a Mac: `scripts/build-macos.sh`, plus the Swift tests in `native/macos/Tests`.
- Hardware, Windows: start the app with one monitor, connect a second monitor, then move the first monitor's brightness and volume sliders without pressing refresh (lazy reopen), and again after pressing refresh. The card must return to the ready status and the value must apply. Disconnect the second monitor and repeat. Record the result in the Windows hardware checklist only when it was actually run.

## Notes

- The exact Win32 error returned by a stale handle was not captured. `mapLastError` sends everything except the two named codes to `Io`, so triggering the reopen on `Io` and `Disconnected` covers it. During the hardware step, enable the trace sink once to log the error that triggers the first reopen, and keep that finding for the validation notes.
- A silent reopen was chosen over reporting the monitor as newly discovered. `core.rs` `discover` bumps the connection generation, drops queued actions, resets `last_presence` and takes the current raw brightness as the new baseline, which could store a presence-dimmed value as the baseline.
- Reopening every known port on each refresh costs one `GetPhysicalMonitorsFromHMONITOR` call per monitor, only on topology, resume, manual refresh and startup, so the CPU and battery impact is negligible.
- The "Sem resposta" status clears with the next successful hardware result from the core. If the reopen succeeds but the user does nothing, the card keeps the old status until the next attempt. Making the shell trigger a read after a reopen would need a core contract change, which is out of scope.
- No new dependency and no new network use.
- Two PCs sharing one monitor and both running TelaViva compete for the same DDC bus. That is a hardware limitation. The recovery here only makes each app heal after the other finishes.
