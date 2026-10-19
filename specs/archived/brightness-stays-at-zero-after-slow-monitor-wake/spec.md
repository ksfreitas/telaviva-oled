# Bug: Brightness stays at zero after a slow external monitor wakes

## Bug Description

On macOS, with the lid closed, one external monitor and presence protection on, the external monitor sometimes comes back from display sleep at brightness 0. The slider shows 0, and nothing raises it again until the user moves it. In one occurrence, moving the slider first produced a DDC update error; the app then reopened the DDC port and the next attempt worked.

Expected: after wake, the core reads the panel, confirms that it still holds the dimmed value the app wrote, and restores the saved brightness (the baseline) as soon as the monitor answers DDC.

Actual: when the monitor answers DDC late after wake, the core gives up the restore after about 7 seconds and leaves the panel at the presence dim level (raw 0 on macOS). The core then no longer owns the brightness, so no later event restores it, and the camera stays suspended because the monitor is below the brightness activation threshold, so presence protection cannot recover it either.

The user has seen it only at work (lid closed, a monitor whose DDC commands are visibly slower). At home (lid open, Laptop follow on) it has not happened.

The Windows app shares the core and has the same failure paths (see Root Cause Analysis, Windows). Neither app keeps persistent diagnostic records of brightness decisions, so a field occurrence cannot be traced afterwards: macOS writes only debug-level unified log messages, which the system does not persist, and Windows writes errors only unless `TELAVIVA_LOG_LEVEL` is set before launch.

## Problem Statement

1. The post-wake restore has a retry budget counted in attempts, not in time, and it is shorter than the time some monitors (and the DDC port that macOS or Windows recreates after display sleep) take to answer DDC after wake. Once the budget runs out the restore is abandoned for good. The restore must survive a monitor that takes several seconds to answer DDC after wake, while still bounding DDC traffic for a monitor that never answers. This applies to macOS and Windows.
2. There is no always-on, bounded, privacy-safe diagnostic log of what the core decided (ownership, baseline, restore, failures) and what the shell saw (sleep, wake, discovery, DDC attempt results). Without it, the remaining failure paths below cannot be told apart in the field.

## Solution Statement

### Part 1: wait for DDC readiness after wake (macOS and Windows)

Implement the follow-up that ADR 0006 deferred, in the shared core controls dispatcher:

- `display_power(on)` opens a wake readiness window of `WAKE_READY_WINDOW_SECS` = 30 s, in addition to the existing 2 s `WAKE_GRACE_SECS` hold. `display_power(off)` and `shutdown` close it.
- An operation that starts while the window is open records the window's end. While `now` is before that end, a transient failure (`io`, or a successful read without a value) does not consume the per-primitive budget of `ATTEMPTS_PER_PRIMITIVE` = 3. The primitive is retried every `WAKE_READY_RETRY_SECS` = 2 s instead. Permanent errors (`unsupported`, `disconnected`) and a set readback mismatch keep their current behavior.
- Once the window has ended, the normal budget (3 attempts, 0.2 s then 1.0 s backoff) applies to the attempts that remain, so the operation finishes as it does today.

The core therefore receives one `hardware_result` per wake read: a success as soon as the monitor answers, or a failure only after the window. `MAX_RESTORE_FAILURES` and the read-before-write ownership rules in `core.rs` stay unchanged. Both shells already send `display_power` to the controls handle (macOS from `NSWorkspace.screensDidSleep/screensDidWake` in `MacDDCController.swift`; Windows from `GUID_SESSION_DISPLAY_STATUS` through `SessionEvents` and `AppController.cpp`), so the fix reaches both platforms with no shell logic change. Each failed retry still reopens the DDC port (macOS `markServiceStale`, Windows `DdcController::run` reopen), so a stale port is replaced while the window is open instead of on the user's next slider move.

DDC traffic stays bounded: at most about 15 extra attempts per waiting operation, only during the 30 s after a wake, and only while the monitor does not answer.

### Part 2: always-on diagnostic log with rotation (macOS and Windows)

The complexity is moderate. Windows already has a bounded, rotating file logger; macOS has the native unified log, whose retention and rotation the system manages. The work is a shared source of core decision records plus two small sinks:

