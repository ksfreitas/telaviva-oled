# Changelog

## Unreleased

### Fixed

- Reliable macOS DDC requests for the connected LG ULTRAFINE: transmit requests twice with short spacing, validate complete replies, and confirm the requested brightness or volume through readback before reporting success. A one-point brightness change and restoration were verified on this LG connection.
- Popup brightness and volume sliders now follow the pointer immediately from a local draft, while a bounded background coordinator sends only the latest target to the monitor and reconciles on real DDC completion. Interactive feel on the physical LG is still pending acceptance.
- Keyboard routing now also requires and requests Input Monitoring, re-evaluates permissions when the app is activated or Settings opens (no polling), retries creating the event tap after permission changes, shows each permission's state with its request button disabled once granted, and local ad-hoc builds use an identifier-based designated requirement so permissions survive rebuilds (existing grants must be re-added once).
- Brightness keys that the keyboard sends as plain key codes 144 and 145 are now recognized and routed to the external monitor under the cursor; they were ignored before.
- Hardware brightness and volume keys pressed quickly no longer make the target jump back: the key router now ignores monitor readbacks while its own DDC writes are in flight and for one second afterwards, then compares again, so external changes and monitor clamping are still followed.
- A dimmed monitor that wakes while its DDC channel is not ready is now restored: a failed wake readback or restore write is retried up to three times while ownership of the dimmed value is kept, instead of dropping ownership and leaving the monitor at its dimmed brightness.
- The camera status shown when display sleep suspends monitoring was missing its pt-BR text and now has it.
- The macOS app now builds with older SDKs such as the macos-14 CI runner's: the camera connect and disconnect observers use the raw AVFoundation notification names instead of the `AVCaptureDevice.wasConnectedNotification` members that only newer SDKs provide.

### Changed

- macOS popup height follows the number of monitors and scrolls beyond three; hardware checks are pending acceptance.
- macOS Settings is a larger, resizable window with General, Keyboard and Automatic brightness tabs; hardware checks are pending acceptance.
- Brightness keys pass through to macOS for monitors that follow the MacBook screen brightness; hardware checks are pending acceptance.
- The macOS popup no longer shows an "Applying" label under a brightness or volume slider while a change is in flight; only the failure message ("Could not apply; showing confirmed value") remains. Hardware checks are pending acceptance.
- Lower CPU and energy use of the macOS camera path: a retained camera session (always-on, or the dimmed return check) is set once, right after it starts, to a low frame rate (target 2 fps, snapped to the nearest rate the camera supports; the built-in camera floors at 15 fps) and never changed while it runs, because changing the rate of a running camera restarts its stream and blinks the camera LED, and single-frame sessions keep the device rate; the camera queue runs at background quality of service while the watchdog and settle deadlines are timed on a separate utility queue; the sample timer allows 10% leeway (at most one second) so wakeups can coalesce; the capture preset falls back from VGA to medium and then low for cameras without VGA; inference preprocessing skips interpolation for 640x480 frames and no longer builds output-name strings per detection; ONNX Runtime thread spinning is disabled; release builds use thin LTO with one codegen unit. Sampling cadence, hysteresis, presence semantics and the C ABI are unchanged. Hardware CPU, energy and frame-rate measurements are pending.
- Faster first presence reading after wake on macOS: the first face sample is taken about one second after an eligible display wakes, power returns to AC while battery suspension is on, or presence protection is re-enabled, instead of after a full sampling interval. Stale low-light and busy or unavailable camera status is cleared on resume. Hardware timing is pending acceptance.
- Dark frames in single-frame face checks on macOS now keep the camera on for five seconds and report only the settled sample, so a dark frame taken during auto-exposure no longer starts the low-light cycle. In dark rooms the camera stays on about five seconds longer per single-frame face check. Continuous capture modes keep reporting an insufficient-light frame immediately. Hardware acceptance is pending.
- Default hardware-key step is now 5 points (was 6.25) and Shift+Option moves 1 point (was 1.5625).
- Following the MacBook screen brightness now pauses while the MacBook lid is closed or its built-in screen is off, and resumes by itself when the lid opens or the screen turns on; manual brightness control of the external monitors returns during the pause. The Settings message for an offline built-in display now also covers a switched-off screen. Hardware checks are pending acceptance.
- Presence checking on macOS now stays off while every eligible monitor is at or below the brightness threshold, which defaults to 20%; lower the slider to 10% to keep protection at low brightness. Hardware acceptance is pending.
- Calibration of external monitors against the MacBook screen is inverted: each point now sets the monitor to 25, 50, 75 or 100% of its brightness, the calibration slider sets the MacBook brightness, and the core inverts the saved points when following. Monitors calibrated with earlier development builds need recalibration. Calibration is refused, and a calibration in progress stops, while the MacBook screen is off or the lid is closed. Hardware checks are pending acceptance.
- Ambient calibration points are now added and removed by the user (at least two, kept in ascending order); existing points are kept. Hardware checks are pending acceptance.

