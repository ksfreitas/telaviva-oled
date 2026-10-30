# Changelog

## Unreleased

## 0.1.5 - 2026-10-29

### Added

- Lock the session after confirmed absence, an opt-in option in Settings that is off by default. When the screen has been dimmed for an absent user, the session is locked after a delay of 1 to 60 seconds (5 by default), if a confirming check still finds no face and the light is sufficient. The lock is requested once and is reported only when the system confirms it; a failed lock keeps the screen dimmed and is logged. The camera pauses while the session is locked and resumes after unlock. It is implemented on macOS only so far, Windows is pending, and it uses a private macOS interface that a future macOS release may remove. Hardware checks are pending.
- A developer-only benchmark harness for face detection models, off by default and built only with the `bench` cargo feature of `telaviva-inference` (`examples/occlusion_bench.rs`). It is not part of the shipped libraries or the app.

### Changed

- macOS uses the RetinaFace MobileNet0.25 face detection model (see ADR 0042), which finds faces behind masks and with a hand over the mouth or nose; Windows still uses YuNet until its port. Hardware checks are pending.
- The presence brightness threshold, global and per display, can be set from 1 percent (it was 0 percent). A stored 0 is raised to 1 when the settings load, and the other settings are kept. Hardware checks are pending.
- The return check interval defaults to 0.5 second (it was 1 second). The confirming check before a dim uses the same interval, so the wait before a display is dimmed is shorter too. A saved value is kept. Hardware checks are pending.
- Upgrade note: a build older than this one resets its settings when it reads a `settings.json` that contains `lock_on_absence` or `lock_delay_s`, as it already does for earlier additive settings. It renames the file to `settings.json.corrupt` and starts from defaults.

## 0.1.4 - 2026-10-27

### Added

- Keep awake while a face is detected on macOS and Windows, off by default, in Settings > General: continuous presence monitoring keeps the display and computer active while a face is detected, independently of brightness protection, battery preferences and display brightness. The first no-face result or unavailable detection releases the inactivity requests and restores normal operating-system behavior. Manual lock, screensaver and sleep actions take precedence. On macOS, posting activity may need a permission, and Settings shows an action to grant it. On Windows it holds a system and display request and sends one zero-displacement mouse move per second, with local checks for locked, secure-desktop, display-off, suspend and screen-saver states; no permission prompt is shown, and Settings says so when Windows blocks simulated input. The option and its status are available in English and Portuguese.
- Laptop screen protection on Windows: the built-in panel appears in the popup and in Settings > General as "Laptop" (Notebook in Portuguese), with a protection switch, a brightness slider and a Protection brightness slider, and no volume control. It dims when no face is detected and when the screensaver starts, and it is restored afterwards.
- An option, off by default, that turns off "Change brightness automatically when lighting changes" in the active power plan while the Windows laptop panel is protected, for AC and DC power. The app restores the setting afterwards, unless you turned it on during protection, and it recovers the setting and the panel brightness at the next launch after a crash.
- A Monitored display selector on Windows, with Automatic (the main external display, otherwise the laptop panel), the remembered displays and the laptop panel as choices. The camera follows the monitored display.
- A presence brightness threshold saved for each display on Windows, with a range of 0 to 90 percent. Hardware checks are pending.
- On Windows, the built-in camera is ignored while the lid is closed, as if it were disconnected. The camera choice is kept, and the built-in camera is used again when the lid opens.

### Changed

- Monitor sliders on macOS and Windows use aligned brightness, speaker and protection shield icons, with explanatory tooltips in English and Portuguese and accessible slider help.
- Protection brightness on Windows is a fixed level of 1 to 50 percent for each screen, used for both presence and the screensaver, with a default of 1 percent. Hardware checks are pending.
- Keep camera on is now on by default and moved to Settings > Advanced on macOS and Windows, with a note that turning it off can cause detection problems, depending on the camera, and "insufficient light" events. Hardware checks are pending.
- Upgrade note: existing installs keep their saved value; turn the option on in Settings > Advanced.
- The camera status "busy" (Portuguese "ocupada") now reads "no image" (Portuguese "sem imagem") on macOS and Windows. It is shown when no frame arrives in time or the camera fails to start while the device is present, not when another app holds the camera. Hardware checks are pending.
- The macOS Settings window is now a fixed size (760 by 720 points) and can no longer be resized; content that does not fit scrolls.

### Fixed