- Core: a bounded in-memory ring of diagnostic lines (`MAX_DIAGNOSTIC_LINES` = 256; the oldest line is dropped and a `diagnostics_dropped count=N` line is emitted on the next drain), filled at the decision points in `core.rs`, and drained through a new additive C ABI function `int32_t tv_diagnostics_drain(TvCore *core, TvBuffer *out_buffer)` that returns a JSON array of strings and empties the ring. `TV_CORE_ABI_VERSION` stays 1, as for the controls handle. Lines are plain `event key=value ...` text; the sinks add timestamps.
- Monitor identity in every line is `id-xxxxxxxx`, the FNV-1a 32-bit hash already used by the Windows `shortId()` (`native/windows/Platform/Diagnostics.cpp`), so core and shell lines correlate without writing serial numbers, EDID data, names or connection paths. No camera data, presence images or settings values other than brightness numbers are logged. This keeps the README privacy statement ("Device identifiers are not logged") true.
- macOS sink: `os.Logger` with subsystem `com.telaviva.oled` (the bundle identifier) and categories `brightness` (core lines) and `ddc` (shell lines), at `.notice` level so the unified log persists them; retention and rotation are managed by macOS. Collect with `log show --last 1d --predicate 'subsystem == "com.telaviva.oled"'` or Console.app.
- Windows sink: the existing `Diagnostics` file `%LOCALAPPDATA%\TelaVivaOLED\telaviva.log`, which rolls to `telaviva.log.1` (one previous generation). Add a `Notice` level between `Error` and `Info`, make `Notice` the default (`TELAVIVA_LOG_LEVEL` still raises it to `info` or `debug`), and raise `kMaxLogBytes` from 256 KiB to 1 MiB, so at most about 2 MiB are kept. Windows has no native automatic cleanup for files under `%LOCALAPPDATA%`; the Event Log needs an event source registered under HKLM (administrator rights, which the per-user zip install does not have), and ETW records nothing without a running collector, so the self-rotating file is the right mechanism. `Uninstall-TelaVivaOLED.ps1 -RemoveUserData` already removes it.
- Shell lines (both platforms, same event names): app start (version), display sleep/wake notification, `display_power` sent, Windows resume and topology refresh, monitor added or removed by discovery (hashed id, brightness read raw/max or `read_failed`), each failed DDC attempt (op, VCP code, error, elapsed ms) and the first success after failures, DDC port or IOAV service reopen. Each attempt line is logged by the shell where the primitive runs (`MacDDCController.runAttempt`, `DdcController::run`).

Core lines (names fixed so both platforms log identical text): `monitor_discovered`, `monitor_disconnected`, `display_state`, `presence_dim`, `presence_clear`, `dim_enqueued`, `restore_enqueued`, `read_result`, `baseline_adopted`, `ownership_verified`, `restore_failure` (with `failures=N/3`), `ownership_dropped` (with the reason), `manual_set`, `follow_target`. Each carries the hashed monitor id and the relevant numbers (`raw`, `observed`, `baseline`, `target`, `owned`). Presence samples themselves are not logged, only dim and clear transitions, and `follow_target` is logged only when the raw target changes, so the volume stays at a few hundred lines a day.

## Steps to Reproduce

On hardware (macOS, Apple Silicon):

1. Close the lid with one external monitor whose DDC answers slowly after wake (or behind a dock). Enable presence protection for that monitor. Laptop follow is irrelevant because the built-in display is unavailable with the lid closed.
2. Set the monitor to 50 %. Step away until presence protection dims it to 0.
3. Let macOS put the display to sleep (or run `pmset displaysleepnow`).
4. Wake the display. If the monitor does not answer DDC within about 7 s after `screensDidWake`, it stays at 0 and the slider shows 0.
5. Move the slider: the first write may fail with an `io` error on the stale IOAV port; the port is reopened and the next attempt works.

On Windows 11: the same steps with the display turned off by the power plan timeout (session display status off), then woken. The same 7 s budget applies.

Deterministic reproduction without hardware (the regression tests in this plan): feed the controls handle `display_power` on, let the 2 s grace pass, and fail every brightness read with `io` for 10 s of injected clock. The handle reports the read as failed after about 1.2 s; the core runs out of `MAX_RESTORE_FAILURES` after 4 such reads and drops ownership, so no `set_brightness` back to the baseline is ever enqueued.

## Root Cause Analysis

Sequence, verified in the code at HEAD 22bb8fb:

