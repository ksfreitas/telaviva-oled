# ADR-0020: Windows efficiency: EcoQoS set once, UI exemption only while a window is visible, user-forced Efficiency mode respected

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

The Windows spec requires minimal background cost (FR-025 to FR-028) and respect for a user-forced Efficiency mode (Q7), without raising the timer resolution.

## Decision

- At startup the process requests EcoQoS once (`ProcessPowerThrottling`, `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`). The app never calls `timeBeginPeriod`.
- While the popup or Settings window is visible, only the UI thread requests normal quality (thread level). It returns to EcoQoS when the window is hidden.
- The process-level state is never reasserted after startup.
- If the priority class is idle, or the process-level state differs from what the app set (the user forced Efficiency mode), the controller enters UserManaged: no thread exemptions, no process-level writes, and a diagnostic line.
- The hook thread requests normal quality while the keyboard hook is installed, except in UserManaged, where no thread is exempted and a delayed hook is logged.
- Timers use thread-pool timers with a tolerable delay of 10% of the delay, at most 1 s.

## Alternatives Considered

- **No EcoQoS.** Higher idle cost.
- **Raising the timer resolution.** Violates FR-027.
- **Always normal quality.** Fights the user's choice.

## Consequences

- Behavior under a forced Efficiency mode is not measured. Not run: spikes S6 and S8 (forced-mode detection and EcoQoS effects), and the EcoQoS on-versus-off measurement.
- Under UserManaged, the keyboard hook may be delayed. The watchdog and logging are the only mitigation (see [ADR 0021](0021-windows-volume-key-hook-and-route.md)).
- Evidence:
  - `native/windows/Tests/efficiency_policy.cpp` holds policy tests with a fake `IEfficiency`. These are unit evidence only.
  - [Measurements](../specs/archived/windows-version/validation/measurements.md) lists idle CPU and memory of the full app, and EcoQoS on versus off, as not yet measured.

## Non-Obvious Constraints

- Do not reassert process-level efficiency state after startup. Doing so overrides a user-forced Efficiency mode.
