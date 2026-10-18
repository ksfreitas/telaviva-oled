# Changelog

## Unreleased

### Fixed

- The Windows Settings toggles now sit flush with the right edge of their cards instead of floating about 100 px to the left, in line with the sliders, number boxes and combo boxes.

### Changed

- The `specs/oled-monitor-protection/` folder is removed. It held only three short pointers to the archived record of the initial application, which stays in `specs/archived/external-monitor-control-and-oled-presence-protection/`; the project map in `AGENTS.md` now points at `specs/archived/`.
- The README is now a short entry point (highlights, platform support, status, install, getting started, privacy, build and contributing). The detailed behavior of each feature moved to the new `docs/user-guide.md`, and the README no longer lists the hardware of the maintainer's test machines; those results remain in the validation reports.

## 0.1.0 - 2026-10-17

First release.

### Fixed

- Reliable macOS DDC requests for the connected LG ULTRAFINE: transmit requests twice with short spacing, validate complete replies, and confirm the requested brightness or volume through readback before reporting success. A one-point brightness change and restoration were verified on this LG connection.
- Popup brightness and volume sliders now follow the pointer immediately from a local draft, while a bounded background coordinator sends only the latest target to the monitor and reconciles on real DDC completion. Interactive feel on the physical LG is still pending acceptance.
- Keyboard routing now also requires and requests Input Monitoring, re-evaluates permissions when the app is activated or Settings opens (no polling), retries creating the event tap after permission changes, shows each permission's state with its request button disabled once granted, and local ad-hoc builds use an identifier-based designated requirement so permissions survive rebuilds (existing grants must be re-added once).
- Brightness keys that the keyboard sends as plain key codes 144 and 145 are now recognized and routed to the external monitor under the cursor; they were ignored before.
- Hardware brightness and volume keys pressed quickly no longer make the target jump back: the key router now ignores monitor readbacks while its own DDC writes are in flight and for one second afterwards, then compares again, so external changes and monitor clamping are still followed.
- A dimmed monitor that wakes while its DDC channel is not ready is now restored: a failed wake readback or restore write is retried up to three times while ownership of the dimmed value is kept, instead of dropping ownership and leaving the monitor at its dimmed brightness.
- The camera status shown when display sleep suspends monitoring was missing its pt-BR text and now has it.
- The macOS app now builds with older SDKs such as the macos-14 CI runner's: the camera connect and disconnect observers use the raw AVFoundation notification names instead of the `AVCaptureDevice.wasConnectedNotification` members that only newer SDKs provide.
- Release builds of the macOS Xcode project link the `aarch64-apple-darwin` release Rust libraries under `target/aarch64-apple-darwin/release/` instead of the debug libraries in `target/debug/`; Debug still links `target/debug/`, and the packaging README lists the Rust build prerequisites for the Xcode project.
- C header documentation is corrected: `tv_inference_detect_bgra` no longer lists `3` (unavailable), which only the create functions return, `tv_inference_create` lists its real return codes, and `tv_submit` documents when it returns `TV_CLOSED` after shutdown. These are comment-only changes with no ABI change.
- Live documentation no longer points to the navigation stub `specs/oled-monitor-protection/architecture.md` as the design; it links to the permanent documents, `api.md`, the ADRs and the archived originals.

### Changed

