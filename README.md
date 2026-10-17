# TelaViva OLED

TelaViva OLED is an open-source desktop app for controlling external monitor brightness and volume, with optional OLED brightness protection. External monitors can optionally follow the brightness of a MacBook's built-in display. Its shared policy and inference code is written in Rust; the macOS desktop shell uses SwiftUI and AppKit on Apple Silicon. A Windows 11 x64 shell (WinUI 3) is in the repository; its status is described in the [Windows](#windows) section.

Presence protection detects any face locally. It does not identify people, enroll faces, save webcam images, or send frames to a service. The default presence sampling interval is 10 seconds (configurable from 5 to 300 seconds); while dimmed, return checks default to 1 second (0.25 to 5 seconds). Presence protection is off until enabled, and pauses on battery by default. The camera stays open during return checks, while inference still follows the configured interval. Normal presence checks use one-frame sessions unless **Keep camera on** is enabled (off by default). This option retains capture between checks without increasing inference frequency. Capture still stops when presence protection is disabled, battery suspension applies, no eligible monitor remains, all eligible external displays sleep, every eligible monitor is at or below the brightness threshold (20% by default), or the app exits. Insufficient light suspends face detection instead of counting as absence: recovery waits 10 seconds, keeps capture running for five seconds to allow exposure adjustment, then checks a fresh frame. Two usable-light checks are required before face detection resumes.

## Status

The project is under active development. Rust tests, native macOS builds, and an ABI smoke test have passed. A read-only DDC check has read brightness and volume on one Dell S3225QC connected to an M4 Pro. A connected LG ULTRAFINE has also passed a one-point brightness change and restoration through the application's Rust/native command path, with fresh physical readback; see the [DDC fix and hardware evidence](specs/archived/external-monitor-hardware-keys-and-fluid-controls/lg-ultrafine-ddc-brightness-writes.md). This does not establish support across monitors, ports, adapters, docks, or Apple Silicon generations. Broader camera, physical write/restoration, multi-monitor and release-distribution acceptance remains in progress. See the [implementation plan](specs/archived/external-monitor-control-and-oled-presence-protection/plan.md) and [validation reports](specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md) for evidence and outstanding work. Following the MacBook brightness, calibration and per-monitor protection have passed automated tests only; hardware acceptance is pending, see the [hardware checklist](specs/archived/macbook-brightness-follow/validation/hardware-checklist.md). The Liquid Glass app icon rendering in the Dock and Finder and the menu-bar presence icon are pending acceptance on real macOS hardware.

The macOS app targets Apple Silicon and macOS 14 or later. Development builds have been exercised on macOS 27; this is not a tested compatibility matrix. Intel Macs are deferred. DDC support depends on the monitor and connection.

## Windows

The Windows version is a Windows 11 x64 tray application. A native WinUI 3 shell sits over the same shared Rust core and C ABI as the macOS app. It is implemented in the repository and covered by automated tests. Most hardware checks have not been run, so the status below lists only recorded results. Windows 10 and ARM64 are not supported yet.

### What the Windows version provides

- A tray icon with a flyout: a card per external monitor with brightness and volume sliders, the presence status, Settings and Quit.
- A Settings window with General, Keyboard and Automatic brightness pages, in English (US) and Portuguese (Brazil).
- DDC/CI control of external monitors through the Windows monitor API (dxva2), with readback verification of each write.
- Presence protection, off until enabled: local face detection (any face, no identification, no enrollment) with YuNet through ONNX Runtime on the CPU. Two valid no-face checks dim the monitor, and a returning face restores it. Low light suspends detection instead of counting as absence.
- Optional screensaver protection.
- Battery suspension: presence checking pauses on battery by default. It also pauses while every eligible external display is asleep.
- Camera selection with a history of up to eight cameras. Infrared cameras are hidden from selection.
- Laptop follow: on a laptop with a controllable internal panel, external monitors follow the panel brightness through calibration points you manage. The panel is called **Laptop** in English and **Notebook** in Portuguese. Laptop follow is disabled with a stated reason on desktops. It pauses while the lid is closed or the panel is off. Calibration turns Windows adaptive brightness off and restores it afterwards, including after a crash at the next launch.
- Hardware keys (opt-in): the volume and mute keys adjust an external monitor only while it is the default sound output. Brightness keys are not intercepted and keep their normal Windows behavior.
- Efficiency mode: the app requests Efficiency mode (EcoQoS) while it is idle in the background, and it leaves a user-forced Efficiency mode in place.
- Launch at login, off by default, through the per-user `Run` registry value.