1. Presence absence dims the monitor to `min(baseline, brightness.min)` (`crates/telaviva-core/src/core.rs`, `reconcile_all`). On macOS `featureJSON` reports `min: 0` (`native/macos/TelaViva/MacDDCController.swift`), so the panel goes to raw 0. The successful dim sets `owned = true`, `raw = 0` and `observed = 0`; `baseline` keeps the user's level.
2. Display sleep: `screensDidSleep` sends `display_power(off)` and reports every monitor `asleep`. `display_state(asleep)` clears `presence_dimmed`, resets `restore_failures` and drops the monitor's actions. `owned`, `raw` and `baseline` are kept.
3. Wake: `screensDidWake` sends `display_power(on)`, which holds DDC for `WAKE_GRACE_SECS` = 2 s (`crates/telaviva-core/src/controls/retry.rs`, `dispatch.rs`), then rediscovers displays and reports `awake`. `display_state(awake)` sets `wake_ownership_check`; `reconcile_all` has no reason and `owned` is true, so the target is the baseline and, because `observed` (0) differs, it enqueues `read_brightness`.
4. Each core read is one controls `Operation` with 3 attempts and 0.2 s then 1.0 s backoff (`retry.rs`, `on_failure`). On macOS, `runAttempt` fails with `io` while the monitor or its recreated IOAV service does not answer, and marks the service stale. A failed read costs about 1.2 s plus I/O time.
5. `hardware_result` keeps ownership for a failed read only while `restore_failures < MAX_RESTORE_FAILURES` = 3 (`core.rs`). The fourth failed read, about 2 s + 4 x 1.3 s, roughly 7 s after wake, sets `owned = false`.
6. With no reason and `owned == false`, `reconcile_all` has no target, so nothing is enqueued again. The panel stays at 0, `observed` stays 0, the slider shows 0, and the camera is suspended as `suspended_low_brightness`, so presence protection cannot cycle the monitor back either. Only a manual change sets a new baseline.
7. `markServiceStale` schedules one rediscovery per failure, and that pass replaces the IOAV service only if the new lookup already returns one. If the last pass ran before macOS recreated the service, the stale port stays cached until the next failure, which is the user's first slider move: it fails with `io`, the port is replaced, and the retry works. This matches the reported error followed by a reconnect.

ADR 0006 named this exact residual risk ("A monitor whose DDC takes longer than that to answer after power-on can still stay dimmed") and deferred the readiness wait. The budget is counted in attempts instead of time, so a slower monitor or a slower port reopen loses the restore.

Why only at work: at home Laptop follow is on with the lid open. `follow_target` gives the monitor a target that does not depend on `owned`, so `reconcile_all` rewrites it after the failed reads, and every built-in brightness change triggers it again. With the lid closed, `BuiltInDisplayBackend` reports the built-in display offline, `builtin_display` sets `builtin_available = false`, and `follow_target` returns `None` for every monitor. Nothing masks the lost restore there, and the work monitor answers DDC more slowly.

### Sleep while protected: scenarios run against the core

Three scenarios were run against the core with a temporary test (setup: monitor at raw 70, presence protection on, two absent samples enqueue the dim to 0). The existing contract test `failed_wake_readback_keeps_ownership_and_retries_the_restore` covers scenario A and passes.

| Scenario | Core state after wake | Result |
| --- | --- | --- |
| A. Dim acknowledged at 0, display asleep, display awake, wake read returns 0 | `read_brightness` matches `raw`, then `set_brightness 70`; final `raw = 70`, `owned = true` | Restored. 0 is not kept. |
| B. Dim acknowledged at 0, then `monitor_disconnected` and `monitor_discovered` with a reading of 0 (the monitor left the online display list during sleep) | `baseline = 0`, `owned = false`, no pending action, also after `awake` | Stays at 0 permanently; the saved level is lost. |
| C. Display asleep while the dim write is in flight; its late result arrives | Core drops the action and ignores the result; `raw = 70`, `owned = false`, `status = pending`, nothing queued after `awake` | Panel at 0 if the write landed, core believes 70; nothing corrects it. |

Scenario A is the designed behavior: presence protection is cleared on every display-state change, the saved level is restored, and the camera dims again after two new absent samples (ADR 0006). Scenario B produces the same visible symptom as this bug and depends on whether the monitor leaves the online display list during sleep, which cannot be determined from the code; ADR 0006 records it as needing a design decision because `contracts.md` forbids reusing a baseline after reconnect. Scenario C needs display sleep to start during the sub-second dim write. Both are left to the owner's decision (see Notes); the Part 2 log shows which of the three paths happened in the field (`monitor_disconnected`/`monitor_discovered` around a wake for B, `restore_failure` and `ownership_dropped` for this bug).

### Windows

The Windows shell sends the same core and controls events and reaches the same paths:

