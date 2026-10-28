# Windows implementation handoff

**Status (2026-10-27): implemented; physical acceptance marked complete by the owner.** The 2026-10-26 session was narrowed to macOS. The Windows shell was implemented on 2026-10-27 following this handoff; build and native test evidence is recorded in the Windows section of [validation.md](validation.md). This file is kept as the design record. The acceptance section at the end still applies in full.

## Current shared contract

The current source of truth is [contracts.md](contracts.md), with product rules in [spec.md](spec.md) and design decisions in [architecture.md](architecture.md). The core starts `keep_awake_session` eligibility as false; the Windows shell must reconcile actual current guards by sending `{"type":"keep_awake_session","active":true}` when appropriate, before it expects camera evidence. This command only enables eligible monitoring; it never grants presence. On any eligibility change, the core invalidates evidence/generation and requires a fresh positive sample.

Settings schema 1 gains `keep_awake_with_presence` (Boolean, default false). Preserve full settings payloads and all existing preferences. The additive snapshot `keep_awake` contains `enabled`, `requested`, `observation_id`, `generation`, `valid_for_ms`, and `reason`. Missing snapshot data must decode safely as disabled/not requested. Lease tokens are observation timestamp plus camera generation, not snapshot revision. A repeated token may shorten but never extend the local monotonic deadline; subtract queue delay from remaining TTL. The core lease is sampling interval plus 15 seconds, capped at 315 seconds.

First valid absent, low light, unavailable/unknown capture, expired evidence, generation change, disable, manual lock/sleep, active screensaver, display-off, lid close and shutdown all revoke immediately. Re-entry into an eligible session starts monitoring and requires new positive evidence. Never auto-unlock or wake. Keep-awake monitoring runs independently of battery/brightness/DDC gates, without changing the user's saved camera or protection preferences. Manual action and mandatory system/security policy take precedence.

## Windows components to implement

- `native/windows/Platform/KeepAwakeController.h/.cpp`: UI-free, injectable policy/backend and a dedicated owner worker. Keep the worker asleep indefinitely when there is no active lease or command; wake on command, expiry, or the next required guard/pulse poll. Do not poll once per second while disabled.
- Energy request: `SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)` and clear the app's request on that same worker/thread. Do not use Away Mode. This addresses idle display/system sleep requests only; Microsoft explicitly says it does not suppress screensavers or explicit user sleep.
- Activity pulse: candidate is zero-displacement `SendInput` mouse movement, with no key/click and no cursor movement. Check `SPI_GETBLOCKSENDINPUTRESETS`, the injection result, interactive session and every local veto immediately before each pulse, including the immediate first pulse. Unknown state denies the pulse. A successful API return does not establish that all lock mechanisms honor the event.
- Session/safety integration: reuse `SessionEvents`, power notifications, `ScreensaverMonitor`, and laptop environment state. The existing `SessionEvents` adapter handles session lock/unlock; extend it for suspend/resume if needed. Ensure saver observation is active when either saver-based dimming or keep-awake needs it. Poll or subscribe to actual saver state and fail closed on unknown. Check display state and lid state as local vetoes too.
- Settings/UI: `Platform/JsonCodec.*`, `SettingsModel.*`, Windows General settings UI/host and both locale resource dictionaries; surface saved option separately from current status. Distinguish waiting, active request, unavailable/permission, and policy-blocked states without claiming policy suppression.
- Tests: `native/windows/Tests/keep_awake.cpp`, plus settings payload, JSON codec, session event, and localization parity coverage. Use fake backend and injectable monotonic clock/scheduler for expiry, disable, stale/replayed token, same-token nonrenewal, queue-delay expiry, local guard changes, release/shutdown, failed calls and policy block.

## Implementation hazards already identified

- Construct all worker state before starting the thread. Starting a member worker during member initialization can race reads of later-declared stop/status fields; start it in the constructor body or declare it last.
- Stamp lease receipt at enqueue time and subtract time spent queued when processing it. Otherwise a stalled worker silently extends the lease.
- Schedule the wake for the minimum of pulse time and lease deadline; a fixed one-second wait can hold effects past expiry.
- Update local guard vetoes synchronously before queuing a core `active:false` command. A queued old snapshot must not override the veto. Keep a token/generation high-water mark across revocation, including `observation_id == 0` release snapshots, so replay cannot restore a prior request.
- `OpenInputDesktop` alone is not sufficient evidence that the session is unlocked. Check the opened desktop name is exactly `Default`, confirm current WTS session is active, and check display/lid/saver guards directly before injection. If any query fails, do not pulse.
- A blocked-injection/policy path must report a localized policy status rather than remain indistinguishable from ordinary waiting. Distinguish OS request acceptance from verified behavior.
- Release the execution-state request on first absence/unknown/guard change/expiry and during shutdown. Surface a failed clear as unavailable and retry/clean up safely; thread termination clears that thread's execution-state contribution, but a release failure during continued app operation still needs explicit handling.
- Core `session_active` starts false. Initial native reconciliation must not wait for a screensaver observer that is only enabled by the old saver-protection setting; otherwise keep-awake-only startup can deadlock before camera monitoring begins.
- Keep a single owner for worker callbacks and join it before freeing callback targets. Do not let UI refresh cadence own expiry or cleanup.

## Build and test commands (Windows host required)

From repository root, run the supported end-to-end package build, which compiles Rust/native targets and runs the native test executable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Configuration Debug -NoZip
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Configuration Release -NoZip
```

The script requires Windows 11 x64, Windows PowerShell 5.1, stable Rust `x86_64-pc-windows-msvc`, Visual Studio 2022 C++ tools and Windows SDK 10.0.26100. If native-only iteration is useful after Rust outputs exist, build the solution and run its tests explicitly:

```powershell
msbuild native\windows\TelaViva.sln /m /p:Configuration=Debug /p:Platform=x64
native\windows\build\x64\Debug\tests.exe
```

`scripts\build-windows.ps1` already builds the app and runs `native\windows\build\x64\<Configuration>\tests.exe`; use its `-NoZip` option to avoid packaging. Verify test project wildcard inclusion or explicitly register any new test source, and verify Platform/App project registration before relying on a successful build. On non-Windows hosts, do not claim Windows compilation or tests passed.

## Acceptance still required

Automated policy tests do not prove the four OS outcomes. On Windows 11, separately record configured idle deadlines and observed durations for automatic screensaver, automatic session lock, display sleep, and system sleep with a visible face and no physical input. Also test first absent, camera/low-light failure, permission/policy rejection, expiry/stalled worker, manual lock, explicit sleep, manually started screensaver, lid close, display off, suspend/resume, secure/elevated desktop and recovery with fresh presence. Verify no automatic unlock/wake and no permanent power/security preference edits. Mandatory managed policy may still lock despite the activity pulse; do not describe that as overridden or claim universal suppression from `SetThreadExecutionState`/`SendInput` return values.

Windows host acceptance, physical behavior and resource measurements were marked complete by the owner on 2026-10-27; no values are recorded here. The Windows SDK build and tests must be reported separately from macOS evidence.
