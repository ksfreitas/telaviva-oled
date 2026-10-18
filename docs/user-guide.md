# TelaViva OLED user guide

This guide describes how each feature of TelaViva OLED behaves on macOS and Windows. For an overview, installation and build instructions, see the [README](../README.md).

- [The popup or flyout](#the-popup-or-flyout)
- [Settings](#settings)
- [Presence protection](#presence-protection)
- [Screensaver protection](#screensaver-protection)
- [Following the laptop brightness](#following-the-laptop-brightness)
- [Keyboard and hardware keys](#keyboard-and-hardware-keys)
- [Windows specifics](#windows-specifics)
- [Limits and compatibility](#limits-and-compatibility)
- [Privacy](#privacy)

## The popup or flyout

Click the menu bar icon (macOS) or the tray icon (Windows) to open it.

- Each detected external monitor has a card with an independent brightness slider and speaker volume slider. A control the monitor does not support is shown as unavailable.
- The popup fits one to three monitors and scrolls beyond three.
- Each monitor has a switch that includes it in or excludes it from OLED protection (on by default). The same switches are listed in Settings.
- With screensaver protection on, each monitor also has a **Protection brightness** slider. It cannot exceed that monitor's current brightness.
- The icon shows presence:
  - a filled dot when a face is detected;
  - no dot when no face is detected, or when presence is off, suspended or unavailable;
  - a dot outline when light is insufficient.
- **About TelaViva OLED** is in the menu bar popup (macOS) or the tray right-click menu (Windows), and in Settings. It shows the installed version, the project's GitHub page and whether a newer release is available. **Check now** asks GitHub again. The app only informs you; it never downloads or installs an update.

## Settings

Settings has General, Keyboard and Automatic brightness pages.

- **Language:** English (US) or Portuguese (Brazil).
- **Camera:** the app remembers the cameras you select, up to eight, most recent first, and uses the first one that is connected, so reconnecting a camera or plugging in a preferred one is picked up without changing settings. When none is connected, it falls back to the system default camera. On Windows, infrared cameras are hidden from selection.
- **Sampling intervals:** the presence sampling interval defaults to 10 seconds (5 to 300 seconds). While dimmed, return checks default to 1 second (0.25 to 5 seconds).
- **Keep camera on:** off by default. See [Presence protection](#presence-protection).
- **Battery behavior:** presence checking pauses on battery by default.
- **Presence brightness threshold:** a slider from 10 to 90% (20% by default).
- **Launch at login:** off by default.
- **Hardware-key steps:** see [Keyboard and hardware keys](#keyboard-and-hardware-keys).

## Presence protection

Presence protection is off until you enable it, and enabling it requests camera access. It detects any face locally. It does not identify people, enroll faces, save webcam images or send frames to a service.

### How it works

- Two valid no-face checks dim the monitor, and a returning face clears the presence reason and restores the brightness.
- Checks run at the sampling interval. While a monitor is dimmed, return checks run faster.
- The camera stays open during return checks, while inference still follows the configured interval.

### Camera use

Normal presence checks use one-frame sessions. With **Keep camera on** enabled (off by default), capture is retained between checks without increasing the inference frequency.

Capture stops when:

- presence protection is disabled;
- battery suspension applies;
- no eligible monitor remains;
- all eligible external displays sleep;
- every eligible monitor is at or below the brightness threshold (20% by default; lower the slider to 10% to keep protection at low brightness);
- the app exits.

### Low light

Insufficient light suspends face detection instead of counting as absence. Recovery waits 10 seconds, keeps capture running for five seconds to allow exposure adjustment, then checks a fresh frame. Two usable-light checks are required before face detection resumes.

### Combined reasons and sleep

- Protection reasons are combined. Restoration checks the current brightness before changing it, so external adjustments can be preserved.
- Display sleep follows the operating system's reported state. A monitor that is manually powered off may remain logically awake, depending on its connection.
- When monitors have mixed sleep states, monitoring continues for the awake eligible monitors and hardware writes to sleeping targets are withheld.
- After wake, protection writes use fresh brightness readback and ownership checks.

## Screensaver protection

Screensaver protection is optional. Enable it, then choose a **Protection brightness** for each monitor in the popup. The strongest active reduction applies, and brightness is restored only after every applicable protection reason clears. Screensaver protection is independent of presence protection.

## Following the laptop brightness

On a laptop whose built-in display has adjustable brightness, **Settings > Automatic brightness** can link calibrated external monitors to the built-in display, including its automatic brightness driven by the ambient light sensor. The built-in display is the MacBook on macOS and the Laptop on Windows (Notebook in Portuguese).

### Calibration

- Each monitor has calibration points that you add and remove. At least two are needed for the monitor to follow.
- A point can be any whole percentage of the monitor's brightness from 1 to 100%, up to 32 per monitor. Points are kept in ascending order automatically.
- Adding a point, or choosing **Calibrate** on a saved point, opens a dialog with a slider for the laptop screen and two buttons. **Done** saves the point at the slider value and closes the dialog; **Cancel** (or Escape) closes it without saving. **Done** is available once the monitor has reached the point's brightness.
- Choosing a point sets the monitor to that brightness and shows a slider for the laptop screen, so you can match the two screens by eye.
- Removing points until fewer than two remain stops following, and the monitor shows as not calibrated. Existing points are kept and listed.
- Calibrations made by earlier development builds must be redone.
- Calibration cannot be performed while the laptop screen is off or the lid is closed: the calibration controls stay hidden, and a calibration in progress stops and restores the earlier brightness.
- While calibrating, the app turns automatic brightness off. It restores the earlier brightness and setting when you finish, cancel, close Settings or quit. If the app was interrupted, it recovers them at the next launch.

### Following

- Only monitors with supported DDC brightness and a complete calibration (at least two points) follow the laptop. Other monitors keep manual control.
- For a following monitor, the popup shows only the volume slider (and the protection brightness), and the brightness keys control the laptop screen.
- Following pauses while the lid is closed or the built-in screen is off. During the pause the popup shows the brightness slider again, the brightness keys (with keyboard routing enabled) control the external monitor again, and the monitors keep their last brightness.
- When the lid opens or the screen turns on, following resumes by itself and sets each calibrated monitor to the current laptop level, replacing any manual change made during the pause.
- Calibration is saved per monitor ID, kept for disconnected monitors and reused when the same monitor ID returns. The same monitor on a different port or dock may need recalibration.
- On a computer without a built-in display with adjustable brightness, such as a desktop or a Mac mini, the switch stays disabled and explains why.

### Platform notes

- **macOS:** this feature reads and sets the built-in brightness through undocumented macOS interfaces ([ADR 0007](../adr/0007-private-display-apis-for-built-in-brightness.md)), so it is not suitable for Mac App Store distribution and may stop working after a macOS update.
- **Windows:** calibration turns Windows adaptive brightness off and restores it afterwards, including at the next launch after a crash.

## Keyboard and hardware keys

### macOS keyboard routing

Keyboard routing is optional and starts disabled. Enable it in Settings and grant macOS Accessibility and Input Monitoring access when requested.

- Brightness up and down keys adjust the supported external monitor under the cursor. An unsupported control, or a cursor over the built-in display, keeps normal macOS behavior. Brightness keys that arrive as plain key codes 144 and 145 are recognized too. The event tap acts only on those two key codes and never reads typed characters.
- Volume up and down keys ignore the cursor. They adjust an external monitor's speakers only while that monitor is the Mac's current sound output, matched through its display EDID identifier. With headphones, built-in speakers or any other output, or when no single monitor matches, macOS keeps control of the volume. Matching has been verified on a limited set of hardware.
- The mute key follows the volume rule. While an external monitor is the sound output, mute sets that monitor's volume to zero and the next press restores the previous level. This is emulated with volume zero, not the monitor's own mute function. Otherwise macOS keeps control.
- Holding a key retains the initially selected monitor until release.
- The step for each key pair is set in Settings, from 1 to 15 percentage points (5 by default), separately for brightness and volume. Shift+Option moves 1 point.
- For a monitor that follows the MacBook brightness, the brightness keys keep normal macOS behavior and control the MacBook screen.
- Only ordinary function keys retain their normal behavior unconditionally.

Physical keyboard capture and device compatibility still require hardware acceptance.

### Windows hardware keys

The Windows volume and mute keys are opt-in. They adjust an external monitor only while it is the default sound output. Brightness keys are not intercepted and keep their normal Windows behavior.

## Windows specifics

- **Tray icon:** Windows 11 may place a new tray icon in the overflow area. Select the overflow arrow in the taskbar and drag TelaViva OLED onto the taskbar, or open Settings > Personalization > Taskbar > Other system tray icons and turn it on.
- **Efficiency mode:** the app requests Efficiency mode (EcoQoS) while it is idle in the background, and it leaves a user-forced Efficiency mode in place.
- **Launch at login:** off by default, through the per-user `Run` registry value.
- **Install, uninstall and data locations:** see the [Windows packaging guide](../packaging/windows/README.md). Settings, calibration and logs stay in `%LOCALAPPDATA%\TelaVivaOLED` unless you remove them on uninstall.
- **SmartScreen:** release builds are not code-signed yet, so Windows may show "Windows protected your PC" the first time you run `TelaViva.exe`. Only continue for a zip you downloaded from the project and checked against its SHA-256.
- **Acceleration:** face detection runs on the CPU by default. DirectML is off by default because the acceleration allowlist `assets/inference/acceleration-profiles.toml` is empty, so no device uses an accelerator. A device gets DirectML only after a recorded measurement shows lower energy than the CPU with the same detection result. NPU support is out of the first release.

## Limits and compatibility

- macOS targets Apple Silicon and macOS 14 or later. Intel Macs are deferred.
- Windows support is Windows 11 x64 only. Windows 10 and ARM64 are not supported yet.
- DDC support depends on the monitor, the port, the cable, any adapter or dock, and the graphics hardware. It is connection-specific and not uniform, including on Apple Silicon.
- Setting brightness to zero does not guarantee that an OLED panel turns completely off, and it does not prevent all burn-in.
- Hardware validation is partial on both platforms. Recorded results, including what has not been checked, are in:
  - the [macOS release-readiness report](../specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md);
  - the [MacBook follow checklist](../specs/archived/macbook-brightness-follow/validation/hardware-checklist.md);
  - the [Windows hardware checklist](../specs/archived/windows-version/validation/hardware-checklist.md);
  - the [Windows measurements](../specs/archived/windows-version/validation/measurements.md).

## Privacy

Everything runs locally and the app has no telemetry. Camera frames are processed in memory and are not stored, transmitted or logged, and device identifiers are not logged.

The only network request is an anonymous HTTPS GET to the GitHub releases API, which looks for a newer version. It is made only when the About page is open or **Check now** is pressed. Apart from the standard HTTPS headers, it sends nothing but the app version in the `User-Agent` header, and it downloads nothing.

Third-party licenses are listed in [`licenses/DEPENDENCIES.md`](../licenses/DEPENDENCIES.md).
