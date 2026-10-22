# TelaViva OLED user guide

This guide describes how each feature of TelaViva OLED behaves on macOS and Windows. For an overview, installation and build instructions, see the [README](../README.md).

- [The popup or flyout](#the-popup-or-flyout)
- [Settings](#settings)
- [Presence protection](#presence-protection)
- [Screensaver protection](#screensaver-protection)
- [Laptop screen protection](#laptop-screen-protection)
- [Following the laptop brightness](#following-the-laptop-brightness)
- [Keyboard and hardware keys](#keyboard-and-hardware-keys)
- [Inference device](#inference-device)
- [Windows specifics](#windows-specifics)
- [Limits and compatibility](#limits-and-compatibility)
- [Diagnostic logs](#diagnostic-logs)
- [Privacy](#privacy)

## The popup or flyout

Click the menu bar icon (macOS) or the tray icon (Windows) to open it.

- Each detected external monitor has a card with an independent brightness slider and speaker volume slider. A control the monitor does not support is shown as unavailable. On macOS and Windows, a laptop's built-in screen has a card too, with a brightness slider and no volume slider. See [Laptop screen protection](#laptop-screen-protection).
- The popup fits one to three monitors and scrolls beyond three.
- Each monitor has a switch that includes it in or excludes it from OLED protection (on by default). The same switches are listed in Settings. A laptop's built-in screen has one too.
- While presence or screensaver protection is on, each protected monitor also has a **Protection brightness** slider, from 1% to 50%. The same level applies to presence and to the screensaver, and it does not depend on the monitor's current brightness. Protection never raises a screen above its current brightness.
- Presence is shown by the menu bar icon (macOS) or the tray icon (Windows), not by a line in the popup. On Windows the popup footer shows only the camera status, as `Camera: <status>`. The icon shows:
  - a filled dot when a face is detected;
  - no dot when no face is detected, or when presence is off, suspended or unavailable;
  - a dot outline when light is insufficient.
- **About TelaViva OLED** is in Settings on macOS. On Windows, it is in Settings and the tray right-click menu. It shows the installed version, the project's GitHub page and whether a newer release is available. **Check now** asks GitHub again. The app only informs you; it never downloads or installs an update.

## Settings

Settings has General, Keyboard, Automatic brightness, Advanced and About pages on both platforms.

- **Language:** English (US) or Portuguese (Brazil).
- **Camera:** the app remembers the cameras you select, up to eight, most recent first, and uses the first one that is connected, so reconnecting a camera or plugging in a preferred one is picked up without changing settings. When none is connected, it falls back to the system default camera. On Windows, infrared cameras are hidden from selection.
- **Monitored display:** the display whose brightness decides whether the camera runs. See [Laptop screen protection](#laptop-screen-protection).
- **Sampling intervals:** the presence sampling interval defaults to 10 seconds (5 to 300 seconds). While dimmed, return checks default to 1 second (0.25 to 5 seconds).
- **Battery behavior:** presence checking pauses on battery by default.
- **Check presence only above brightness:** the presence brightness threshold. It is a slider from 0 to 90%, 20% by default. Each display keeps its own value. See [Laptop screen protection](#laptop-screen-protection).
- **Launch at login:** off by default.
- **Hardware-key steps:** see [Keyboard and hardware keys](#keyboard-and-hardware-keys).
- **Inference device** (Advanced page): where face detection runs. See [Inference device](#inference-device).
- **Keep camera on** (Advanced page): on by default. Turning it off can cause detection problems, depending on the camera, and can produce insufficient-light events. See [Presence protection](#presence-protection).

## Presence protection

Presence protection is off until you enable it, and enabling it requests camera access. It detects any face locally. It does not identify people, enroll faces, save webcam images or send frames to a service.

### How it works

- Two valid no-face checks dim the monitor, and a returning face clears the presence reason and restores the brightness.
- Checks run at the sampling interval. While a monitor is dimmed, return checks run faster.
- After the first no-face check, the confirming check runs after the short return interval (1 second by default, range 0.25 to 5 seconds) instead of a full sampling interval, so the monitor dims about one sampling interval plus about a second after you leave. A present, insufficient-light or unavailable result clears that first miss.
- The camera stays open during return checks, while inference still follows the configured interval.

### Camera use

With **Keep camera on** turned off, normal presence checks use one-frame sessions. When it is enabled (on by default, in Settings > Advanced), capture is retained between checks without increasing the inference frequency.

Capture stops when:

- presence protection is disabled;
- battery suspension applies;
- no protected display that the app can read and dim remains;
- every eligible display is asleep and none is dimmed by presence;
- the gate display is at or below its brightness threshold and no display is dimmed by presence (20% by default; the slider goes down to 0%);
- the app exits.

The gate display is the monitored display, or the next awake display when the monitored one sleeps. A display that presence has dimmed keeps the camera running, so that its return is detected.

With the laptop lid closed, the built-in camera is not used. See [Laptop screen protection](#laptop-screen-protection).

### Low light

Insufficient light suspends face detection instead of counting as absence. Recovery waits 10 seconds, keeps capture running for five seconds to allow exposure adjustment, then checks a fresh frame. Two usable-light checks are required before face detection resumes. With **Keep camera on** turned off, one-frame sessions are more likely to see a dark frame while the camera adjusts its exposure, depending on the camera.

### Combined reasons and sleep

- Protection reasons are combined. Restoration checks the current brightness before changing it, so external adjustments can be preserved.
- Display sleep follows the operating system's reported state. A monitor that is manually powered off may remain logically awake, depending on its connection.
- When monitors have mixed sleep states, monitoring continues for the awake eligible monitors and hardware writes to sleeping targets are withheld.
- After wake, protection writes use fresh brightness readback and ownership checks.
- If a monitor that protection has dimmed is disconnected and found again after sleep, under the same monitor ID at the dimmed level, protection restores the brightness it had before the dim. If it comes back at another level, that level is kept. If it comes back under another monitor ID, the earlier brightness is not restored. If you are still away, the monitor dims again after the usual two no-face checks. This memory lasts only while the app runs, so quitting before the monitor returns loses it. Hardware checks are pending.

## Screensaver protection

Screensaver protection is optional. Enable it, and each protected screen dims to its **Protection brightness** when the screensaver starts. Set the level for each monitor in the popup, from 1% to 50% (1% by default). The same level applies to presence. The strongest active reduction applies, and brightness is restored only after every applicable protection reason clears. Screensaver protection is independent of presence protection. The laptop screen is included when it is protected; see [Laptop screen protection](#laptop-screen-protection).

## Laptop screen protection

This section applies to macOS and Windows. On macOS, the built-in screen of a MacBook is called the **MacBook screen**. On Windows, it is called **Laptop** (**Notebook** in Portuguese). The built-in screen takes part in the brightness controls and in OLED protection in the same way as an external monitor. This section calls it the laptop screen.

### Controls

- The laptop screen is listed in the popup and in **Settings > General** as **MacBook screen** on macOS and **Laptop** on Windows. It has a protection switch, a brightness slider and a **Protection brightness** slider. It has no volume control.
- The protection switch is on by default. Turn it off to exclude the screen from protection.
- The brightness slider is locked while protection holds the screen dimmed and while the screen is unavailable. The brightness keys keep their normal macOS or Windows behavior.
- The screen is listed only while it is available. With the lid closed, it is hidden from the popup and from the Settings protection list, and it reappears when the lid opens.

### Protection

- With the switch on, the screen dims when no face is detected, after the same two-check confirmation as external monitors, and when the screensaver starts. It is restored when the face returns or the screensaver stops.
- The app restores the screen when it quits. If a restore fails, the app tries again on the next state change, when the screen becomes available again, and at the next launch.
- **Protection brightness** is a fixed level for each screen, from 1% to 50%. The default is 1%. The same level applies to presence and to the screensaver.
- Protection never raises a screen above its current brightness. A screen that is already darker than the level keeps its brightness. The slider is not limited by the current brightness.
- The **Protection brightness** slider is shown while presence or screensaver protection is on and the screen is protected.
- Calibration is refused while protection holds the screen. See [Following the laptop brightness](#following-the-laptop-brightness).

### Automatic brightness option

- **Turn off the system's automatic brightness while this screen is protected** is off by default. On macOS it changes the setting **Automatically adjust brightness**. On Windows it changes **Change brightness automatically when lighting changes** in the active power plan, for both AC and DC power. Either way, it is not the **Automatic brightness** tab of this app. It is available only while the screen's protection switch is on.
- When the option is on, the app turns the system setting off before it dims the laptop screen. It does this only when protection dims the laptop screen, not when only external monitors are dimmed. With the option off, protection never reads or changes the setting.
- When protection ends, the app turns the setting back on, unless you turned it on during protection. That change is kept.
- If the app quits or crashes while the screen is dimmed, the next launch restores the screen brightness and, if the app had turned the setting off, the setting.
- If the system does not let the app read or change the setting, the screen is still dimmed, a diagnostic is logged and a notice is shown. The app does not restore a setting it did not change.
- On macOS, the option uses private interfaces that are not verified on hardware yet. The Windows laptop checks are not yet recorded. See [Limits and compatibility](#limits-and-compatibility).

### Monitored display and camera

- **Monitored display**, in **Settings > General**, chooses the display whose brightness decides whether the camera runs. The default is **Automatic**, which on macOS picks the main external display, then the other external displays, then this Mac. On Windows, it picks the main external display, otherwise the laptop screen. You can also choose a specific display.
- The app remembers your choices by priority, most recent first, up to eight, like the camera choice. If the chosen display is not connected, the next remembered display that is connected is used, and then Automatic.
- The camera follows the gate display, which is the monitored display or, when the monitored display sleeps, the next awake display. The camera runs while the gate display is above its threshold, or while any display is dimmed by presence. Absence still dims every protected screen, including the laptop screen.
- **Check presence only above brightness** is saved for each display, from 0% to 90%. The slider edits the monitored display's value, or the global value when no display resolves. It is editable while presence protection is on. A display without its own value uses the global value, 20% by default. The camera stays off while the gate display is at or below its threshold and no display is dimmed by presence.
- With the lid closed, the built-in camera is not used. It behaves as if it were disconnected, and your camera choice is kept. The built-in camera is used again when the lid opens.

## Following the laptop brightness

On a laptop whose built-in display has adjustable brightness, **Settings > Automatic brightness** can link calibrated external monitors to the built-in display, including its automatic brightness driven by the ambient light sensor. The built-in display is the MacBook on macOS and the Laptop on Windows (Notebook in Portuguese).

### Calibration

- Each monitor has calibration points that you add and remove. At least two are needed for the monitor to follow.
- A point can be any whole percentage of the monitor's brightness from 1 to 100%, up to 32 per monitor. Points are kept in ascending order automatically.
- Adding a point, or choosing **Calibrate** on a saved point, opens a dialog with a slider for the laptop screen and two buttons. **Done** saves the point at the slider value and closes the dialog; **Cancel** (or Escape) closes it without saving. **Done** is available once the monitor has reached the point's brightness.
- Choosing a point sets the monitor to that brightness and shows a slider for the laptop screen, so you can match the two screens by eye.
- Removing points until fewer than two remain stops following, and the monitor shows as not calibrated. Existing points are kept and listed.
- Calibrations made by earlier development builds must be redone.
- Calibration cannot be performed while the laptop screen is off or the lid is closed: the calibration controls stay hidden, and a calibration in progress stops and restores the earlier brightness. Calibration is also refused while protection holds the laptop screen dimmed; the app asks you to try again in a moment.
- While calibrating, the app turns the system's automatic brightness off. It restores the earlier brightness and setting when you finish, cancel, close Settings or quit. Calibration and the [automatic brightness option](#automatic-brightness-option) share one owner of that setting, so it is restored only when the last of them ends. If the app was interrupted, it recovers the setting at the next launch. If the external monitor is not connected then, its brightness is restored when the monitor appears, for up to 90 seconds.

### Following

- Only monitors with supported DDC brightness and a complete calibration (at least two points) follow the laptop. Other monitors keep manual control.
- For a following monitor, the popup shows only the volume slider (and the protection brightness), and the brightness keys control the laptop screen.
- Following pauses while the lid is closed or the built-in screen is off. During the pause the popup shows the brightness slider again, the brightness keys (with keyboard routing enabled) control the external monitor again, and the monitors keep their last brightness.
- Following also pauses while protection dims the laptop screen. The dimmed level is never used as a follow input, so the monitors keep their brightness. Following resumes with a fresh reading after the laptop screen is restored.
- When the lid opens or the screen turns on, following resumes by itself and sets each calibrated monitor to the current laptop level, replacing any manual change made during the pause.
- Calibration is saved per monitor ID, kept for disconnected monitors and reused when the same monitor ID returns. The protection switch, the presence threshold, the protection brightness and the monitored-display choice follow the monitor ID too. On macOS the ID is tied to the port, so the same monitor on a different port or dock may need recalibration and may come back as a new display. Whether the ID also changes after sleep is not verified.
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

- Each press changes the volume by the step set in Settings as **Volume key step** (5 points by default, from 1 to 15). Holding Shift uses a fine step of 1 point.

## Inference device

Face detection runs on one device at a time. The **Inference device** setting, on the Advanced page of Settings, chooses it. The available choices and Automatic behavior depend on the platform.

- **Windows:** Automatic (the default) uses an integrated GPU when one passes the per-session check, and uses the CPU otherwise. A discrete GPU is available as a manual choice and is never selected automatically. The intent is to leave the main CPU and graphics card available for other work; energy use has not been measured, so no energy benefit is claimed. You can also select CPU or a specific GPU, listed by name and marked integrated or discrete.
- **macOS on Apple Silicon:** Automatic (the default) requests all CoreML compute units and falls back to CPU if validation or a later runtime call fails. Neural Engine preference and GPU preference ask CoreML to use the corresponding compute units; CoreML may still run unsupported operations on the CPU, and the app cannot report the physical unit used by each operation. CPU uses ONNX Runtime without CoreML. Energy use has not been measured, so no energy benefit is claimed.

Before relying on an accelerator, the app checks that it starts, returns the same presence classification as the CPU for the bundled self-test frame, and answers within 250 ms on macOS and Windows (the allowance covers the first CoreML run, which compiles lazily). This is a check of that frame, not a guarantee of equal results on every camera image. Speed compared with the CPU does not matter. If validation fails, or an accelerator fails later, detection continues on the CPU without losing the current frame.

- With **Automatic** on Windows, a failing card is skipped quietly until the graphics driver or the app version changes. On macOS, a failure keeps the current preference session on CPU; changing the preference or relaunching allows a new validation.
- With an accelerator you chose, the page shows a notice that the CPU took over. Selecting a different option and then returning to it tries it again.
- If the card you chose is no longer present (for example after a driver change or a removed card), the app behaves as Automatic for that run, keeps your choice stored, and shows a notice.

The page shows the device actually in use. The device is chosen when detection starts, so before the first detection the page says so. A change takes effect without restarting the app.

On Windows, DirectML is supplied by Windows and is not bundled with the app. If it is missing or too old for a card, that card fails the check and the CPU is used.

## Windows specifics

- **Tray icon:** Windows 11 may place a new tray icon in the overflow area. Select the overflow arrow in the taskbar and drag TelaViva OLED onto the taskbar, or open Settings > Personalization > Taskbar > Other system tray icons and turn it on.
- **Efficiency mode:** the app requests Efficiency mode (EcoQoS) while it is idle in the background, and it leaves a user-forced Efficiency mode in place.
- **Launch at login:** off by default, through the per-user `Run` registry value.
- **Install, uninstall and data locations:** see the [Windows packaging guide](../packaging/windows/README.md). Settings, calibration and logs stay in `%LOCALAPPDATA%\TelaVivaOLED` unless you remove them on uninstall.
- **SmartScreen:** release builds are not code-signed yet, so Windows may show "Windows protected your PC" the first time you run `TelaViva.exe`. Only continue for a zip you downloaded from the project and checked against its SHA-256.
- **Acceleration:** see [Inference device](#inference-device). The package ships one ONNX Runtime, the DirectML build, and uses the DirectML that comes with Windows. NPUs are not supported.
- **Laptop screen protection:** the laptop's built-in screen takes part in protection, in the monitored display choice and in the lid rule for the built-in camera, as on macOS. See [Laptop screen protection](#laptop-screen-protection). The automatic brightness option uses **Change brightness automatically when lighting changes** in the active power plan, for AC and DC, and restores it afterwards. The Windows laptop checks are not yet recorded.

## Limits and compatibility

- macOS targets Apple Silicon and macOS 14 or later. Intel Macs are deferred.
- Windows support is Windows 11 x64 only. Windows 10 and ARM64 are not supported yet.
- The laptop screen features, including the automatic brightness option and the monitored display, are not verified on hardware yet on either platform: the macOS hardware checks and the Windows laptop checks are not yet recorded.
- DDC support depends on the monitor, the port, the cable, any adapter or dock, and the graphics hardware. It is connection-specific and not uniform, including on Apple Silicon.
- Per-display settings and the reconnect restore follow the monitor's ID. On macOS the ID includes the port location, so a monitor on another port or dock is treated as a new display: its settings return to defaults, a protection switch you turned off turns back on, and a brightness dimmed by protection stays at the dimmed level instead of being restored.
- Setting brightness to zero does not guarantee that an OLED panel turns completely off, and it does not prevent all burn-in.
- The macOS private APIs for the built-in brightness and the system's automatic brightness are not verified on hardware. See [Laptop screen protection](#laptop-screen-protection).
- On a laptop with no external monitor, the camera runs for the laptop screen, subject to battery suspension and the brightness threshold.
- Hardware validation is partial on both platforms. Recorded results, including what has not been checked, are in:
  - the [macOS release-readiness report](../specs/archived/external-monitor-control-and-oled-presence-protection/validation/release-readiness.md);
  - the [MacBook follow checklist](../specs/archived/macbook-brightness-follow/validation/hardware-checklist.md);
  - the [Windows hardware checklist](../specs/archived/windows-version/validation/hardware-checklist.md);
  - the [Windows measurements](../specs/archived/windows-version/validation/measurements.md).

## Diagnostic logs

TelaViva OLED keeps a local log of its brightness decisions and of each DDC attempt. It is always on, it stays on the device, and nothing sends it anywhere. Each line names a monitor only by a short hashed id, such as `id-3f9c2a10`.

- **macOS:** the lines go to the unified log under the subsystem `com.telaviva.oled`, in the categories `brightness` and `ddc`. To read the last day in Terminal, run `log show --last 1d --predicate 'subsystem == "com.telaviva.oled"'`, or filter the same subsystem in Console.app. macOS decides how long the lines are kept, typically several days.
- **Windows:** the lines go to `%LOCALAPPDATA%\TelaVivaOLED\telaviva.log`. When the file reaches 1 MiB it is renamed to `telaviva.log.1`, and one previous generation is kept, so at most about 2 MiB remain. The default level is Notice. Set `TELAVIVA_LOG_LEVEL` to `info` or `debug` before you start the app to log more, or to `error` to log less. Uninstalling keeps these files unless you run the uninstaller with `-RemoveUserData`.

A wake leaves these events: the display sleep and wake lines, `display_power`, `display_state` with `state=awake`, one `attempt_failed` line for each failed DDC attempt and `attempt_recovered` once a later attempt succeeds, then `read_result`, `ownership_verified` and `restore_enqueued`. If the restore gives up, `restore_failure` counts up to `failures=3/3` and `ownership_dropped` follows. A monitor that left and came back shows `monitor_disconnected` and then `monitor_discovered`. If that monitor was dimmed by protection and returns at the dimmed level, the log shows `dim_resumed` after `monitor_discovered`, then `read_result`, `ownership_verified` and `restore_enqueued`. A return at another level shows no `dim_resumed`.

## Privacy

Everything runs locally and the app has no telemetry. Camera frames are processed in memory and are not stored, transmitted or logged, and device identifiers are not logged. Diagnostic logs show a monitor only as a short hashed id, never by its serial number, name or connection path.

The only network request is an anonymous HTTPS GET to the GitHub releases API, which looks for a newer version. It is made only when the About page is open or **Check now** is pressed. Apart from the standard HTTPS headers, it sends nothing but the app version in the `User-Agent` header, and it downloads nothing.

Third-party licenses are listed in [`licenses/DEPENDENCIES.md`](../licenses/DEPENDENCIES.md).
