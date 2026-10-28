# Architecture: Keep the Computer Awake While Presence Is Detected

**Feature**: Keep awake with presence · **Date**: 2026-10-26 · **Branch**: `feat/keep-awake-with-presence` · **Specs**: [spec.md](spec.md)

## Summary

**Delivery scope update (2026-10-26)**: The owner requested macOS only in this session. Windows sections describe the intended counterpart, not completed implementation. See [windows-handoff.md](windows-handoff.md). Shared core remains in scope; `native/windows/` stays unchanged.

Add an independent consumer of local face observations to the shared core. Each native shell owns temporary OS inactivity requests and periodic activity reporting, with an independently expiring presence lease. Absence or unknown detection releases those requests immediately; the existing two-miss brightness policy remains separate.

## Technical Context

**Language/Stack**: Existing Rust core/C ABI, SwiftUI/AppKit, WinUI 3/C++/WinRT. **Dependencies**: Existing OS SDKs; no new package, asset or network service. **Storage**: Existing settings schema 1, additive Boolean defaulting to false. **Testing**: Rust policy/contract tests, native fake-backend tests and platform-specific OS acceptance. **Platform**: Apple Silicon macOS and Windows 11 x64.

## Technical Decisions

### Decision 1: Independent monitoring and inhibition policy

**What**: Persist `keep_awake_with_presence`; enable monitoring when either this setting or presence brightness protection needs it. Keep separate evidence for inhibition and separate brightness eligibility. **Why**: Brightness thresholds, battery suspension and DDC capabilities cannot stop this feature. **Alternatives Considered**: Reusing the brightness toggle/miss counter would violate the owner's decisions. **Trade-offs**: Camera and battery use increase while enabled; explain this beside the option. Stored preferences remain unchanged.

When this feature is enabled, capture is retained between bounded scheduled assessments even if `always_on_camera` is false. The existing face interval remains unchanged. Low-light recovery and camera failure backoff remain in use; unknown detection releases inhibition. Battery-related dimming eligibility remains as before even though capture now continues for this other consumer. No unrelated platform feature is reimplemented.

### Decision 2: First-miss release and finite monotonic lease

**What**: Only an accepted fresh positive observation for the current camera generation requests inhibition. First absent, low light, unavailable status, disabled setting, session transition or generation change revokes it. A positive observation grants a lease of `presence_sampling_interval_s * 1000 + 15000` milliseconds, at most 315000 ms. **Why**: Accommodates the scheduled check and bounded acquisition/assessment overhead while preventing indefinite retention after a stall. **Trade-offs**: A completely silent failure can retain inhibition until expiry; an explicit failure revokes immediately. A detection miss may permit locking before a later positive observation.

Use `Instant` for core elapsed-time evaluation and injectable monotonic time in pure policy tests. Existing epoch capture validation still rejects old, future and out-of-order observations. Snapshot `valid_for_ms` is remaining time, never a renewed TTL. A native serialized worker owns a second local deadline, and repeated snapshots with the same generation/token cannot extend it. Core snapshots do not need periodic expiry submissions: native expiry releases OS effects independently. No activity pulse occurs at or beyond expiry. Snapshot publication can stop without leaving an indefinitely renewed request.

### Decision 3: Temporary OS energy requests plus activity pulses

**What**: macOS owns idle display/system IOPM assertions. Windows owns `SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)` on one dedicated owner thread and clears it on that same thread. Both implement activity pulses only while the lease and interactive-session guard are valid. **Why**: Energy requests alone do not establish screensaver/lock suppression. **Alternatives Considered**: Editing global screensaver/power preferences breaks coexistence and crash cleanup; Away Mode has the wrong purpose; per-frame inference wastes resources.

Candidate pulse implementation: a mouse-move event at the current cursor position on macOS through Quartz, and zero relative displacement `MOUSEEVENTF_MOVE` through `SendInput` on Windows. No keys, clicks or cursor displacement. Pulse interval is one second, independent of inference cadence; the immediate first pulse is guarded identically. Event injection permission on macOS must be checked, requested only through explicit user interaction, and exposed when unavailable. Windows checks `SPI_GETBLOCKSENDINPUTRESETS`, injection return values and session eligibility without changing policy or elevating privileges.

**Trade-offs and evidence boundary**: Successful calls establish requests/event delivery, not proof that every automatic lock mechanism honors them. Mandatory policies take precedence as the owner approved. Zero-displacement activity efficacy, secure/elevated desktops, macOS posting permissions, and manual-saver races require physical acceptance. Do not claim full support until those tests pass. If a supported OS cannot meet the four outcomes without disrupting manual actions, record that failure and revise this mechanism before release; do not silently ship a narrower feature as equivalent.

