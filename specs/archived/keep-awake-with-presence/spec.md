# Feature Specification: Keep the Computer Awake While Presence Is Detected

**Feature Branch**: `feat/keep-awake-with-presence`
**Current delivery scope**: macOS only, explicitly narrowed by the owner on 2026-10-26 during implementation. Windows remains part of the eventual requirements below; native Windows implementation and validation are deferred to a later session on a Windows PC. Shared core contracts are implemented now.
**Input**: User description: "Adicione uma opção em ambas as plataformas para que quando ativada, a tela nunca bloqueia, nunca desliga, nunca suspende o computador enquanto houver presença. Quando ativada, o monitoramento é sempre ativo. Quando sair a presença, libera a proteção de tela, desligamento e suspenção (volta ao normal)"

## Context and Understanding

TelaViva currently detects any face locally to protect display brightness when the user is away. It does not keep the operating system awake. Its existing screensaver protection dims displays when a screensaver runs; it does not prevent screensaver activation or session locking.

Presence acquisition currently depends on brightness protection being enabled, power policy, eligible displays, display sleep and brightness thresholds. Keeping the camera open between samples does not override those gates. The requested feature needs presence monitoring independently of whether a display can be dimmed, so someone reading or watching the screen without keyboard or mouse input can remain active as long as a face is detected.

The original requirements target macOS and Windows. The owner later narrowed this delivery to shared core and macOS; Windows remains deferred as recorded above. This document defines behavior; implementation evidence is recorded separately in validation.md.

## Feature Description

Add a persistent option, named **Keep awake while a face is detected** / **Manter ativo enquanto um rosto for detectado**, that prevents automatic screensaver activation, automatic session locking, display power-off due to inactivity, and computer sleep due to inactivity while valid presence is detected. Once absence is established, release TelaViva's inhibition and allow the operating system's normal behavior to apply.

Enabling this option also enables ongoing presence monitoring, including on battery, below brightness thresholds, and without a display eligible for brightness protection. It works independently of the brightness-protection toggle. Ongoing monitoring means detection remains scheduled while the app and session can run; it does not mean inference on every camera frame or an ability to execute while the computer is asleep.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As a user on either platform, I can enable one option to keep my display and computer active while I am present, without periodically touching the keyboard or mouse.
- **US-002**: As a user, I can leave the computer and have normal screensaver, locking, display sleep and system sleep behavior resume automatically.
- **US-003**: As a user, I can use this option independently of OLED brightness protection and see when presence detection or keep-awake behavior is unavailable.
- **US-004**: As a user, I can disable the option without losing my previous battery, camera and brightness-protection preferences.

### Functional Requirements

