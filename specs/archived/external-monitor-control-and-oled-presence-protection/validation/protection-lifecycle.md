# Protection lifecycle source review

Date: 2026-10-16. This review traces current Rust policy and the macOS adapter. It records code-reviewed behavior separately from hardware acceptance; it does not authorize or perform monitor writes or camera access.

## Code-reviewed behavior

- Screensaver state starts as `unknown`; `MenuBarController` explicitly submits `unknown` during startup, then receives start/stop notifications. The core applies screensaver protection only for `active`, so unknown startup state does not trigger a dim action.
- Brightness observations preserve a raw DDC current value and the device maximum. The adapter reports normalized `value` separately from raw `raw`; Rust maps normalized user controls into the device range, and DDC actions/readbacks carry raw values. Restoration therefore uses the saved raw baseline rather than a normalized percentage.
- The core marks brightness as app-owned only after a successful brightness write. Before restoring an owned value it requests a fresh read and restores only if the current raw value still matches the expected dim target. A changed value updates the baseline and clears ownership, preserving an external/manual change. Shutdown clears protection reasons and queues the same read-before-restore check for owned brightness.
- `tv_shutdown` submits the shutdown policy; the macOS termination flow stops camera acquisition, begins core shutdown, polls pending hardware actions, and allows up to eight seconds for them to finish. If actions remain, it logs that termination proceeds with actions pending. Thus restoration is attempted when possible, but process exit or failed hardware communication can prevent completion.

## Hardware verification status

No monitor write, readback, or restoration was performed for this review. Source behavior does not establish DDC support or successful physical restoration for any display/connection. Existing core contract tests exercise policy ownership and shutdown with simulated hardware results; they are not physical acceptance tests.

## Outstanding acceptance checks

- On supported external displays, verify raw readback, successful dim, matching read-before-restore, and preservation of a manual brightness change while dimmed.
- Exercise exit with pending writes, failed readback, failed restore, disconnect/reconnect, and the eight-second termination deadline; confirm status is not shown as restored after failure.
- Exercise screensaver startup while inactive, active, stopped, and unavailable, including overlap with presence protection.
- Verify the native app reports accurate operational state for actual DDC behavior and that no hardware writes occur during this source review.