- Fix update checks for existing release tags with a fourth numeric version component, such as `v0.1.3.1`, instead of reporting an invalid response.
- Keep macOS protection-brightness sliders responsive while settings snapshots arrive asynchronously, preserving the selected 1–50% target.
- Keep local presence monitoring active while keep-awake posting is paused for screensaver/display state or an awake clamshell session. Correct camera suspension reporting so a blocked session is not described as low display brightness.
- Resume macOS presence monitoring when keep-awake is enabled and the local session is unlocked but the core still holds an inactive session state after a lock or wake transition.
- Calibration crash recovery on Windows now restores the external monitor's brightness once the monitor is available again, for up to 90 seconds after launch, as on macOS. Hardware checks are pending.
- The camera is retried after it stops delivering images, after 30 s, then 1 min, then every 5 min, on macOS and Windows. Before, a camera left "busy" or "unavailable" stayed off until the app was restarted. Presence protection resumes by itself when the camera answers.

## 0.1.3 - 2026-10-21

### Added

- Laptop screen protection on macOS and Windows: the built-in screen appears in the popup and in Settings > General as "MacBook screen" on macOS and "Laptop" on Windows, with a protection switch, a brightness slider and a Protection brightness slider, and no volume control. It dims when no face is detected and when the screensaver starts, is restored afterwards, and is hidden while the lid is closed.
- An option, off by default, to turn off the system's automatic brightness while the laptop screen is protected: "Automatically adjust brightness" on macOS, and "Change brightness automatically when lighting changes" in the active power plan on Windows. The app turns it back on afterwards unless you turned it on during protection, and after a crash it recovers the setting and the screen brightness at the next launch. On macOS it uses private interfaces that are not verified on hardware yet.
- A Monitored display selector on macOS and Windows: Automatic (on macOS, the main external display, then the other external displays, then this Mac; on Windows, the main external display, otherwise the laptop screen) is the default, or you can choose a display. The choice is remembered by priority, like the camera, up to eight displays. The camera follows the monitored display, or the next awake display when it sleeps.
- The presence brightness threshold is saved per display on macOS and Windows, with a range of 0 to 90 percent (it was 10 to 90 percent on Windows). A display without its own value uses the global value.
- On macOS and Windows, the built-in camera is ignored while the lid is closed, as if it were disconnected. The camera choice is kept, and the built-in camera is used again when the lid opens.

### Changed

- Protection brightness is a fixed level of 1 to 50 percent for each screen on macOS and Windows, used for both presence absence and the screensaver. The default is 1 percent. Before, the level applied to the screensaver only on macOS, with a default of 0, and presence absence dimmed to 0 on Windows.
- The camera follows the gate display (the monitored display, or the next awake display when it sleeps) and also runs while any display is dimmed by presence. It was any protected display above the threshold.
- Calibration and laptop screen protection share one owner of the system's automatic brightness on macOS and Windows, so the setting is restored when the last of them ends, unless you turned it on in the meantime. Calibration is refused while protection holds the laptop screen.
- Upgrade note: the laptop screen is protected by default on macOS and Windows, and its protection switch turns it off.
- Upgrade note: a build older than this one treats a `settings.json` written by this version as corrupt and resets its settings, as it already does for earlier additive fields.

### Fixed

- Calibration crash recovery now restores the external monitor's brightness once the monitor is available again, for up to 90 seconds after launch. It was lost before. Hardware checks are pending.
- A monitor that protection dimmed gets its previous brightness back, rounded to a whole unit, when it is dropped and rediscovered (or rediscovered in place) under the same ID at the dimmed level. Before, the dimmed level became the new baseline, so the monitor stayed dimmed. At another level that level is kept, and under another ID the previous brightness is not restored. Shared by macOS and Windows; hardware checks are pending.
- A brightness or volume set from a slider is stored as a whole device unit. Before, a fractional value (for example a device value of 45.15) never matched the whole number the monitor reports, so the app re-read and rewrote the monitor about every second, costing CPU and DDC traffic. Shared by macOS and Windows; hardware checks are pending.

## 0.1.2 - 2026-10-19

### Added

- Always-on local diagnostic logs of brightness decisions and DDC attempts, with monitors as hashed ids: the macOS unified log (subsystem `com.telaviva.oled`) and a rotating `telaviva.log` of at most about 2 MiB on Windows. Core ABI: `tv_diagnostics_drain`, additive to ABI version 1.

### Changed

- The accelerated self-test latency budget is now 250 ms on macOS and Windows (it was 125 ms), to leave headroom for a cold CoreML compile.

### Fixed

- An external monitor dimmed by protection no longer stays at its dimmed brightness when its DDC answers slowly after wake: DDC attempts are retried for up to 30 s after wake before the restore is given up, on macOS and Windows. Hardware checks are pending.
- The Windows debug trace no longer writes the monitor id, which contained the EDID serial.
- On macOS, Automatic, Neural Engine and GPU always fell back to the CPU since 0.1.1 (Settings showed "In use: CPU", reason `session_failed`): a passing accelerator self-test was treated as a failure by the validator wiring. The self-test result is now used as is. The latency budget was not the cause. A hardware check of the corrected build is pending.

## 0.1.1 - 2026-10-18

### Verified