### Verified status

Rows marked Pass in [the Windows hardware checklist](specs/archived/windows-version/validation/hardware-checklist.md) were run on one machine: Windows 11 Pro build 26300, an AMD Ryzen 7 9800X3D desktop with an AMD Radeon iGPU and an NVIDIA RTX 4080 SUPER, and one Dell S3225QC connected over HDMI. The machine has no laptop panel, no battery and no NPU.

- The Dell S3225QC is listed with a stable monitor identifier.
- DDC read of brightness and volume on the Dell S3225QC over HDMI.
- CPU inference on the test face fixture: 5.27 ms mean, face present, confidence 0.822190 (see the [Windows measurements](specs/archived/windows-version/validation/measurements.md)).
- The flyout in the dark theme with live data, from automated capture.

Partially verified:

- DirectML on the Radeon iGPU and the RTX GPU returned the same result as CPU on the fixture, and latency was recorded. Energy was not measured, because the test machine is a desktop without trustworthy per-component power data. DirectML is not used by default (see below).

Everything else in the checklist is **not hardware-verified**. This includes DDC writes and restoration, monitor sleep and wake, two-monitor and dock cases, the flyout on other themes, scales and taskbar positions, the tray icon in the overflow area, presence with the camera (denied access, sharing with a video call, two cameras, dim and restore latency), screensaver protection, battery suspension, Laptop follow, keyboard keys, Efficiency mode latency, launch at login, clean-profile install and uninstall, and SmartScreen behavior. The only idle measurement is for the bare shell, not the full app: 0.078% of one core and a 57.6 MB working set over 60 seconds. The full-app idle cost is not measured.

### Acceleration

Face detection runs on the CPU by default. DirectML is off by default: the acceleration allowlist `assets/inference/acceleration-profiles.toml` is empty, so no device uses an accelerator. A device gets DirectML only after a recorded measurement shows lower energy than the CPU with the same detection result. No such measurement exists yet. NPU support is out of the first release.

### Install and uninstall

Release builds are not code-signed yet. The release is a per-user zip, not an installer package, and neither the build nor the installer needs administrator rights. Build it with `scripts\build-windows.ps1`, which writes `dist\TelaVivaOLED-<version>-win-x64.zip` and prints its SHA-256. Extract the zip and run `Install-TelaVivaOLED.ps1`. The app installs to `%LOCALAPPDATA%\Programs\TelaVivaOLED` with a Start menu shortcut. Install and uninstall in a clean profile are not yet run.

To uninstall, use Settings > Apps, or run `Uninstall-TelaVivaOLED.ps1` from the install folder. Settings, calibration and logs stay in `%LOCALAPPDATA%\TelaVivaOLED` unless you pass `-RemoveUserData`. The full options are in [the packaging guide](packaging/windows/README.md).

Windows SmartScreen may show "Windows protected your PC" the first time you run `TelaViva.exe`, because the build is unsigned. This behavior is not yet verified (row I3). Only continue for a zip you downloaded from the project and checked against the SHA-256 printed by the build.

### Tray icon

Windows 11 may place the new tray icon in the overflow area. To pin it, select the overflow arrow in the taskbar and drag TelaViva OLED onto the taskbar. You can also open Settings > Personalization > Taskbar > Other system tray icons and turn it on. Neither method has been verified on the test machine yet (row U5).

### Privacy

Everything runs locally. The app has no telemetry and makes no network requests of its own. Camera frames are processed in memory and are not stored, transmitted or logged. Device identifiers are not logged.

### Third-party components

The Windows build redistributes the Windows App SDK and WinUI 3 runtime. They ship under Microsoft Software License Terms, not an open-source license. The ONNX Runtime CPU library, the YuNet model and the Rust crates are open source. See the Windows section of [`licenses/DEPENDENCIES.md`](licenses/DEPENDENCIES.md) and [`licenses/windows/`](licenses/windows/README.md).

## Build on macOS

Requirements: Apple Silicon, macOS 14 or later, Xcode command-line tools and macOS SDK, stable Rust, and Python 3. The model is included in the repository. From the repository root, run:

```sh
./scripts/build-macos.sh
```