- Display power: `SessionEvents` registers `GUID_SESSION_DISPLAY_STATUS` (`native/windows/Platform/SessionEvents.cpp`); `AppController.cpp` `events.onDisplayState` calls `ddc->onDisplayState(displayOn)` (every external monitor reported `asleep` or `awake` together) and `s.controls->displayPower(displayOn)`. The same 2 s grace, 3-attempt budget and `MAX_RESTORE_FAILURES` apply, so this bug (path 1) is reachable on Windows, and Part 1 fixes it there with no shell change.
- `DdcController::run` reopens the physical monitor handle once after any non-`unsupported` failure (ADR 0032), but adds no wake delay; the reopen fails while the monitor is absent from the catalog, so a slow monitor still exhausts the budget.
- Scenario B is reachable and may be more likely than on macOS: `PBT_APMRESUMEAUTOMATIC` triggers an immediate, non-debounced `ddc->refresh()`; a transient `QueryDisplayConfig` failure or `targetAvailable == FALSE` empties the catalog (`DisplayCatalog.cpp`), so every monitor gets `monitor_disconnected` and then `monitor_discovered` with a fresh read. The Windows identity key is EDID-only (ADR 0018), so the id is the same after rediscovery, but the core still starts a new record.
- Scenario C is reachable: Windows does not gate DDC on display state in the shell; an attempt already on the single DDC executor still runs after display off.
- There is no equivalent of the macOS `incompleteBrightnessAtDiscovery` resubmission on Windows.
- The Windows debug trace `ddc reopened port for <monitorId>` (`DdcController.cpp`) writes the raw monitor id, which contains the EDID serial, against `Diagnostics.h` and the README privacy statement. Part 2 replaces it with `shortId()`.

### Hypotheses checked and ruled out

- Laptop follow running with the lid closed: it does not run. `BuiltInDisplayBackend.status` reports the display offline when the lid is closed, `BuiltInBrightnessMonitor` stops polling, and `follow_target` requires `builtin_available`. A built-in read failure is reported as `nil`, never as 0. The follow toggle is global but curves are per monitor id, and the work monitor's id differs from the home monitor's.
- Brightness command queue: the core `enqueue` replaces the last pending command of the same kind for a monitor in place, and the manual slot coalesces slider and key targets. Only the in-flight head is not replaced; a newer command queues behind it and runs after it, so no stale pending brightness command runs after a newer one. Manual, protection and follow commands share the core queue.
- A DDC read failure becoming 0: no path converts a failed read into 0. `DDCTransaction.parse` rejects `maximum == 0`, `featureJSON` reports a failed read as unsupported with no value, and the core does not change `observed`, `raw` or `baseline` on a failed read. The 0 is the real dim level the app wrote before sleep.

## Relevant Files

Part 1:

- `crates/telaviva-core/src/controls/retry.rs`: `ATTEMPTS_PER_PRIMITIVE`, `BACKOFF_SECS`, `WAKE_GRACE_SECS` and `Operation::on_failure`; the readiness window constants and the in-window retry rule go here.
- `crates/telaviva-core/src/controls/dispatch.rs`: `display_power` arms the wake grace; `start` creates each `Operation`; `shutdown` clears timers. It must open, close and pass the readiness window.
- `crates/telaviva-core/src/controls/retry/tests.rs`, `crates/telaviva-core/src/controls/dispatch/tests.rs`, `crates/telaviva-core/tests/controls_contract.rs`: regression tests.
- `crates/telaviva-core/src/core.rs`: `display_state`, `reconcile_all`, `hardware_result` and `MAX_RESTORE_FAILURES` define the ownership drop that Part 1 avoids; their logic is not changed by Part 1.
- `native/macos/TelaViva/MacDDCController.swift`, `native/macos/TelaViva/ControlsDriver.swift`, `native/windows/TelaViva/Host/AppController.cpp`, `native/windows/Platform/SessionEvents.cpp`, `native/windows/Platform/ControlsDriver.cpp`: send `display_power`; read only for Part 1.

Part 2:

- `crates/telaviva-core/src/core.rs`: decision points that push diagnostic lines (`discover`, `disconnect`, `display_state`, `presence`, `reconcile_all`, `hardware_result`, `manual_set`, `recompute_follow`).
- `crates/telaviva-core/src/ffi.rs`, `include/telaviva_core.h`: the new `tv_diagnostics_drain` export.
- `crates/telaviva-core/tests/header_contract.rs`, `crates/telaviva-core/tests/ffi_contract.rs`, `crates/telaviva-core/tests/core_contract.rs`: header, ABI and content tests.
- `tools/windows/check-dll-exports.ps1`: checks DLL exports against the header; it must see the new function.
- `native/macos/TelaViva/CoreBridge.swift`: drains after each snapshot refresh (`refreshOnce`) and on shutdown, logs to category `brightness`.
- `native/macos/TelaViva/MacDDCController.swift`, `native/macos/TelaViva/PresenceCamera.swift`: the `wakeLog` fallback subsystem `br.com.codebit.telaviva` is stale; the bundle identifier is `com.telaviva.oled` (`Info.plist`). `MacDDCController` gets the `ddc` category lines.
- `native/windows/Platform/Diagnostics.h`, `native/windows/Platform/Diagnostics.cpp`: `Notice` level, default level and `kMaxLogBytes`.
- `native/windows/Platform/CoreBridge.cpp`: drains in `runWorker` after each snapshot and on shutdown.
- `native/windows/Platform/DdcController.cpp`: shell DDC lines and the `shortId()` fix for the reopen trace.
- `native/windows/TelaViva/Host/AppController.cpp`: `requestedLogLevel` mapping, `onDisplayState`, `onResume`, `onTopologyChanged` lines.
- `native/windows/Tests/` (`ddc_controller.cpp`, a diagnostics test file if none exists) and `native/macos/Tests/` with `tools/tests/test-core-bridge.sh`: shell tests.
- `README.md` (Privacy), `docs/user-guide.md` (new "Diagnostic logs" section; Privacy), `api.md` (Native shell core ABI), `ARCHITECTURE.md`, `CHANGELOG.md`.