- The inference crate compiles its CoreML provider code only for macOS builds, so the Windows build does not depend on CoreML; the provider list and selection rules are unchanged on macOS.
- Live documentation (README, ARCHITECTURE and api.md) describes Windows as an implementation in progress with a Windows 11 x64 target, with its hardware status limited to recorded checks.
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
- Camera session timing moves from the macOS shell into the Rust core: session retention, the acquisition and assessment deadlines, the warm-up and dark-settle windows, and the stale-callback check now run in `telaviva-core` behind the additive `TvCameraLifecycle` C ABI (`tv_camera_lifecycle_create`, `tv_camera_lifecycle_step` and `tv_camera_lifecycle_destroy`); ABI version stays 1. Swift keeps the timers, the monotonic clock and AVFoundation. Camera hardware acceptance is pending.
- The core snapshot no longer carries the `messages` field, and the core's `.ftl` locale catalogs (`locales/en-US.ftl` and `locales/pt-BR.ftl`) are removed: each native shell owns its UI strings, while the core keeps the `locale` setting and returns status codes. `os_locale` is still accepted by `tv_create` but no longer affects snapshot content.
- The Windows version shown in About is read from the core instead of a hard-coded copy, and the macOS `Info.plist` gains `CFBundleVersion`.
- The README and the packaging README now describe the update check as the app's only network request.

### Added

- Windows 11 x64 tray application under `native/windows/`: a flyout with a card per external monitor, a Settings window with General, Keyboard and Automatic brightness pages in English (US) and Portuguese (Brazil), and a per-user zip with install and uninstall scripts; the Windows status and its hardware checks are in the README.
- Windows DDC monitor control through dxva2, with readback verification of each write. A read of brightness and volume on one Dell S3225QC over HDMI passed; writes and restoration are not yet hardware-verified.
- Windows presence protection with local YuNet inference on the CPU, screensaver protection, battery and display-sleep suspension, and camera selection that hides infrared cameras; hardware checks are pending.
- Windows Laptop brightness follow (Notebook in pt-BR) with user-managed calibration points, lid and panel pause, and restore of Windows adaptive brightness; disabled with a stated reason on desktops. Hardware checks are pending because no laptop was available.
- Windows opt-in volume and mute keys for the external monitor that is the default sound output; brightness keys are not intercepted.
- Windows Efficiency mode handling: the app requests EcoQoS while idle in the background and does not override a user-forced Efficiency mode.
- Windows launch at login through the per-user `Run` registry value, off by default.
- Inference C ABI additions, with no change to the core ABI version: provider value `2` (DirectML candidate), `tv_inference_create_with_provider_device` and `tv_inference_directml_available`. DirectML is off by default because the acceleration allowlist is empty; provider value `3` is reserved for a future NPU provider and is not accepted.
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
- Header contract tests: `crates/telaviva-core/tests/header_contract.rs` and `crates/telaviva-inference/src/header_contract_tests.rs` check `include/telaviva_core.h` and `include/telaviva_inference.h` against the Rust ABI (prototypes, struct fields, constants and opaque types), so header drift fails `cargo test` without a new dependency.
- About page on macOS and Windows with the application icon, version, GitHub project link, credits, and English (US) and Portuguese (Brazil) text, reachable from Settings and from the tray or menu bar icon.
- Update check against the GitHub releases API that runs only when the About page is open or Check now is pressed, informs the user only, and never downloads an update.
- Two additive core C ABI exports, `tv_core_version` and `tv_update_evaluate`, with the core ABI version still at 1.
- Version consistency test that fails when the Cargo, inference crate, Info.plist or app.manifest version differs from the core version.

- Baseline project documentation and CodeAdvisor conventions for the planned Rust application on macOS and Windows.

- Shared Rust policy core and versioned C ABI, with automated settings, protection ownership, battery, low-light, and lifecycle checks.
- Initial local YuNet/ONNX Runtime inference crate with pinned model/runtime provenance and CPU fallback.
- Native macOS menu-bar shell and Apple Silicon DDC adapter under integration; hardware acceptance remains pending.
- Core coverage reporting and macOS CI checks. Windows implementation is deferred.

- MIT project license and public GitHub README with macOS build, usage, privacy, compatibility and contribution guidance.
- Git ignore rules for local build outputs, downloaded runtimes, IDE state and local signing material.

- Consolidated the initial feature documentation into ADR 0002, the cumulative architecture overview and `api.md`; archived the original specification, plan and validation history without marking pending acceptance complete.