The script creates an ad-hoc signed local app bundle at `build/macos/TelaViva OLED.app`. On the first build, it downloads the pinned ONNX Runtime and verifies its checksums; later builds reuse the file and verify it again while packaging. The app does not download runtime files when it runs. Run `./scripts/build-macos.sh --help` for usage. See the [macOS build guide](packaging/macos/README.md) for the manual build steps, diagnostics and packaging details. Distribution signing and notarization are separate release steps.

To open the compiled app:

```sh
open 'build/macos/TelaViva OLED.app'
```

The app icon resources in `packaging/macos/Resources/` are generated and committed rather than built in CI, because the macos-14 CI runner cannot compile Icon Composer `.icon` files. Regenerating them with `sh tools/branding/build-icons.sh` needs Xcode 26 or later.

## Usage

1. Open the app and click its icon in the macOS menu bar. The icon shows presence: a filled dot when a face is detected, no dot when no face is detected or when presence is off, suspended or unavailable, and a dot outline when light is insufficient. Visual checks on a real menu bar are pending acceptance.
2. Adjust brightness and speaker volume independently for each detected external monitor. Unsupported controls are shown as unavailable. The popup fits one to three monitors and scrolls beyond three.
3. Open **Settings** to choose English (US) or Portuguese (Brazil), the camera, sampling intervals, continuous camera capture, battery behavior, optional launch at login, and the brightness-key and volume-key steps. Settings has General, Keyboard and Automatic brightness tabs. The app remembers the cameras you select, up to eight, most recent first, and uses the first one that is connected, so reconnecting a camera or plugging in a preferred one is picked up without changing settings. When none is connected, it falls back to the system default camera; hardware checks with two webcams are pending.
4. Enable presence protection to request camera access. Two valid no-face samples trigger dimming; a returning face clears the presence reason. Low light suspends detection instead of triggering absence. Presence checking also needs a monitor above the brightness threshold, set in Settings with a slider from 10 to 90% (20% by default); lower it to 10% to keep protection at low brightness.
5. Optionally enable screensaver protection, then choose **Protection brightness** for each monitor in the popup. The slider cannot exceed that monitor's current brightness. Each monitor in the popup also has a switch that includes it in or excludes it from OLED protection (on by default); the same switches are listed in Settings.

**Follow the MacBook brightness (optional, MacBook only).** On a MacBook whose built-in display has adjustable brightness, Settings > Automatic brightness can link calibrated external monitors to the built-in display, including macOS automatic brightness driven by its ambient light sensor. Each monitor has calibration points that you add and remove. At least two are needed for the monitor to follow. A point can be any whole percentage of the monitor's brightness from 1 to 100%, up to 32 per monitor, and the points are kept in ascending order automatically. Adding a point, or choosing Calibrate on a saved point, opens a dialog with a slider for the MacBook screen and two buttons: **Done** saves that point at the slider value and closes the dialog, and **Cancel** (or Escape) closes it without saving. **Done** is available once the monitor has reached the point's brightness. Removing points until fewer than two remain stops following, and the monitor shows as not calibrated. Existing points are kept and shown in the list. Choosing a point sets the monitor to that brightness and shows a slider for the MacBook screen, so you can match the two screens by eye. Calibrations made by earlier development builds must be redone. Calibration cannot be performed while the MacBook screen is off or the lid is closed: the calibration controls stay hidden, and a calibration in progress stops and restores the earlier brightness. While calibrating, the app turns macOS automatic brightness off and restores the earlier brightness and setting when you finish, cancel, close Settings or quit. If the app was interrupted, it recovers them at the next launch. Only monitors with supported DDC brightness and a complete calibration (at least two points) follow the MacBook. For those monitors, the popup shows only the volume slider (and the protection brightness), and the brightness keys control the MacBook. Other monitors keep manual control. Following pauses while the MacBook lid is closed or its built-in screen is off; during the pause the popup shows the brightness slider again, the brightness keys (with keyboard routing enabled) control the external monitor again, and the monitors keep their last brightness. When the lid opens or the screen turns on, following resumes by itself and sets each calibrated monitor to the current MacBook level, replacing any manual change made during the pause. Calibration is saved per monitor ID and kept for disconnected monitors, and it is reused when the same monitor ID returns; the same monitor on a different port or dock may need recalibration. On a Mac without a built-in display with adjustable brightness, such as a Mac mini, the switch stays disabled and explains why. This feature reads and sets the built-in brightness through undocumented macOS interfaces ([ADR 0007](adr/0007-private-display-apis-for-built-in-brightness.md)), so it is not suitable for Mac App Store distribution and may stop working after a macOS update.

