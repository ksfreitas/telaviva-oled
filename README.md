# TelaViva OLED

TelaViva OLED is an open-source desktop app for controlling external monitor brightness and volume, with optional OLED brightness protection. External monitors can optionally follow the brightness of a MacBook's built-in display. Its shared policy and inference code is written in Rust; the current desktop shell uses SwiftUI and AppKit on Apple Silicon macOS. Windows support is planned and has not been implemented or validated.

Presence protection detects any face locally. It does not identify people, enroll faces, save webcam images, or send frames to a service. The default presence sampling interval is 10 seconds (configurable from 5 to 300 seconds); while dimmed, return checks default to 1 second (0.25 to 5 seconds). Presence protection is off until enabled, and pauses on battery by default. The camera stays open during return checks, while inference still follows the configured interval. Normal presence checks use one-frame sessions unless **Keep camera on** is enabled (off by default). This option retains capture between checks without increasing inference frequency. Capture still stops when presence protection is disabled, battery suspension applies, no eligible monitor remains, all eligible external displays sleep, every eligible monitor is at or below the brightness threshold (20% by default), or the app exits. Insufficient light suspends face detection instead of counting as absence: recovery waits 10 seconds, keeps capture running for five seconds to allow exposure adjustment, then checks a fresh frame. Two usable-light checks are required before face detection resumes.

## Status

The project is under active development. Rust tests, native macOS builds, and an ABI smoke test have passed. A read-only DDC check has read brightness and volume on one Dell S3225QC connected to an M4 Pro. A connected LG ULTRAFINE has also passed a one-point brightness change and restoration through the application's Rust/native command path, with fresh physical readback; see the [DDC fix and hardware evidence](specs/archived/external-monitor-hardware-keys-and-fluid-controls/lg-ultrafine-ddc-brightness-writes.md). This does not establish support across monitors, ports, adapters, docks, or Apple Silicon generations. Broader camera, physical write/restoration, multi-monitor and release-distribution acceptance remains in progress. See the [implementation plan](specs/archived/external-monitor-control-and-oled-presence-protection/plan.md) and [validation reports](specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md) for evidence and outstanding work. Following the MacBook brightness, calibration and per-monitor protection have passed automated tests only; hardware acceptance is pending, see the [hardware checklist](specs/archived/macbook-brightness-follow/validation/hardware-checklist.md). The Liquid Glass app icon rendering in the Dock and Finder and the menu-bar presence icon are pending acceptance on real macOS hardware.

The macOS app targets Apple Silicon and macOS 14 or later. Development builds have been exercised on macOS 27; this is not a tested compatibility matrix. Intel Macs are deferred. DDC support depends on the monitor and connection.

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
3. Open **Settings** to choose English (US) or Portuguese (Brazil), the camera, sampling intervals, continuous camera capture, battery behavior, optional launch at login, and the brightness-key and volume-key steps. Settings has General, Keyboard and Automatic brightness tabs.
4. Enable presence protection to request camera access. Two valid no-face samples trigger dimming; a returning face clears the presence reason. Low light suspends detection instead of triggering absence. Presence checking also needs a monitor above the brightness threshold, set in Settings with a slider from 10 to 90% (20% by default); lower it to 10% to keep protection at low brightness.
5. Optionally enable screensaver protection, then choose **Protection brightness** for each monitor in the popup. The slider cannot exceed that monitor's current brightness. Each monitor in the popup also has a switch that includes it in or excludes it from OLED protection (on by default); the same switches are listed in Settings.

**Follow the MacBook brightness (optional, MacBook only).** On a MacBook whose built-in display has adjustable brightness, Settings > Automatic brightness can link calibrated external monitors to the built-in display, including macOS automatic brightness driven by its ambient light sensor. Each monitor has four calibration points (25%, 50%, 75% and 100% of the MacBook brightness). Choosing a point sets the MacBook screen to that level and shows a slider for the monitor, so you can match the two screens by eye. While calibrating, the app turns macOS automatic brightness off and restores the earlier brightness and setting when you finish, cancel, close Settings or quit. If the app was interrupted, it recovers them at the next launch. Only monitors with supported DDC brightness and all four points calibrated follow the MacBook. For those monitors, the popup shows only the volume slider (and the protection brightness), and the brightness keys control the MacBook. Other monitors keep manual control. Following pauses while the MacBook lid is closed or its built-in screen is off; during the pause the popup shows the brightness slider again, the brightness keys (with keyboard routing enabled) control the external monitor again, and the monitors keep their last brightness. When the lid opens or the screen turns on, following resumes by itself and sets each calibrated monitor to the current MacBook level, replacing any manual change made during the pause. Calibration is saved per monitor ID and kept for disconnected monitors, and it is reused when the same monitor ID returns; the same monitor on a different port or dock may need recalibration. On a Mac without a built-in display with adjustable brightness, such as a Mac mini, the switch stays disabled and explains why. This feature reads and sets the built-in brightness through undocumented macOS interfaces ([ADR 0007](adr/0007-private-display-apis-for-built-in-brightness.md)), so it is not suitable for Mac App Store distribution and may stop working after a macOS update.

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
- `include/`: C interfaces used by the native shell.
- `assets/model/` and `licenses/`: model provenance and third-party license notices.
- `assets/branding/`: vector icon sources (master SVG, Liquid Glass layers, Icon Composer document and menu-bar glyphs); regenerate with `sh tools/branding/build-icons.sh`.
- `packaging/macos/Resources/`: generated app icon resources (`AppIcon.icns`, `Assets.car`).
- `specs/archived/external-monitor-control-and-oled-presence-protection/`: requirements, design, implementation plan, and validation evidence.
- `specs/archived/macbook-brightness-follow/`: requirements, design, plan and the hardware validation checklist for following the MacBook brightness.

TelaViva OLED is licensed under the [MIT License](LICENSE). Third-party dependency, model, runtime, and DDC attribution records are listed in [`licenses/`](licenses/README.md); those notices remain separate from the project's license.