### Added

- On macOS, external monitors follow the MacBook screen brightness through per-monitor calibration curves, with a calibration tab; hardware checks are pending acceptance.
- New application icon for macOS: an Icon Composer `.icon` layered for Liquid Glass on macOS 26 and later, with an `.icns` fallback for macOS 14 and 15; vector sources are in `assets/branding/` and regenerate with `tools/branding/build-icons.sh`; Dock and Finder appearance is pending acceptance.
- Presence menu-bar icon on macOS replaces the static monitor symbol: a filled dot when a face is detected, no dot when no face is detected or presence is off, suspended or unavailable, and a dot outline when light is insufficient. The core adds the additive snapshot field `camera.presence`; pending acceptance on a real menu bar.
- Per-monitor OLED protection participation toggle in the popup and in Settings, so each external monitor can opt in or out; hardware checks are pending acceptance.
- Camera selection history on macOS: the app remembers the cameras you select, up to eight, most recent first, and uses the first one that is connected. When none is connected it falls back to the system default camera, and "Use default camera" clears the history. Hardware checks with two webcams are pending.
- Configurable hardware-key step for brightness and volume (1 to 15 points each, default 5) in Settings; Settings window is taller.
- Optional macOS keyboard brightness and volume routing to the supported external monitor under the cursor, with Accessibility status and settings.
- Hardware mute key for the external monitor that is the Mac's sound output: it sets the monitor volume to zero and the next press restores the previous level (emulated through the volume control; the monitor's own mute is not used). Other outputs keep macOS mute.
- Generation-checked manual commands and deterministic keyboard-routing tests covering held keys, repeat accumulation, and stale targets.
- Behavior change: hardware volume keys now adjust an external monitor only while it is the Mac's default sound output (matched by display EDID identifier), instead of the monitor under the cursor, so headphones and other outputs stay under macOS control. Brightness keys still follow the cursor.
- Optional continuous camera capture, suspension while all eligible external displays sleep, and a five-second exposure-settling period for low-light recovery on macOS.
- Deterministic native camera lifecycle tests, display-state policy/ABI coverage, and localized inline camera status wording.
- One-command Apple Silicon macOS build through `scripts/build-macos.sh`, compatible with Sh, Bash, and Zsh.
- Finalized the camera lifecycle decision in ADR 0003 and archived its standalone plan and unrun macOS hardware acceptance checklist.
- Corrected the macOS CI `lipo` invocation to verify the packaged arm64 executable.
- Configurable presence brightness threshold on macOS: a Settings slider (10 to 90%, default 20%) sets the brightness an eligible monitor must be above for presence checking to run; hardware acceptance is pending.

- Baseline project documentation and CodeAdvisor conventions for the planned Rust application on macOS and Windows.

- Shared Rust policy core and versioned C ABI, with automated settings, protection ownership, battery, low-light, and lifecycle checks.
- Initial local YuNet/ONNX Runtime inference crate with pinned model/runtime provenance and CPU fallback.
- Native macOS menu-bar shell and Apple Silicon DDC adapter under integration; hardware acceptance remains pending.
- Core coverage reporting and macOS CI checks. Windows implementation is deferred.

- MIT project license and public GitHub README with macOS build, usage, privacy, compatibility and contribution guidance.
- Git ignore rules for local build outputs, downloaded runtimes, IDE state and local signing material.

- Consolidated the initial feature documentation into ADR 0002, the cumulative architecture overview and `api.md`; archived the original specification, plan and validation history without marking pending acceptance complete.