Keyboard routing is optional and starts disabled. Enable it in Settings and grant macOS Accessibility and Input Monitoring access when requested. Brightness up/down keys then adjust the supported external monitor under the cursor; an unsupported control or a cursor over the built-in display keeps normal macOS behavior. Brightness keys that arrive as plain key codes 144 and 145 are recognized too; the event tap acts only on those two key codes and never reads typed characters. Volume up/down keys ignore the cursor: they adjust an external monitor's speakers only while that monitor is the Mac's current sound output (matched through its display EDID identifier, verified so far only on one LG ULTRAFINE over DisplayPort); with headphones, built-in speakers, or any other output, or when no single monitor matches, macOS keeps control of the volume. Holding a key retains the initially selected monitor until release. The step for each key pair is set in Settings (1 to 15 percentage points, 5 by default, separately for brightness and volume); Shift+Option uses 1 point. The mute key follows the volume rule: while an external monitor is the Mac's sound output, mute sets that monitor's volume to zero and the next press restores the previous level (emulated with volume zero, not the monitor's own mute function); otherwise macOS keeps control. Only ordinary function keys retain their normal behavior unconditionally. Physical keyboard capture and device compatibility still require hardware acceptance. For a monitor that follows the MacBook brightness, the brightness keys keep normal macOS behavior and control the MacBook screen.

Protection reasons are combined, and restoration checks the current brightness before changing it so external adjustments can be preserved. Display sleep follows macOS-reported state; a monitor manually powered off may remain logically awake depending on its connection. When monitors have mixed sleep states, monitoring continues for awake eligible monitors and hardware writes to sleeping targets are withheld. After wake, protection writes use fresh brightness readback and ownership checks. Screensaver startup reconciliation and connection-specific behavior remain validation work. Setting brightness to zero does not guarantee that an OLED panel turns completely off or prevent all burn-in.

## Development checks

The core tests do not require the ONNX Runtime. The integration test that executes the pinned model does; prepare the runtime first as above.

```sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
cargo clippy --workspace --all-targets --locked -- -D warnings
tools/tests/test-camera-lifecycle.sh
tools/tests/test-ddc-transaction.sh
tools/tests/test-manual-monitor-controls.sh
tools/tests/test-popup-layout.sh
tools/tests/test-builtin-brightness-monitor.sh
tools/tests/test-ambient-calibration.sh
tools/tests/test-core-bridge.sh
tools/tests/test-camera-lifecycle-bridge.sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-default-audio-output.sh
tools/tests/test-media-key-probe.sh
tools/tests/test-status-item-icon.sh
tools/tests/test-branding-assets.sh
```

After fetching the runtime, run the full test suite with `cargo test --workspace --all-targets --locked`. CI intentionally skips only the runtime integration test and builds/packages the native app separately. The macOS workflow runs on macOS 14; it does not perform camera acceptance or physical monitor writes.

`tools/tests/test-core-bridge.sh` needs the Rust core library (`libtelaviva_core.a`) built first (see the macOS workflow order).

## Project layout

- `crates/telaviva-core/`: shared settings, protection policy, and C ABI.
- `crates/telaviva-inference/`: local image-quality assessment and YuNet inference.
- `native/macos/`: SwiftUI/AppKit UI, camera capture, and macOS device adapters.
- `native/windows/`: WinUI 3 shell and the Windows platform layer (Windows 11 x64).
- `packaging/windows/`: Windows zip build and per-user installer scripts.
- `include/`: C interfaces used by the native shell.
- `assets/model/` and `licenses/`: model provenance and third-party license notices.
- `assets/branding/`: vector icon sources (master SVG, Liquid Glass layers, Icon Composer document and menu-bar glyphs); regenerate with `sh tools/branding/build-icons.sh`.
- `packaging/macos/Resources/`: generated app icon resources (`AppIcon.icns`, `Assets.car`).
- `specs/archived/external-monitor-control-and-oled-presence-protection/`: requirements, design, implementation plan, and validation evidence.
- `specs/archived/macbook-brightness-follow/`: requirements, design, plan and the hardware validation checklist for following the MacBook brightness.

TelaViva OLED is licensed under the [MIT License](LICENSE). Third-party dependency, model, runtime, and DDC attribution records are listed in [`licenses/`](licenses/README.md); those notices remain separate from the project's license.