### New Files

- `adr/0033-wait-for-ddc-readiness-after-wake.md`: the wake readiness window, its constants, its bounds, and how it relates to ADR 0006 and ADR 0029.
- `adr/0034-local-diagnostic-logs.md`: always-on local diagnostic logs, the core ring and `tv_diagnostics_drain`, the hashed monitor id, the macOS unified log and Windows rotating file sinks, retention, and why the Windows Event Log and ETW were not chosen.
- `crates/telaviva-core/src/diagnostics.rs`: the bounded ring, `short_id` (FNV-1a 32-bit, `id-%08x`, identical to the Windows `shortId`) and the line builder, with unit tests.
- `native/macos/TelaViva/DiagnosticLog.swift`: the two `Logger` instances and a Swift `shortId` port, with a test in `native/macos/Tests/` that uses the same test vector as the Windows test.

## Step by Step Tasks

### 1. Write the failing regression tests first (Part 1)

- In `crates/telaviva-core/src/controls/retry/tests.rs`, add:
  - `read_keeps_retrying_inside_the_wake_ready_window`: an `Operation` for `OpKind::Read { code: 0x10 }` created with a readiness end of `t0 + 30.0`; fail the attempt with `AttemptError::Io` five times, advancing time by the returned `Step::Wait` each time. Every failure inside the window must return `Step::Wait(now + WAKE_READY_RETRY_SECS)` followed by another `Run`; then a successful read with a value finishes `ok: true` with that value.
  - `read_falls_back_to_the_normal_budget_after_the_window`: the same operation keeps failing past `t0 + 30.0`; after the window it gets at most the normal remaining attempts with 0.2 s and 1.0 s backoff and finishes `ok: false, error: "io"`.
  - `permanent_errors_end_at_once_inside_the_window`: `Unsupported` and `Disconnected` inside the window finish at once, as today.
  - `readback_mismatch_stays_final_inside_the_window`: a `SetVerify` whose readback differs still finishes `ok: false` without a retry.
  - `operation_without_window_keeps_three_attempts`: an operation with no readiness end behaves exactly as today.
- In `crates/telaviva-core/src/controls/dispatch/tests.rs`, add:
  - `wake_window_keeps_a_failing_read_running_until_the_monitor_answers`: `display_power(on)` at t = 0, a pending `read_brightness` head, grace passes at 2 s, attempts fail with `io` until t = 12 s, then succeed. The dispatcher submits exactly one result, `ok: true` with the value, and `next_deadline` reports each 2 s wait.
  - `display_power_off_closes_the_wake_window`, `operation_started_after_the_window_uses_the_normal_budget`, `shutdown_clears_the_wake_window`.
- In `crates/telaviva-core/tests/controls_contract.rs`, add `slow_monitor_wake_read_is_reported_once_after_it_answers`: through the JSON contract, send `display_power` on, observe a core snapshot whose head is a `read_brightness`, answer `run_attempt` effects with `io` for 10 s of injected clock, then with a value, and assert one `submit_result` with `ok: true`.
- Run `cargo test -p telaviva-core` and confirm the new tests fail before the fix and every existing test passes, including `failed_wake_readback_keeps_ownership_and_retries_the_restore` and `restore_retries_are_bounded`.

### 2. Add the readiness window to the retry policy

In `crates/telaviva-core/src/controls/retry.rs`:

- Add `pub(crate) const WAKE_READY_WINDOW_SECS: f64 = 30.0;` ("Time after wake during which a transient DDC failure does not consume the attempt budget, because some monitors and docks answer DDC only several seconds after wake.") and `pub(crate) const WAKE_READY_RETRY_SECS: f64 = 2.0;` ("Delay between attempts inside the wake readiness window.").
- Add `ready_until: Option<f64>` to `Operation` and `with_ready_until(self, until: Option<f64>) -> Self`; `Operation::new` keeps its signature.
- In `on_failure`, before the budget check: if the error is not permanent and `self.ready_until.is_some_and(|until| now < until)`, return `self.wait(primitive, attempts.min(ATTEMPTS_PER_PRIMITIVE - 1), now + WAKE_READY_RETRY_SECS)`, so in-window failures never exhaust the budget and the first failure after the window still has at least one normal attempt left. Keep `attempts >= 1` so the `BACKOFF_SECS` index stays valid.
- Do not change `on_success`, the readback mismatch rule, `SETTLE_SECS`, `BACKOFF_SECS`, `WAKE_GRACE_SECS` or `RESULT_RESUBMITS`.

