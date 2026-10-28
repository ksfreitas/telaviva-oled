# Validation: Keep Awake With Presence

**Date**: 2026-10-26 · **Branch**: `feat/keep-awake-with-presence`

## Scope and status

The first delivery (2026-10-26) was narrowed by the owner to the shared core and macOS. The Windows native implementation was then completed on 2026-10-27 and is reported in [Windows implementation](#windows-implementation-2026-10-27); the [Windows handoff](windows-handoff.md) is kept as the design record. No release/version bump or installed-application replacement is part of either delivery.

Shared-core and macOS implementation and automated verification are complete. Physical OS acceptance was marked complete by the owner on 2026-10-27; this report holds no recorded values for it. This report distinguishes policy/backend tests from physical OS acceptance. It does not claim that API success proves automatic-lock suppression.

## Automated checks

| Check | Result | Evidence boundary |
| --- | --- | --- |
| `cargo fmt --all -- --check` | Passed | Current shared Rust sources |
| `cargo clippy --workspace --all-targets --locked -- -D warnings` | Passed | No Rust warnings |
| `cargo test --workspace --all-targets --locked --quiet` | Passed: 568 tests | Includes 9 keep-awake integration tests, 3 deterministic lease tests, version consistency and bundled CPU inference smoke; Windows-only tests compile out on macOS |
| `tools/tests/test-keep-awake.sh` | Passed | Fake native backend: no input posted to the user session |
| Core bridge / camera / localization scripts | Passed | Additive decoding, persistence and unchanged camera behavior |
| `scripts/build-macos.sh` | Passed | Apple Silicon application built at `build/macos/TelaViva OLED.app`; no physical presence/idle acceptance implied |
| Packaged `Contents/MacOS/TelaViva --smoke-test` | Passed, exit 0 | Packaged ABI/inference startup smoke, no live-camera or inactivity proof |
| `codesign --verify --deep --strict` | Passed | Local ad-hoc package signature, not notarization |
| Settings window layout / camera lifecycle bridge scripts | Passed | Native regression suites |
| Windows build and native tests | Passed on 2026-10-27 | See [Windows implementation](#windows-implementation-2026-10-27); this row was deferred in the macOS delivery |
| `git diff --check` and scope review | Passed | macOS delivery: no Windows native changes or version edits; the Windows delivery is scoped below |

An intermediate Rust check ran during an agent's refactor and failed on removed internal type names; the completed source was rechecked successfully with the full commands above. That transient failure is not an unresolved result.

## Policy coverage

The shared tests cover default-off migration; continuous monitoring with no eligible display and on battery/unknown power; preservation of saved camera/battery options; low-brightness and display opt-out gating for brightness protection; first-absence inhibition release while brightness still requires two misses; unknown/light/camera-failure release; session and settings generation invalidation; stale-observation rejection; idempotent session reports; strict command validation; monotonic expiry and the maximum lease. Session reports leave brightness-only camera state unchanged while the new feature is disabled.

Passing native tests additionally establish nonrenewal of a repeated token, revocation barriers, first absent with `observation_id=0`, inactive/resume snapshots, independent exact-deadline expiry, no disabled idle timer, cleanup, permission denial and assertion/post failures. Native snapshot queue time must be deducted from the remaining evidence lease.

## OS integration limitations

- Idle display/system requests alone do not establish screensaver/session-lock prevention. The selected additional mechanism is zero-displacement activity posting, conditional on current lease, permissions and session guards.
- macOS exposes no substantiated public general initial screensaver/lock query for this use. The lock guard uses read-only `IOConsoleLocked` from the IORegistry root with public console-session eligibility checks; unknown/type mismatch denies activity. This private key is supported by [Apple's XNU implementation](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/iokit/Kernel/IOService.cpp) and [private-key definitions](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/iokit/IOKit/IOKitKeysPrivate.h), but is not a stable public SDK contract.
- The native adapter loads the system ScreenSaver framework and checks the runtime signatures for `ScreenSaverController +controller` and `-screenSaverIsRunning` before making a typed read-only call. These are private APIs; missing/unexpected signatures produce unknown and deny activity. The current host returned true for `screenSaverIsRunning` and false for its background-state query. That observation does not establish whether a visible saver was active or that the method has the required semantics across macOS versions. No saver was stopped or overridden for testing.
- Existing screensaver/lock distributed notification names are undocumented. A cached active notification can veto immediately, but a cached inactive notification cannot bypass a live guard query. `WallpaperAgent` process presence is not treated as proof a saver is running.
- Mandatory security policies, explicit lock/sleep/saver/display-off, lid state and forced system sleep take precedence. The app never unlocks or wakes on presence.

## Physical acceptance checklist

All rows below were marked complete by the owner on 2026-10-27, who stated they were observed on hardware. No versions, deadlines, durations or measurements are recorded here; automated fakes did not check these boxes.

- [x] Face visible, no keyboard/mouse: remain active beyond the separately configured screensaver, automatic-lock, display-sleep and system-sleep deadlines; record macOS version, deadlines and duration.
- [x] Leave camera view: first valid no-face result releases owned requests; ordinary OS inactivity behavior returns without changed timeouts.
- [x] Low light, camera disconnect, permission denial and silent capture stall: release, with finite expiry for silent failure; recover only on fresh presence.
- [x] Lock manually and keep a face in view: never dismiss/unlock the session. Unlock normally and require fresh evidence.
- [x] Start a screensaver manually, including immediately before a pulse and before app launch: it stays active until ordinary user action ends it.
- [x] Display-off, sleep, lid close, Fast User Switching, wake and reopen: explicit actions prevail and no stale token reactivates effects.
- [x] Permission grant/revocation: localized UI and native behavior agree; no automatic repeated permission prompt.
- [x] Disable, quit, abnormal exit and restart: no persistent OS power/security preference changes or stale owned requests.
- [x] Measure camera-open time, inference cadence and CPU/memory on AC/battery with feature off/on; disabled feature creates no recurring keep-awake work.

## Security and resource review

No high-confidence exploitable vulnerabilities identified in the scoped review. The intended boundary is app-owned, finite inhibition based on local any-face observations; it is not authentication. No enrollment, face identity, network call, elevated helper or global security-setting mutation is introduced. Session transitions revoke before queued snapshots can act. Review fixed stale generation acceptance after newer negative snapshots, zero-token absence handling, generation floors after resume, expired/queued leases, missing-permission cleanup and cached inactive guards. Remaining risks are the private OS state interfaces, manual-saver transition races and unmeasured physical inactivity/resource behavior; these are not reported as verified support.

The standard macOS build retains an existing `Sendable` warning around a `self` capture in `MenuBarController.swift` (outside the added keep-awake paths). It does not fail the build.

No real keyboard/mouse activity was injected, camera frame collected, session locked, saver stopped or OS timeout changed for the automated checks in this session.

## Version metadata update: 0.1.4

After feature validation, the application metadata was advanced to 0.1.4: both Rust crate manifests, Cargo-generated lockfile entries, both macOS plist keys, Windows assembly version 0.1.4.0, README and changelog. `cargo check --workspace --quiet` and all three `version_consistency` tests passed. The Windows manifest change is version metadata only; the earlier physical and platform validation results remain unchanged. The previously built app bundle was not regenerated by this metadata-only update.

## Follow-up corrections and installed build: 2026-10-26

Three user-reported regressions were addressed in the 0.1.4 build:

- The macOS protection target slider used snapshot-backed reads during asynchronous settings writes. Both external and built-in cards now retain a local draft through editing and acknowledgement, with a fixed 1–50% range. A dedicated asynchronous bridge operation reads the latest core target map and submits only the changed settings map under the FFI lock. Regression tests reproduce lost per-display targets and verify that waiting for the worker does not block the caller or overwrite keep-awake/protection preferences.
- Native camera eligibility had been coupled to the stricter activity-posting guards, including lid state. Monitoring now continues for an awake clamshell session and during saver/display posting vetoes. Real lock/inactive/unknown-lock/system-sleep still blocks capture demand; all native activity posts retain their strict guard checks. First post-veto evidence must have an observation capture timestamp newer than the transition, including when the core generation did not change. The core no longer labels a session pause as low display brightness.
- The GitHub latest-release response was HTTP 200 with tag `v0.1.3.1`; the old strict three-component parser rejected it. Comparison now accepts three or four numeric components, with an omitted revision treated as zero, while rejecting malformed tags. Tests establish that installed 0.1.4 is newer than 0.1.3.1 and installed 0.1.3 is older.

Final checks passed: Rust formatting, workspace Clippy with warnings denied, all 570 workspace tests, protection-draft tests, asynchronous CoreBridge tests, keep-awake guard/lease tests, built-in protection, camera/localization, popup layout, update checker, standard macOS package build, packaged smoke test, signature verification and `git diff --check`. IntelliJ reported no diagnostics in the nine implementation/helper files rechecked. The known Sendable warning in the existing settings-close observer remains in the Swift package build.

The signed 0.1.4 bundle was installed at `/Applications/TelaViva OLED.app` and reopened (PID 83069 at verification). Its executable matched the built artifact byte for byte, and its installed signature verified. The previous bundle was preserved at `/Users/cassio/Library/Application Support/TelaVivaOLED/Backups/20261026-145251/TelaViva OLED.app`.

The running app logged an unlocked session, saver inactive, display awake, lid closed, `safety=eligible`, `camera_monitoring=true` and `posting_permission=true`. This verifies current guard eligibility in the user's clamshell configuration; it does not substitute for physical face-away/DDC or OS inactivity-deadline acceptance. No camera images were stored or sent, and no OS power/security settings were changed. The broader physical checklist above was later marked complete by the owner on 2026-10-27.

## Windows implementation (2026-10-27)

Owner request: implement the Windows version of this feature (tasks T010–T012, T015–T016 and the Windows part of T020). The shared core and ABI are unchanged; the Windows shell consumes the existing settings key, `keep_awake` snapshot object and `keep_awake_session` command.

### What was built

- `Platform/KeepAwakeController.*`: the macOS lease/guard policy ported, with an injectable backend, `IScheduler` and `IClock`. State sits under one mutex; timer callbacks hold only a weak reference plus a sequence number, never call `scheduler.cancel`, and cancellation of superseded timers runs after the mutex is released. A failed timer creation fails closed as unavailable.
- `Platform/Win32KeepAwakeBackend.*`: `SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)` and a zero-displacement `SendInput` mouse move on one lazily created owner thread (none while disabled); posting is denied when `SPI_GETBLOCKSENDINPUTRESETS` reports blocked or fails. The live session guard requires the console session to be active and the input desktop to be named `Default`; any failed query is unknown and denies the pulse.
- Codec and settings: `keep_awake_with_presence`, the `keep_awake` snapshot object (a missing or non-object value decodes as inert), `buildKeepAwakeSession`, and remaining-lease accounting from the snapshot decode time.
- Host: `SessionEvents` gained `onSuspend`; `AppController` now assigns the previously unassigned `onSessionLock`, forwards display, resume, suspend, lid and saver changes to the backend, reports the local session state to the core under one mutex, applies the lease after `publish()` returns, enables saver observation for either consumer, and releases the OS request first on shutdown.
- UI: General page toggle with help, monitoring note and a status line refreshed every two seconds while the option is on; strings in en-US and pt-BR.
- A closed lid with an awake session is not a pulse veto, matching macOS.

### Automated checks

| Check | Result | Evidence boundary |
| --- | --- | --- |
| `msbuild native\windows\TelaViva.sln /p:Configuration=Debug /p:Platform=x64` | Passed, no warnings from this change | Platform, Tests and the WinUI app compile and link |
| `tests.exe` (Debug) | Passed: 777 tests run, 0 failed checks | 774 before the UI and host work; includes 23 keep-awake controller/backend-policy cases, codec, settings payload, session event and localization key tests |
| `scripts\build-windows.ps1 -Configuration Release -NoZip` | Passed, exit 0 | Rust DLLs, Release app assembled in `dist\TelaVivaOLED` (283 files), `tests.exe` (Release) 777 tests, 0 failed |
| `git diff --check` | Passed | |

The Rust workspace was not changed by this delivery and was not rerun; the macOS delivery's Rust results above stand. The OS calls (`SetThreadExecutionState`, `SendInput`, the live session queries) are exercised only by the Windows build, not by the fake-backend tests.

### Windows physical acceptance (marked complete by the owner)

All rows were marked complete by the owner on 2026-10-27, who stated they were observed on Windows 11. No deadlines, durations or measurements are recorded here; the fake-backend tests do not check them.

- [x] Face visible, no keyboard or mouse: remain active beyond the configured automatic screensaver, automatic lock, display sleep and system sleep deadlines; record the deadlines and observed durations.
- [x] First no-face result, low light, camera disconnect and silent stall: release; ordinary idle behavior returns without changed timeouts.
- [x] Policy rejection: with synthetic input resets blocked, the status shows the policy-blocked message and no pulses are sent.
- [x] Manual lock with a face in view never dismisses the lock; after unlock, fresh evidence is required.
- [x] A manually started screen saver (including just before a pulse and before launch) is not dismissed by a pulse.
- [x] Display off, explicit sleep and resume, lid close with and without an external monitor, secure or elevated desktop (UAC), and Fast User Switching: explicit actions prevail and no stale lease reactivates effects.
- [x] Disable, quit and abnormal exit: no persistent power or security preference changes and no stale execution-state request.
- [x] Measure camera-open time, CPU and memory on AC and battery with the option off and on; the disabled option creates no recurring keep-awake work and no owner thread.
