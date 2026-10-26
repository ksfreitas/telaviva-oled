# ADR 0040: Presence-driven idle inhibition

- Status: Accepted design; physical OS acceptance pending
- Date: 2026-10-26
- Feature: [Keep awake with presence](../specs/keep-awake-with-presence/spec.md)

## Context

Brightness-based OLED protection cannot keep a reader's computer active without keyboard/mouse interaction. Its camera demand depends on brightness, battery and eligible monitors, so reusing that toggle would not satisfy independent presence-driven inactivity prevention on macOS and Windows.

The owner explicitly selected prevention of automatic inactivity behavior only, release when detection is unknown, and release on the first valid no-face result. Manual session actions and mandatory security policies retain precedence.

## Decision

Add a default-off, persisted `keep_awake_with_presence` option. The core owns independent evidence and camera demand; each native shell owns OS requests and a serialized activity driver. Monitoring continues on battery and below brightness thresholds while enabled and retains capture between bounded inference checks. Other preferences stay stored unchanged; brightness protection retains its own eligibility and two-miss confirmation.

A valid positive sample grants a monotonic lease of the configured normal sampling interval plus 15 seconds. First absence, unknown detection, generation changes, inactive session, disabling or shutdown revokes inhibition. Native expiry is independent of later camera callbacks and UI snapshot publication. Same-token snapshots cannot extend a lease.

Use process-owned idle-display/system requests plus guarded activity reporting. Do not change global screensaver/power/security preferences, elevate privileges or add network use. Manual lock, saver, display-off and sleep transitions veto the native activity driver and require fresh presence after resume. Camera demand is separate: saver/display state and an awake clamshell session do not alone stop inference; a locked/inactive/unknown-lock session or actual system sleep does. Resume from a posting veto requires an observation capture timestamp newer than the transition even if camera generation did not change. Native status distinguishes the saved option, current requests and permission/policy failures.

## Alternatives

- Energy requests alone: insufficient evidence for screensaver/session-lock prevention.
- Changing global inactivity settings: unsafe coexistence and abnormal-exit restoration.
- Reusing the brightness absence counter: violates the requested first-miss release rule.
- Per-frame face inference: unnecessary camera/CPU/battery cost.

## Consequences

The feature uses more camera time when enabled and may need event-posting permission on macOS. Synthetic activity can be rejected or ignored; successful API calls do not prove all lock policies are suppressed. macOS uses the private IORegistry `IOConsoleLocked` property and a typed read-only `ScreenSaverController` runtime query alongside session/saver notifications. Unknown data blocks activity. These platform-specific limitations must be tested on target OS versions. Automated fake-backend tests cannot replace OS idle-deadline and manual-saver acceptance. A failing acceptance case must be recorded and fixed before claiming full support.

Implementation details and actual validation evidence remain in [architecture.md](../specs/keep-awake-with-presence/architecture.md), [contracts.md](../specs/keep-awake-with-presence/contracts.md) and [validation.md](../specs/keep-awake-with-presence/validation.md).