### 3. Open, close and pass the window in the dispatcher

In `crates/telaviva-core/src/controls/dispatch.rs`:

- Add `wake_ready_until: Option<f64>` next to `grace_deadline`, initialized to `None`.
- In `display_power`: on `on`, set it to `Some(now + WAKE_READY_WINDOW_SECS)`; on `off`, set it to `None`. Update the doc comment. In `shutdown`, set it to `None`.
- Where `start` creates the `Operation`, pass `self.wake_ready_until.filter(|until| now < *until)` through `with_ready_until`.
- Do not change `may_run`, `take_ready`, `tick` or `next_deadline`.
- Run the Part 1 tests; they must pass now.

### 4. Core diagnostic ring and ABI (Part 2)

- Write tests first in `crates/telaviva-core/src/diagnostics.rs` (ring bound, drop counter line, `short_id("abc")` equals the Windows `shortId("abc")` value, computed once and shared as the test vector) and in `crates/telaviva-core/tests/core_contract.rs`: `wake_restore_records_diagnostic_lines` (scenario A yields `display_state`, `read_result`, `ownership_verified`, `restore_enqueued` in order, with `id-` ids and no raw monitor id), `exhausted_restore_records_ownership_dropped`, `rediscovery_records_disconnect_and_discovery`, `diagnostic_lines_never_contain_the_monitor_id_name_or_path`.
- Create `crates/telaviva-core/src/diagnostics.rs` with `MAX_DIAGNOSTIC_LINES = 256`, the ring, `short_id`, and a `push(event, monitor_id, fields)` helper; register it in `lib.rs`.
- Add a `diagnostics` field to `Core` and push lines at the decision points listed in the Solution Statement. Do not change any decision logic.
- Add `pub unsafe extern "C" fn tv_diagnostics_drain(handle: *mut TvCore, out: *mut TvBuffer) -> i32` in `ffi.rs`, following `tv_snapshot` for locking, null checks, return codes and buffer ownership (freed with `tv_buffer_free`). Declare it in `include/telaviva_core.h` with a comment, and add it to `header_contract.rs` and an `ffi_contract.rs` test (null handle, empty ring returns `[]`, drain empties the ring).

### 5. macOS sink

- Create `native/macos/TelaViva/DiagnosticLog.swift` with `Logger(subsystem: "com.telaviva.oled", category: "brightness")`, `Logger(subsystem: "com.telaviva.oled", category: "ddc")` and `shortId(_:)`; log with `.notice` and `privacy: .public` (lines contain only hashed ids and numbers). Add the file to the Xcode project.
- In `CoreBridge.swift`, after each `tv_snapshot` read in `refreshOnce` and once before `tv_shutdown` returns, call `tv_diagnostics_drain`, decode the string array and log each line to `brightness`.
- In `MacDDCController.swift`, log the shell lines listed in the Solution Statement to `ddc` (sleep, wake, `display_power`, discovery add or remove with the brightness read, `runAttempt` failure with op, code, error and elapsed ms, first success after failures, `markServiceStale`). Use `shortId(key)` for every monitor reference.
- Replace the stale `br.com.codebit.telaviva` fallback in `MacDDCController.swift` and `PresenceCamera.swift` with `com.telaviva.oled`.
- Add a Swift test for `shortId` with the shared vector, and extend `tools/tests/test-core-bridge.sh` to check that a drained line reaches the logger callback (inject the logger).

### 6. Windows sink

- In `Diagnostics.h/.cpp`: add `LogLevel::Notice` between `Error` and `Info`, make `Notice` the constructor default, and set `kMaxLogBytes` to `1024 * 1024`. In `AppController.cpp`, keep `TELAVIVA_LOG_LEVEL` `info` and `debug`, and add `error` to lower it.
- In `CoreBridge.cpp` `runWorker`, after each snapshot fetch and once in `shutdown` before `tv_shutdown`, call `tv_diagnostics_drain` and log each line at `Notice`. The worker already owns the core call path; keep the drain on it.
- In `DdcController.cpp` and `AppController.cpp`, log the same shell lines as macOS at `Notice`, with `shortId(monitorId)`; change the reopen trace to `shortId(monitorId)`.
- Add tests in `native/windows/Tests/`: `shortId` with the shared vector, the `Notice` default and rotation at 1 MiB (write past the cap into a temp folder and check `telaviva.log.1`), and in `ddc_controller.cpp` that no logged line contains the raw monitor id.

