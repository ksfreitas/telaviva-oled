# Validation: Keep Awake With Presence

**Date**: 2026-10-26 · **Branch**: `feat/keep-awake-with-presence`

## Scope and status

The owner explicitly narrowed this session to shared core and macOS. Windows native implementation is deferred; `native/windows/` is unchanged. The [Windows handoff](windows-handoff.md) preserves remaining work. No release/version bump or installed-application replacement is part of this delivery.

Shared-core and macOS implementation and automated verification are complete. Physical OS acceptance remains pending. This report distinguishes policy/backend tests from physical OS acceptance. It does not claim that API success proves automatic-lock suppression.

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
| Windows build and native tests | Deferred by owner | Later Windows session |
| `git diff --check` and scope review | Passed | No Windows native changes or version edits |

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

All rows below remain pending unless explicitly replaced by observed results. Automated fakes do not check these boxes.

- [ ] Face visible, no keyboard/mouse: remain active beyond the separately configured screensaver, automatic-lock, display-sleep and system-sleep deadlines; record macOS version, deadlines and duration.
- [ ] Leave camera view: first valid no-face result releases owned requests; ordinary OS inactivity behavior returns without changed timeouts.
- [ ] Low light, camera disconnect, permission denial and silent capture stall: release, with finite expiry for silent failure; recover only on fresh presence.
- [ ] Lock manually and keep a face in view: never dismiss/unlock the session. Unlock normally and require fresh evidence.
- [ ] Start a screensaver manually, including immediately before a pulse and before app launch: it stays active until ordinary user action ends it.
- [ ] Display-off, sleep, lid close, Fast User Switching, wake and reopen: explicit actions prevail and no stale token reactivates effects.
- [ ] Permission grant/revocation: localized UI and native behavior agree; no automatic repeated permission prompt.
- [ ] Disable, quit, abnormal exit and restart: no persistent OS power/security preference changes or stale owned requests.
- [ ] Measure camera-open time, inference cadence and CPU/memory on AC/battery with feature off/on; disabled feature creates no recurring keep-awake work.

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

The running app logged an unlocked session, saver inactive, display awake, lid closed, `safety=eligible`, `camera_monitoring=true` and `posting_permission=true`. This verifies current guard eligibility in the user's clamshell configuration; it does not substitute for physical face-away/DDC or OS inactivity-deadline acceptance. No camera images were stored or sent, and no OS power/security settings were changed. The broader physical checklist above remains pending unless separately observed.