- **FR-001**: The system MUST expose the persistent option on both macOS and Windows, with equivalent behavior and en-US and pt-BR strings. Proposed default: disabled for new and existing installations.
- **FR-002**: While the option is enabled and presence is valid, the system MUST prevent automatic screensaver activation, automatic session locking, display power-off from inactivity and system sleep from inactivity. Each outcome MUST be validated separately; keeping the display awake alone is insufficient.
- **FR-003**: While the option is enabled, monitoring MUST remain enabled independently of presence-based brightness protection, screensaver brightness protection, battery-suspension preferences, display brightness thresholds, display protection eligibility and whether any controllable external monitor is connected. Stored preferences MUST remain unchanged and apply normally when the option is disabled.
- **FR-004**: Enabling the option MUST keep the camera session active between scheduled checks where capture is available, overriding the effective **Keep camera on** preference without rewriting it. Detection MUST retain bounded sampling rather than infer on every frame, and minimize resource use.
- **FR-005**: Presence MUST mean detection of any face, without identity recognition or enrollment. Frames MUST remain local; the feature MUST add no network use.
- **FR-006**: On confirmed absence, the system MUST release only TelaViva's keep-awake effects. It MUST allow the OS to apply existing settings without forcing immediate lock, screensaver, display-off or sleep, changing saved OS timeouts, or cancelling another application's keep-awake effects. Release does not promise a new full OS idle interval.
- **FR-007**: On disabling the option or exiting the app, the system MUST release its keep-awake effects. Restarting the app MUST require fresh presence before inhibiting inactivity behavior. Abnormal termination MUST NOT leave permanent OS preference changes behind.
- **FR-008**: Explicit manual lock, sleep, screensaver and shutdown actions, lid behavior and mandatory OS/security policies MUST take precedence. The feature MUST NOT automatically unlock an already locked session or wake a sleeping computer when a face returns.
- **FR-009**: Low light, denied permission, camera disconnection, capture failure or expired presence evidence MUST release inhibition rather than retain the last positive result indefinitely. These conditions MUST remain distinct from confirmed absence for existing brightness protection. Detection recovery MUST allow inhibition again after fresh presence.
- **FR-010**: The first valid no-face observation MUST release inhibition, without waiting for a second observation or adding a grace period. This rule MUST work even when brightness protection is off or no dimmable display is available. Existing brightness protection MUST retain its separate two-miss rule. A brief valid detection miss may therefore allow automatic locking or sleep before the next positive sample.
- **FR-011**: Camera permission denial or capture failure MUST be visible in localized UI. The UI MUST distinguish the saved option from currently effective inhibition and MUST NOT claim every requested effect is active if the platform rejects any of them.
- **FR-012**: After a manual session transition or system wake, monitoring MUST resume when capture is permitted and the option remains enabled; fresh evidence MUST be required before inhibition resumes. Monitoring MUST NOT override an explicit session lock or physical camera unavailability.
- **FR-013**: When the option is disabled, existing brightness protection, screensaver protection, camera gating, retry behavior and stored preferences MUST retain their current behavior. When enabled, existing brightness protections MUST continue according to their own settings without this feature treating an unavailable camera as absence.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On each platform, with a visible face and the option enabled, separate tests for screensaver activation, automatic locking, display sleep and system sleep pass after waiting beyond the configured inactivity deadlines, without keyboard or mouse input. Record deadlines, observation durations and OS configuration.
- **SC-002**: After the first valid no-face result, the feature releases inhibition on the next policy reconciliation, with no second-miss confirmation or extra grace period. With defaults, the target detection delay is approximately at most one normal sampling interval plus capture/assessment time during healthy capture; physical departure is not detected instantaneously. Existing brightness protection still waits for its second valid no-face result.
- **SC-003**: With this option enabled, monitoring remains scheduled in each of these scenarios on both platforms: battery operation, low display brightness, brightness protection disabled, no external monitor, and no display eligible for dimming. With it disabled, previous gating and preferences are restored.
- **SC-004**: Disabling the option, quitting, restarting, losing camera permission, disconnecting the camera and entering low light produce the agreed release/recovery behavior without permanent OS preference changes. Design MUST define a finite, testable evidence-expiry deadline for stalled capture.
- **SC-005**: Manual lock and sleep behavior passes the agreed C1 acceptance cases on each platform; returning presence does not bypass session authentication.
- **SC-006**: Tests on both platforms cover enable/disable, persistence and migration, present/absent/unknown transitions, stale observations, failure/recovery and coexistence with brightness protection. Physical OS acceptance is recorded separately from simulated or automated policy tests.
- **SC-007**: Both locales expose the option and effective status without missing strings. Resource measurements compare feature-off and feature-on behavior on AC and battery, including camera-open time and inference cadence; no per-frame inference loop is introduced by the feature.

## Clarification Needed

None. The owner resolved all three questions on 2026-10-26 with "1) A; 2) A; 3) B".

### Recorded Decisions

1. **C1 — A:** Prevent automatic inactivity behavior only; preserve manual lock/sleep/screensaver, lid behavior and mandatory system policies. Never auto-unlock or auto-wake.
2. **C2 — A:** When detection is unavailable, release inhibition and allow normal OS behavior. Resume inhibition only after fresh positive detection.
3. **C3 — B:** Release on the first valid no-face result. This is intentionally independent of brightness protection's existing two-miss rule; brief misses may allow automatic locking or sleep.

## Notes

- The request explicitly makes monitoring always active while this option is enabled. Battery and brightness preferences therefore do not veto this feature; they remain stored for normal operation afterward. This is not an unresolved question.
- The design must assess OS support for all four automatic behaviors independently, including required permissions and policies, and bring any inability to meet the requested behavior back to the owner before narrowing the scope.
- Microsoft documents that its execution-state sleep prevention does not suppress screensavers and does not prevent explicit user sleep: [SetThreadExecutionState](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate). Apple describes idle-display sleep prevention separately: [PreventUserIdleDisplaySleep](https://developer.apple.com/documentation/iokit/kiopmassertiontypepreventuseridledisplaysleep). These establish design investigation needs, not selected implementation APIs or proof that automatic locking is solved.
- Current behavior was checked against `docs/user-guide.md`, `api.md`, shared core camera gates, and both native camera/screensaver integrations. In particular, camera-open retention is distinct from always-enabled monitoring.
- Out of scope: identity-based authentication, automatic unlocking, changing OS power/security preferences permanently, waking a sleeping system, release publishing and changes to monitor brightness restoration semantics.
- Discovery is resolved. Next workflow: `codeadvisor-dev-design`. This specification contains no implementation plan.