### 7. Record the decisions and update the docs

- Create `adr/0033-wait-for-ddc-readiness-after-wake.md` and `adr/0034-local-diagnostic-logs.md` in the format of the existing ADRs (Status Accepted, date, Context, Decision, Alternatives Considered, Consequences, Non-Obvious Constraints). ADR 0033 alternatives: raise `MAX_RESTORE_FAILURES` (rejected: unpaced and attempt-counted), a native readiness probe per shell (rejected: duplicated per platform), unbounded retry (rejected). ADR 0034 alternatives: Windows Event Log (rejected: needs an HKLM event source and administrator rights), ETW (rejected: needs a running collector), a macOS file under `~/Library/Logs` (rejected: no automatic cleanup and its own rotation code), shell-only logging from snapshots (rejected: core decisions such as `ownership_dropped` are not in the snapshot, and two formatters would drift).
- Add one line to `adr/0006-...md` Consequences and to `adr/0029-...md` after the retry budget bullet pointing to ADR 0033; do not rewrite them.
- `ARCHITECTURE.md`: one sentence each in the restore retry paragraph and the controls handle section for the window; a short diagnostics paragraph replacing the `wake` category sentence.
- `api.md`: document `tv_diagnostics_drain` under "Native shell core ABI".
- `docs/user-guide.md`: a "Diagnostic logs" section with where the logs are and how to collect them on each platform (`log show --last 1d --predicate 'subsystem == "com.telaviva.oled"'`; `%LOCALAPPDATA%\TelaVivaOLED\telaviva.log` and `.1`), and update Privacy: monitors appear only as short hashed ids. Same wording in `README.md` Privacy.
- `CHANGELOG.md` under `## Unreleased`: in `### Fixed`, "An external monitor dimmed by protection no longer stays at its dimmed brightness when its DDC answers slowly after wake: DDC attempts are retried for up to 30 s after wake before the restore is given up, on macOS and Windows. Hardware checks are pending."; in `### Added`, "Always-on local diagnostic logs of brightness decisions and DDC attempts, with monitors as hashed ids: the macOS unified log (subsystem `com.telaviva.oled`) and a rotating `telaviva.log` of at most about 2 MiB on Windows. Core ABI: `tv_diagnostics_drain`, additive to ABI version 1."; in `### Fixed`, "The Windows debug trace no longer writes the monitor id, which contained the EDID serial."

### 8. Run the Validation Commands

Run every command below and confirm zero failures. The Windows commands run on Windows or in Windows CI.

## Validation Commands

```sh
# Before the Part 1 fix (task 1 only): the new tests fail, all existing tests pass.
cargo test -p telaviva-core --lib controls::retry
cargo test -p telaviva-core --lib controls::dispatch
cargo test -p telaviva-core --test controls_contract

# After all tasks: Rust gates used by CI.
cargo +stable fmt --all -- --check
cargo +stable test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
cargo +stable clippy --workspace --all-targets --locked -- -D warnings
cargo test -p telaviva-core --test core_contract
cargo test -p telaviva-core --test header_contract
cargo test -p telaviva-core --test ffi_contract
cargo test -p telaviva-core --test version_consistency

# macOS shell.
cargo +stable build --workspace --release --target aarch64-apple-darwin --locked
tools/tests/test-core-bridge.sh
tools/tests/test-controls-bridge.sh
tools/tests/test-ddc-transaction.sh
scripts/build-macos.sh
```

```powershell
# Windows (Windows 11 x64 or Windows CI), same steps as .github/workflows/windows.yml.
cargo test --workspace --all-targets --locked
powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\build-rust-dlls.ps1 -Configuration Release
powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\check-dll-exports.ps1
msbuild native\windows\TelaViva.sln /restore /p:Platform=x64 /p:Configuration=Release /m
native\windows\build\x64\Release\tests.exe
```

Hardware checks after the fix:

- macOS, lid closed, the work monitor: repeat Steps to Reproduce 1 to 4 at least five times and confirm the monitor returns to its saved level after each wake. Then run `log show --last 1h --predicate 'subsystem == "com.telaviva.oled"'` and confirm the timeline shows `display_state`, the failed `ddc` attempts (if any), `read_result`, `ownership_verified` and `restore_enqueued`, with no raw monitor id, serial or name.
- Windows 11, one external monitor: the same with the display turned off by the power plan; confirm the same lines in `%LOCALAPPDATA%\TelaVivaOLED\telaviva.log` and that the file rolls to `telaviva.log.1` past 1 MiB.
- If a monitor still stays dim, the log shows which path happened: `ownership_dropped` after `restore_failure 3/3` (raise `WAKE_READY_WINDOW_SECS`), `monitor_disconnected` then `monitor_discovered` around the wake (scenario B), or a `dim_enqueued` with no result before `display_state asleep` (scenario C).