### Decision 4: Manual transitions override presence

**What**: Each shell serializes eligibility and effect ownership. Lock, inactive/switched session, active screensaver, display-off or system sleep releases effects. Only an inactive/locked/unknown-lock session or actual system sleep reports `keep_awake_session {active:false}` and suspends camera demand. Saver/display/lid states keep the camera consumer active where the OS can still run, while stricter native guards continue to block activity posts. Supported clamshell operation retains external-camera monitoring; actual lid-induced system sleep takes precedence. Once a native posting veto ends, require a capture timestamp newer than the transition, even if the camera generation did not change. **Why**: A face must never unlock, wake or dismiss an intentional screensaver. **Trade-offs**: Session/camera monitoring cannot continue during actual system sleep. This is the explicit exception to always-on monitoring.

macOS reads the private `IOConsoleLocked` IORegistry root property after checking console-login eligibility, and uses an isolated, typed read-only `ScreenSaverController` runtime query for initial and live saver state. Missing or unexpected data denies activity. These private interfaces require version-specific physical acceptance; they are not stable public SDK contracts. A notification reporting inactive never replaces the live checks.

The deferred Windows implementation reuses `SessionEvents`, `PowerEvents` and `ScreensaverMonitor`; enable saver observation for either screensaver brightness protection or keep-awake. macOS adds a session guard to the existing workspace, lid and screensaver observations. Check session lock/input desktop, saver and actual display state again immediately before every pulse; unknown safety state prohibits a pulse. macOS lock/saver queries and existing distributed notification names have limited public guarantees; isolate them behind a testable adapter and document this limitation. Never interpret an unrecognized state as permission to inject input.

### Decision 5: Show requested versus effective state

**What**: The core exports desired state and lease, while native controllers expose local inactive/requesting/permission-required/unavailable status. **Why**: Core intent cannot prove OS acceptance. **Trade-offs**: More localization, but no fabricated universal capability indicator. New settings live on General with explanatory text about always-on monitoring and normal OS behavior after absence. Default remains off. UI permissions/help must be local, with no network use.

## Architecture Overview

Before: brightness protection owns camera eligibility and observations only produce brightness effects. After: either consumer can demand capture; accepted observations update brightness policy and independently update inhibition evidence. Native shell subscribers deduct time since snapshot decoding before forwarding the remaining lease to one controller owning energy requests, activity pulses and safety guards. Losing any guard revokes immediately, even if a queued positive snapshot arrives later.

### Component Structure

**New files/modules**:

- `crates/telaviva-core/src/keep_awake.rs`: pure evidence lease policy and deterministic tests if useful; the core remains the only shared policy owner.
- `crates/telaviva-core/tests/keep_awake_presence.rs`: independent camera demand, first-miss release and JSON contracts.
- `native/macos/TelaViva/KeepAwakeController.swift`: injectable controller, native assertions/activity backend and session safety adapter (may split within this directory if needed).
- `native/macos/Tests/KeepAwakeTests.swift` and `tools/tests/test-keep-awake.sh`: lease, guard, cleanup and permission tests.
- `native/windows/Platform/KeepAwakeController.h/.cpp`: UI-free serialized owner, injectable backend and monotonic lease driver.
- `native/windows/Tests/keep_awake.cpp`: matching fake-platform contract tests.

**Modified files**:

- Core `settings.rs`, `core.rs`, `lib.rs` as needed: additive field, strict session command, camera demand, retry and capture mode, independent snapshot policy.
- macOS `CoreBridge.swift`, `SettingsView.swift`, `Localization.swift`, `MenuBarController.swift`: codec, localized toggle/status, permission action, lifecycle reconciliation. Existing camera driver only changes if effective capture-mode handling needs it.
- Windows `Platform/JsonCodec.h/.cpp`, `SettingsModel.h/.cpp`: complete field round-trip and additive snapshot decoding.
- Windows `TelaViva/SettingsWindow.xaml/.h/.cpp`, `Host/SettingsHost.h`, `Host/AppController.h/.cpp`, resource dictionaries: option, host status and native effect wiring.
- Windows platform/test/app project files: register compilation units and tests; session/power/saver adapters only where necessary for eligibility.
- `docs/user-guide.md`, `README.md`, `api.md`, `ARCHITECTURE.md`, `CHANGELOG.md`, `adr/`: current behavior, permanent decision and limitations; feature validation report records actual commands and outstanding hardware acceptance.

### Component Relationships