- Ran the camera lifecycle acceptance checklist in part on a MacBook Pro with a Dell S3225QC: ten of sixteen scenarios passed, one passed in part and five were not run (see the archived checklist).

### Added

- Inference device setting on the new Advanced page of Settings on Windows and macOS. Windows offers Automatic (the default), CPU, or a specific GPU listed by name and kind. Automatic uses an integrated GPU when it passes a check (session creation, matching presence classification on the bundled self-test frame, and a response within 125 ms), and CPU otherwise; a discrete GPU is only selected by hand. macOS offers Automatic, Neural Engine preference, GPU preference and CPU. Automatic requests all CoreML compute units after hardware validation; each session validates locally, and failures fall back to CPU with a same-frame retry after runtime errors. CoreML choices are preferences, not guarantees of physical operator placement. Both pages show the active route and explicit-choice fallback notices in English and Portuguese. Changes apply without restarting. No energy benefit is claimed.
- Settings schema 1 gains the optional `inference_device` string (default `auto`), accepted as `auto`, `cpu`, `ane`, `gpu` or `gpu:<vendor>:<device>:<ordinal>`; the core ABI version stays 1. A build without this field treats a settings file that contains it as corrupt, as with earlier additive fields.
- Windows adapter classification through DXCore (`IsIntegrated` matched to DXGI by LUID), which does not create a device and so does not wake a discrete GPU.
- The app bundles the inference self-test frame and its attribution.
- Core controls handle ABI: `tv_controls_create`, `tv_controls_step` and `tv_controls_destroy` own manual-control drafts, key policy and DDC dispatch for brightness and volume. The change is additive to ABI version 1, which stays `1`.

### Fixed

- The Windows Settings toggles now sit flush with the right edge of their cards instead of floating about 100 px to the left, in line with the sliders, number boxes and combo boxes.
- Widen the macOS Settings window to 760 points by default and at minimum so all tabs fit.
- On Windows the manual controls compared the raw monitor value with the slider percentage; they now use the normalized percentage, which differs on monitors whose DDC range is not 0 to 100.
- The monitor now dims about one sampling interval after the face leaves instead of two: the confirming no-face check runs after the short return interval.
- Windows and macOS: DDC control recovers by itself after another monitor is connected or the display topology changes, without restarting the app.

### Changed

- The Windows package now ships the DirectML build of ONNX Runtime 1.20.1 as its only `onnxruntime.dll` (about 3 MB larger than the CPU build). It loads and runs the CPU provider even where DirectML is missing, and `DirectML.dll` is still not redistributed. The inference-device change no longer needs a restart because there is one runtime.
- The macOS app bundles official ONNX Runtime 1.23.0, retaining the Rust bindings and Windows 1.20.1 runtime. This supports CoreML compute-unit string options and avoids the 1.22.0 process-exit mutex abort reproduced during the integration spike. Local builds verify cached runtime hashes and refresh outdated copies.
- The `specs/oled-monitor-protection/` folder is removed. It held only three short pointers to the archived record of the initial application, which stays in `specs/archived/external-monitor-control-and-oled-presence-protection/`; the project map in `AGENTS.md` now points at `specs/archived/`.
- The Windows acceleration profile allowlist (`acceleration-profiles.toml`, its parser and its tests) is removed. DirectML is no longer gated by a recorded energy measurement and the "accelerated must be faster than the CPU" rule is replaced by the 125 ms latency check above. Failure markers remain, for Automatic only.
- The README is now a short entry point (highlights, platform support, status, install, getting started, privacy, build and contributing). The detailed behavior of each feature moved to the new `docs/user-guide.md`, and the README no longer lists the hardware of the maintainer's test machines; those results remain in the validation reports.
- Remove the About button from the macOS menu-bar popup; About remains available in Settings.
- The Windows popup no longer shows the face-detection status line and its camera icon, so it matches the macOS popup. The footer camera status line ("Camera: <status>") is unchanged.
- Windows manual controls and hardware keys now run on the core controls handle. Hardware checks are pending.
- DDC writes on Windows are now retried (up to three attempts), and a rejected result submission is resubmitted at most three times instead of re-running the write on every snapshot.
- macOS manual controls, hardware keys and DDC dispatch now run on the core controls handle, as on Windows; the Swift queue and retry code are removed, and `tools/tests/test-controls-bridge.sh` covers the Swift bridge. On macOS each DDC packet is still sent twice inside one primitive, as before; the core's separate duplicate write applies to GSM monitors only. A rejected result is resubmitted at most three times instead of being dropped. Hardware checks are pending.
- Fewer duplicated snapshot reads: `tv_submit` no longer builds a full snapshot to read the revision, macOS no longer refreshes twice for one accepted command and ignores a snapshot whose revision was already published, and the Windows bridge worker skips a revision it already published. The snapshot content and the C ABI are unchanged. The Windows part is not yet built or tested.

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
