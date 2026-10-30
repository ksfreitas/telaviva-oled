# ADR 0041: Lock session on confirmed absence

- Status: Accepted for macOS; Windows implementation and hardware acceptance on both platforms pending
- Date: 2026-10-29
- Feature: [Occlusion-robust face detection, 1% presence-check floor and lock screen on absence](../specs/masked-face-detection-and-lock-on-absence/spec.md)

## Context

TelaViva OLED only dims a display when presence is lost. The user guide and ARCHITECTURE.md state that the application never forces a lock and list locking as outside the trigger scope of brightness protection. The owner has now asked for an opt-in lock after confirmed absence, with a light safeguard so that no lock happens when the camera cannot see reliably.

## Decision

Add a default-off, persisted `lock_on_absence` setting and a user-facing `lock_delay_s` setting, from 1 to 60 seconds, defaulting to 5. The core owns a lock stage with the values Idle, Armed, Requested, Locked and Failed. The stage leaves Idle only while the condition `lock_on_absence && any display presence-dimmed && session active && camera mode face` holds. The stage arms on the absent sample that produced the dim, and it requests a lock on a later absent sample taken at least `lock_delay_s` after arming. Insufficient light, a present sample, an unavailable camera, an inactive session, the option being off or protection being off return the stage to Idle. Insufficient light already clears the dims (ADR 0002), so no lock happens in unfavorable light.

The Locked stage is the one exception to the condition: it ignores the session-inactive part, because the session becomes inactive once the lock takes effect. It stays Locked while the session is inactive, and it returns to Idle when the session becomes active again or when no display is presence-dimmed any more. A lock request is never issued while the session is already inactive.

The core makes no OS calls. The snapshot carries a `lock` block with `requested` and a unique `token`. Each shell locks once per token and answers with the `lock_result` command, where `ok` is true only when the lock is confirmed. A failed result keeps the display dimmed, moves the stage to Failed and does not retry within the same absence episode.

While the session is locked and inactive, the camera is suspended, and it resumes with a first sample about 1 second after unlock. Brightness stays dimmed through the lock, and the first present sample restores it. Keep-awake with presence, when also enabled, keeps its own rule.

On macOS, the lock uses the private `SACLockScreenImmediate` symbol from login.framework, loaded with dlopen and dlsym in one isolated file that follows the pattern of ADR 0007. It fails closed. Confirmation uses the session lock state and the `com.apple.screenIsLocked` notification within 3 seconds. There is no osascript, pmset or keystroke fallback.

On Windows, the lock calls `LockWorkStation` behind an interface in the Platform library. It is confirmed by the `WTS_SESSION_LOCK` session event within 3 seconds.

Both shells report the session lock state whenever keep-awake or the lock option is on.

The feature changes no global OS preference, requires no elevation and adds no network use. The application never unlocks the session and never wakes the system.

## Alternatives

- `CGSession -suspend`: removed in macOS 26.
- `pmset displaysleepnow`: locks only when the password delay is set to immediate.
- AppleScript with Control+Command+Q: requires Accessibility and Automation permissions, and was reported to fail on macOS 26.
- A new `pending_actions` kind: the queue is capped at 8 entries and holds one action per monitor, while a lock applies to the whole session.
- Timers in the shells: would duplicate the stage logic, which must stay in the core.

## Consequences

The macOS lock depends on a private symbol that may disappear in any macOS release. If it does, no lock is performed, a diagnostic line is written, the display stays dimmed, and the limitation is documented. A false absence can lock a user who is still present. The risk is limited by the opt-in default, by two misses plus the delay, by the light gate, and by the detection work of the same feature.

Older binaries reject the new settings fields and reset the settings file, as described in the amendments to ADR 0003. macOS lock-state signals were reported unreliable on macOS 26, so the application reports only confirmed locks. Hardware acceptance on both platforms is still required and is not claimed by this ADR.

Implementation details and validation evidence remain in [architecture.md](../specs/masked-face-detection-and-lock-on-absence/architecture.md), [contracts.md](../specs/masked-face-detection-and-lock-on-absence/contracts.md) and the validation records of that feature.
