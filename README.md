# TelaViva OLED

TelaViva OLED is an open-source desktop app for controlling external monitor brightness and volume, with optional OLED brightness protection. Its shared policy and inference code is written in Rust; the current desktop shell uses SwiftUI and AppKit on Apple Silicon macOS. Windows support is planned and has not been implemented or validated.

Presence protection detects any face locally. It does not identify people, enroll faces, save webcam images, or send frames to a service. The default presence sampling interval is 10 seconds (configurable from 5 to 300 seconds); while dimmed, return checks default to 1 second (0.25 to 5 seconds). Presence protection is off until enabled, and pauses on battery by default. The camera stays open during return checks, while inference still follows the configured interval. Normal presence checks use one-frame sessions unless **Keep camera on** is enabled (off by default). This option retains capture between checks without increasing inference frequency. Capture still stops when presence protection is disabled, battery suspension applies, no eligible monitor remains, all eligible external displays sleep, or the app exits. Insufficient light suspends face detection instead of counting as absence: recovery waits 10 seconds, keeps capture running for five seconds to allow exposure adjustment, then checks a fresh frame. Two usable-light checks are required before face detection resumes.

## Status

The project is under active development. Rust tests, native macOS builds, and an ABI smoke test have passed. A read-only DDC check has read brightness and volume on one Dell S3225QC connected to an M4 Pro. A connected LG ULTRAFINE has also passed a one-point brightness change and restoration through the application's Rust/native command path, with fresh physical readback; see the [DDC fix and hardware evidence](specs/archived/external-monitor-hardware-keys-and-fluid-controls/lg-ultrafine-ddc-brightness-writes.md). This does not establish support across monitors, ports, adapters, docks, or Apple Silicon generations. Broader camera, physical write/restoration, multi-monitor and release-distribution acceptance remains in progress. See the [implementation plan](specs/archived/external-monitor-control-and-oled-presence-protection/plan.md) and [validation reports](specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md) for evidence and outstanding work.

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

## Usage

1. Open the app and click the monitor icon in the macOS menu bar.
2. Adjust brightness and speaker volume independently for each detected external monitor. Unsupported controls are shown as unavailable.
3. Open **Settings** to choose English (US) or Portuguese (Brazil), the camera, sampling intervals, continuous camera capture, battery behavior and optional launch at login.
4. Enable presence protection to request camera access. Two valid no-face samples trigger dimming; a returning face clears the presence reason. Low light suspends detection instead of triggering absence.
5. Optionally enable screensaver protection, then choose **Protection brightness** for each monitor in the popup. The slider cannot exceed that monitor's current brightness.

Keyboard routing is optional and starts disabled. Enable it in Settings and grant macOS Accessibility and Input Monitoring access when requested. Brightness up/down keys then adjust the supported external monitor under the cursor; an unsupported control or a cursor over the built-in display keeps normal macOS behavior. Brightness keys that arrive as plain key codes 144 and 145 are recognized too; the event tap acts only on those two key codes and never reads typed characters. Volume up/down keys ignore the cursor: they adjust an external monitor's speakers only while that monitor is the Mac's current sound output (matched through its display EDID identifier, verified so far only on one LG ULTRAFINE over DisplayPort); with headphones, built-in speakers, or any other output, or when no single monitor matches, macOS keeps control of the volume. Holding a key retains the initially selected monitor until release. Normal steps are 6.25 percentage points; Shift+Option uses 1.5625 points. The mute key follows the volume rule: while an external monitor is the Mac's sound output, mute sets that monitor's volume to zero and the next press restores the previous level (emulated with volume zero, not the monitor's own mute function); otherwise macOS keeps control. Only ordinary function keys retain their normal behavior unconditionally. Physical keyboard capture and device compatibility still require hardware acceptance.

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
tools/tests/test-core-bridge.sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-default-audio-output.sh
tools/tests/test-media-key-probe.sh
```

After fetching the runtime, run the full test suite with `cargo test --workspace --all-targets --locked`. CI intentionally skips only the runtime integration test and builds/packages the native app separately. The macOS workflow runs on macOS 14; it does not perform camera acceptance or physical monitor writes.

`tools/tests/test-core-bridge.sh` needs the Rust core library (`libtelaviva_core.a`) built first (see the macOS workflow order).

## Project layout

- `crates/telaviva-core/`: shared settings, protection policy, and C ABI.
- `crates/telaviva-inference/`: local image-quality assessment and YuNet inference.
- `native/macos/`: SwiftUI/AppKit UI, camera capture, and macOS device adapters.
- `include/`: C interfaces used by the native shell.
- `assets/model/` and `licenses/`: model provenance and third-party license notices.
- `specs/archived/external-monitor-control-and-oled-presence-protection/`: requirements, design, implementation plan, and validation evidence.

TelaViva OLED is licensed under the [MIT License](LICENSE). Third-party dependency, model, runtime, and DDC attribution records are listed in [`licenses/`](licenses/README.md); those notices remain separate from the project's license.
