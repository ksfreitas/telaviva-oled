# ADR-0003: Retain eligible camera sessions and suspend on display sleep

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** camera-lifecycle-and-display-suspension
**Spec:** [Archived implementation plan](../specs/archived/camera-lifecycle-and-display-suspension/plan.md)

## Context

The macOS presence adapter previously used one-frame capture for ordinary checks and retained a camera session only for return checks. Low-light recovery also needed an exposure-settling period, while camera acquisition needed to stop when every eligible external display was asleep. These changes had to preserve existing eligibility, power, error, shutdown, privacy, and brightness-ownership rules. macOS-reported display sleep may not detect a monitor manually powered off while its connection remains logically active.

## Decision

Add an opt-in `always_on_camera` setting, defaulting to false. When enabled, retain a healthy camera session between scheduled checks without increasing inference frequency. Keep capture subordinate to shared core eligibility and suspend it when all eligible external displays are confirmed asleep. Track per-monitor display state with connection generations, suppress DDC writes to sleeping targets, and reconcile after wake using fresh brightness readback and existing ownership checks.

For low-light recovery, retain the 10-second retry delay, then ignore frames during a five-second monotonic exposure-settling window before assessing a fresh frame. Keep the two usable-light threshold and independent bounded acquisition watchdog. Warmup frames do not enter inference or absence counting. The feature remains macOS-scoped; Windows work is deferred.

## Alternatives Considered

- Keep one-frame sessions for all ordinary checks: this remains the default and is selected when the opt-in setting is off.
- Suspend whenever any external display sleeps: this would stop monitoring even when another eligible display remains awake; the selected policy suspends only when all eligible displays are confirmed asleep.
- Treat brightness zero as display sleep: brightness is not a reliable display-power signal, so the implementation uses OS-reported sleep state.
- Count warmup frames toward presence or absence: this could make exposure transitions produce invalid observations, so warmup frames are ignored.

## Consequences

The setting and display-state contract are additive to settings schema 1 and ABI version 1. Keeping the session open does not change inference cadence. Automated Rust and native fake-session checks can verify policy and scheduling without physical devices; actual camera behavior, display sleep reporting, DDC suppression and restoration, and energy use remain hardware acceptance work. The physical checklist is archived with status not run. Existing setting files default the new option to off; older binaries may reject the newly serialized field.

## Non-Obvious Constraints

- Only confirmed asleep state for every eligible external display suspends monitoring; unknown state does not count as asleep, and one awake eligible display permits monitoring.
- Brightness zero does not establish physical display sleep or guarantee OLED protection.
- A manually powered-off monitor may remain logically awake according to macOS and its connection.
- Five seconds of exposure settling follows the existing 10-second low-light retry delay; it is distinct from the five-second acquisition timeout.
- Camera frames remain local and outside the core ABI. Windows implementation and validation remain deferred.