## Notes

- No new dependency. The C ABI change is additive (`tv_diagnostics_drain`); `TV_CORE_ABI_VERSION` stays 1.
- Both platforms change in the same task list, as AGENTS.md requires. There are no new user-facing strings, so no en-US or pt-BR string changes.
- Not verified on hardware: how long the work monitor takes to answer DDC after wake. If it takes more than 30 s, raise `WAKE_READY_WINDOW_SECS`, not `MAX_RESTORE_FAILURES`.
- macOS keeps `.notice` messages for a period and volume that the system decides (typically several days); this is not configurable per app without a configuration profile. That is acceptable for field debugging and needs no code.
- Pending owner decision, not part of this plan:
  - Scenario B (rediscovered while dimmed keeps 0 as the saved level). A fix would retain the baseline of a monitor id that disconnects while the app owns a protection dim, and adopt it on rediscovery only if the fresh reading equals the value the app last wrote. It needs a change to the `contracts.md` rule that forbids reusing a baseline after reconnect and its own ADR. On Windows, a non-debounced resume refresh and transient `QueryDisplayConfig` failures make this path more likely.
  - Scenario C (display sleep while a dim write is in flight). The dispatcher does not cancel an operation whose core action was dropped, and the core ignores its late result.
- Other latent issues found during the investigation, not fixed here: a valid DDC reading of 0 is accepted without a plausibility check (correct for monitors whose minimum is 0); `MonitorPopup.clampScreensaverTarget` persists a 0 screensaver target when the popup opens while the monitor reads 0 (only matters with screensaver protection on); on Windows, a monitor discovered without a DDC port is never retried by `reopenKnownPorts`, and the `RegisterPowerSettingNotification` result is not checked, so a failed registration leaves the app with no display power events and nothing logged.

## Implementation Notes

Implemented on 2026-10-19. All eight steps are done.

- Part 1 (steps 1 to 3): `WAKE_READY_WINDOW_SECS` = 30 and `WAKE_READY_RETRY_SECS` = 2 in `retry.rs`, the window opened and closed by `Dispatch::display_power` and cleared by `shutdown`, and the new regression tests in `retry/tests.rs`, `dispatch/tests.rs` and `controls_contract.rs`. The attempt count is capped at `ATTEMPTS_PER_PRIMITIVE - 1` inside the window, so one normal attempt always remains after it ends.
- Part 2, core (step 4): `crates/telaviva-core/src/diagnostics.rs` (ring of 256 lines, `short_id`), decision lines in `core.rs` and `tv_diagnostics_drain` in `ffi.rs` and the header. Not logged: the ownership drop in `observe_monitor` (no shell sends `monitor_observed`) and `presence_dimmed` cleared outside `presence()` (display sleep, power, camera status), so a wake shows no `presence_clear` line.
- Part 2, macOS (step 5): `DiagnosticLog.swift`, the drain in `CoreBridge.swift` after each snapshot read and at shutdown, `ddc` lines in `MacDDCController.swift`, and the `com.telaviva.oled` fallback subsystem in `MacDDCController.swift` and `PresenceCamera.swift`. A new `tools/tests/test-diagnostic-log.sh` and its CI step; `DiagnosticLog.swift` was added to the compile list of every Swift test script that builds `CoreBridge.swift`. The IOAV service replacement itself is not logged; `service_marked_stale` is the only macOS reopen line.
- Part 2, Windows (step 6): `Notice` level (default), 1 MiB cap, `CoreBridge::setDiagnosticSink` and `drainDiagnostics` on the worker thread and once in `shutdown`, `DdcController::setNotice` with attempt, reopen and discovery lines, shell lines in `AppController.cpp`, the reopen trace now using `shortId`, and `TELAVIVA_LOG_LEVEL=error`. The JSON array is decoded by `CoreBridge::parseDiagnosticLines` (WinRT `JsonArray`). The test `diagnostics_rolls_file_over_at_256_kib` became `diagnostics_rolls_file_over_at_1_mib`.
- Docs (step 7): ADR 0033 and ADR 0034, pointers in ADR 0006 and ADR 0029, `ARCHITECTURE.md`, `api.md`, `docs/user-guide.md`, `README.md` and `CHANGELOG.md`.
- Version: bumped to 0.1.2 on the owner's request, as the last step; no commit, tag or GitHub Release was created.
- Verified on macOS: `cargo fmt`, `cargo clippy --workspace --all-targets -D warnings`, `cargo test --workspace` and every `tools/tests/*.sh` Swift suite pass. Not verified: the Windows C++ changes were not compiled (no Windows build here) and are covered only by the Windows CI; no hardware check of the wake behavior was run. The pending owner decisions (scenarios B and C) are unchanged.
