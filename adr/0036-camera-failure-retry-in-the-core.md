# ADR-0036: Retry a camera that stops delivering images from the core

**Status:** Accepted
**Date:** 2026-10-23
**Feature:** camera-stuck-busy-after-usb-stall
**Spec:** [Bug plan](../specs/archived/camera-stuck-busy-after-usb-stall/spec.md)

## Context

A `busy` or `unavailable` camera status suspended presence protection until an unrelated event arrived: a display wake, power returning to AC, presence switched on again, a brightness gate change, or a camera connect or disconnect. The core maps both statuses to `suspended` (`core.rs:985-988`), and the shells then stop capture and arm no timer. A USB stream that fails without disconnecting the device triggers none of those events.

On 2026-10-23 a USB webcam produced isochronous stream errors (`e00002ed`). No frame arrived, the app's 5-second watchdog reported `busy`, and no session started again until 17:30. Quitting and reopening the app restored presence protection.

The shells report `busy` whenever no frame arrives in time or the start fails with the device present. The word "busy" suggests that another app holds the camera, and the shells cannot separate that case from other start failures ([ADR 0022](0022-windows-presence-camera-shared-mediacapture.md)). Nothing about the failure reached the logs either: macOS wrote the camera lines at debug level, which the system does not persist, and the Windows log and the core ring had no camera line.

The core reads no clock ([ADR 0014](0014-camera-lifecycle-in-the-core.md)), so it cannot schedule a retry by itself. The shells already obey a delay the core provides, `next_sample_in_ms` ([ADR 0030](0030-short-confirmation-wait-after-first-no-face-check.md)), so a retry delay follows the same pattern.

## Decision

- **The core owns the retry policy.** The core counts consecutive `busy` and `unavailable` reports in `camera_failures`. The counter is runtime only and is never persisted. Each such report, from the `camera_status` command or from an `unavailable` presence observation, adds one.
- **Backoff.** The snapshot field `camera.retry_in_ms` is 30 s after the first failure, 60 s after the second, and 300 s from the third on. The values are the constant `CAMERA_RETRY_MS`, next to `RESUME_FIRST_SAMPLE_MS` in `core.rs`.
- **Null in every other state.** `retry_in_ms` is null unless the camera status is `busy` or `unavailable`, presence protection is on and the core is open. `permission` and `error` never get a retry: they need the user to act, and a timer would only repeat the failure.
- **One timer in each shell.** While `retry_in_ms` is not null, a shell arms one one-shot timer for the current camera generation. When it elapses, the shell sends the existing `camera_status` command with status `unknown` and the current generation, the same message the shells already send after a device change. The core then sets `resume_pending`, so the next sample comes after `RESUME_FIRST_SAMPLE_MS` (1 s) instead of the normal interval, and the normal sample path probes the camera. A failed probe reports `busy` again, which adds to the counter, so the next wait is longer.
- **Timer rules.** Other snapshots of the same generation do not postpone a pending timer. A null `retry_in_ms`, a change of generation or mode, or shutdown cancels the timer, or re-arms it for the new generation.
- **Resets.** The counter returns to zero when an accepted presence sample sets the status to `ready`, and on every resume event (`begin_camera_resume`): a display wake, power returning to AC, presence protection being re-enabled, and a brightness gate change.
- **Diagnostics.** The core writes two lines to its diagnostics ring, which both shells drain ([ADR 0034](0034-local-diagnostic-logs.md)). `camera_failure status=<busy|unavailable> failures=<n> retry_in_ms=<n>` is written when a `busy` or `unavailable` status is reported. `camera_retry failures=<n>` is written when `busy` or `unavailable` goes back to `unknown`, either by the timer or by a device change. Neither line carries a device identifier or a camera frame. The shells write them at notice level, to the macOS unified log and to `telaviva.log` on Windows.
- **Label.** `cameraStatus.busy` reads "no image" in en-US and "sem imagem" in pt-BR on macOS and Windows, in place of "busy" and "ocupada". The core status code stays `busy`, so the contract does not change.
- **Contract.** `camera.retry_in_ms` is an additive snapshot field. ABI version stays 1. There is no new setting, dependency or network use.

## Alternatives Considered

- **Retry in each shell with its own backoff.** Rejected: the policy would exist twice and drift between macOS and Windows. In the core it is tested once.
- **The core arms the timer.** Rejected: the core reads no clock (ADR 0014). The shells keep the clock and the timers.
- **Probe on every snapshot while busy.** Rejected: each probe starts the camera, lights its LED and uses energy, so the probe rate has to be bounded.
- **One fixed interval, for example 30 s.** Rejected: a camera that stays gone would be probed twice a minute for as long as it stays gone. Growing waits, capped at 5 minutes, keep that cost small.
- **Retry `permission` and `error` too.** Rejected: see the decision above. A retry would only repeat the same failure.
- **Keep the label "busy" and explain it in the guide.** Rejected: the label is what the user reads, and it names a cause the shells cannot see.
- **A new core status for a stalled stream.** Rejected: it would change the status contract for a label problem, while the retry needs only the existing statuses.

## Consequences

- A stalled camera recovers without a restart. Presence protection resumes at the first accepted sample after a successful probe.
- Each stall still costs the 5-second watchdog and the wait before the first probe, so protection pauses for at least about 35 seconds each time.
- Cost: each retry starts one camera session and takes a few frames. After the third failure the wait is 5 minutes, so a camera that stays gone is probed at most about twelve times an hour. That 5-minute floor is why the LED and battery cost stays small. The cost is not measured.
- Each retry adds two lines to the bounded diagnostic ring ([ADR 0034](0034-local-diagnostic-logs.md)).
- `permission` and `error` still need user action, as before.
- The label now describes what the shells can observe. "no image" covers a stream that stops, a start that is slow, and a start that fails with the device present. It does not claim that another app holds the camera.
- **Not fixed: the hardware stall itself.** USB isochronous errors come from the device, its cable, the hub or the bus, outside the app. This change makes the app recover from a stall. It does not prevent the stall.
- **Hardware verification was marked complete by the owner on 2026-10-27, with no values recorded.** Automated checks cover the core policy and the shells' decoding. The macOS timer in `PresenceCamera.swift` is covered only by a manual check, because no Swift test compiles it. The Windows driver and codec changes build and pass the Windows tests. This decision does not claim that the retry recovers a real stalled camera on either platform beyond what was recorded.
- **Out of scope, unchanged.** `error` is cleared only by a camera change, presence switched off or battery. The Windows driver ignores a stream that produces only warm-up frames. An ABI fault is still reported as `busy`, so it now reads "no image".

## Non-Obvious Constraints

- The retry timer belongs to one camera generation. A timer that fires after a generation change is dropped, and the new generation decides whether it needs a timer.
- Only a null `retry_in_ms`, a generation or mode change, or shutdown changes a pending timer. Other snapshots must not, or a steady stream of snapshots would push the retry back indefinitely.
- The counter counts reports, not time. A probe that fails at once still moves the next wait to the next, longer step.
- The core sets `resume_pending` whenever `unknown` replaces `busy` or `unavailable`, including the `unknown` sent after a device change. Such a sample therefore also comes after 1 s.
- The two diagnostic event names are a contract. Renaming them breaks the `log show` commands and the grep patterns in the user guide ([ADR 0034](0034-local-diagnostic-logs.md)).