The Rust core produces intent, never OS calls or localized messages. Native controllers consume core intent but can always veto activity for safety. OS results stay native because the core needs no platform-specific capability policy. Settings bridges carry the persisted Boolean and native UI exposes local controller status.

### Data Flow

1. The user enables the option; persistence and revision handling use the existing settings update path.
2. The camera runs despite brightness/battery gates. A fresh face sample creates a generation-bound lease.
3. The native owner accepts the remaining lease without extending an existing token, verifies guards and permissions, creates energy requests and schedules activity/expiry.
4. First no-face or unknown event changes requested state to false; the owner cancels future pulses and releases owned requests on its serialized worker.
5. A manual transition immediately revokes locally and invalidates core evidence through the session command. Old callbacks/timers cannot reactivate it. Resume requires a new-generation positive sample.

## Implementation Approach

### User Story Mapping

- **US-001 — Enable keep-awake:** core settings/monitoring, both codecs, both General pages, native controllers. Validate storage migration, continuous capture and independent OS outcomes.
- **US-002 — Leave and resume normal behavior:** core evidence, native watchdog, session/saver/power guards. Validate first-miss and unknown release, no global settings changes and manual-action precedence.
- **US-003 — Independent monitoring and honest status:** core camera gates/retry and native permission/status UI. Validate battery, threshold, no DDC display, failure, permissions and locales.
- **US-004 — Restore preferences:** settings updates and native lifecycle cleanup. Validate off/restart/crash ownership and unchanged saved camera/battery/dimming preferences.

## Integration Points

The existing generic JSON C ABI remains version 1 with additive fields and one strict command, detailed in [contracts.md](contracts.md). Schemas reject wrong types and unknown command fields as before. Build scripts already discover Swift sources; register C++ files explicitly. Tests must confirm native decoding of missing additive data remains safe.

## Technical Constraints

No new network call, identity recognition, persisted face data, global security-policy edits or elevated helper. Native effects cannot override mandatory policies. Preserve version copies; this is not a release. Respect existing DDC timing and restoration, ORT fallback and inference cadence. Windows compilation/hardware validation requires a Windows SDK/host; macOS evidence cannot substitute for it.

## Risks & Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Synthetic input is ignored or blocked | Saver/lock still occurs | Permission/policy status, actual idle deadline acceptance, no full-support claim from a return code |
| Pulse dismisses a manually started saver | Violates owner decision | Serialized guards, check immediately before post, manual-saver stress acceptance; do not release until this passes |
| Late snapshot resurrects inhibition | Unintended unlocked idle time | Token/generation lease, local monotonic expiry, guard veto and fresh evidence after resume |
| Capture demand accidentally enables dimming | Changes existing protection | Separate acquisition eligibility from brightness policy; battery/threshold/opt-out regression tests |
| UI stalls or camera silently hangs | Inhibition outlives presence | Independent watchdog; no same-token renewal; finite lease |
| Session signal is undocumented or absent | Unsafe activity posting | Adapter isolation, fail on unknown state, explicit hardware acceptance boundaries |
| Camera always-on raises resource use | Battery/CPU regression | Existing bounded inference, capture frame-rate limits and AC/battery measurement |

## Open Questions

None requiring an owner decision. The user resolved all product choices. OS pulse efficacy and manual-saver preservation are implementation validation gates, not assumed results. A failing gate must be reported as incomplete behavior and resolved before claiming full feature acceptance.

## Next Steps

1. Generate `plan.md` with test-first native safety/lease validation and both platforms in scope.
2. Execute the macOS and shared-core portion with `codeadvisor-dev-implement --ff`; defer Windows tasks per the owner scope update.
3. Record automated evidence and physical/Windows acceptance separately; no publishing is authorized by this workflow.

## Primary Research

- [Microsoft execution state](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate): energy requests, thread ownership, manual sleep and screensaver limitation.
- [Microsoft SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput): injection results and integrity restrictions; not a screensaver or session-lock guarantee.
- [Microsoft SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow): screensaver state and input-reset policy inspection.
- [Apple idle display assertion](https://developer.apple.com/documentation/iokit/kiopmassertiontypepreventuseridledisplaysleep), [idle system assertion](https://developer.apple.com/documentation/iokit/kiopmassertiontypepreventuseridlesystemsleep).
- [Apple event posting permission](https://developer.apple.com/documentation/coregraphics/cgrequestposteventaccess%28%29).
- [Apple QA1340](https://developer.apple.com/library/archive/qa/qa1340/_index.html): idle versus forced sleep and lifecycle notifications.
