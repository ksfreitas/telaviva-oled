# Windows hardware and acceptance checklist

Every row starts as **Not run**. A row changes only when someone runs it, with the device, driver and date. The README and release notes may claim only rows marked Pass.

Machine used so far: Windows 11 Pro build 26300, desktop, AMD Ryzen 7 9800X3D, AMD Radeon iGPU, NVIDIA RTX 4080 SUPER, one external DELL S3225QC over HDMI, cameras: Lenovo Performance RGB and IR. No NPU, no battery, no internal panel.

## DDC and monitors

| # | Row | Status | Notes |
|---|-----|--------|-------|
| D1 | Monitor listed with a stable id (`--list-displays`) | Pass (2026-10-17) | DELL S3225QC, id `win-DEL-53688-808858963-a9c404a1`; see `spike-s2-identity.md` |
| D2 | DDC read of brightness and volume (`--probe-ddc`) | Pass (2026-10-17) | 35 and 1 at 59 and 71 ms over HDMI |
| D3 | DDC brightness write with readback verification, then restore (owner present) | Not run | task T042 |
| D4 | DDC volume write with readback verification, then restore | Not run | task T042 |
| D5 | Flyout slider changes the real monitor within one second and the value label follows | Not run | needs D3 |
| D6 | Unplug and replug the monitor: card disappears and returns | Not run | |
| D7 | Display sleep and wake: no writes while asleep, fresh readback on wake, state restored | Not run | |
| D8 | Resume from sleep: DDC works after the 2 s grace | Not run | |
| D9 | Two monitors, identical monitors (ambiguous), dock, DisplayPort, USB-C | Not run | no second monitor available yet |
| D10 | HDR on and off | Not run | |

## Flyout and Settings

| # | Row | Status | Notes |
|---|-----|--------|-------|
| U1 | Flyout visual with live data in dark theme | Pass (automated capture, 2026-10-17) | `images/s11-flyout-real-data.png`; owner comparison with Quick Settings pending (T044) |
| U2 | Flyout in light theme, high contrast, 100%, 125%, 150%, 200% | Not run | 125% only so far |
| U3 | Flyout placement with the taskbar on each edge and on a second monitor | Not run | |
| U4 | Light dismiss when clicking elsewhere, Escape, focus order | Not run | |
| U5 | Tray icon visible in overflow and pinned, light and dark taskbar | Not run | owner to confirm |
| U6 | Narrator, keyboard-only pass, text scaling 225% | Not run | |
| U7 | Settings window: all pages in both languages | Partial (2026-10-17) | General, Keyboard and Automatic brightness pages open and expose accessible names in pt-BR (UI Automation dump); en-US and the rendered look not captured (session locked) |

## Presence, camera and power

| # | Row | Status | Notes |
|---|-----|--------|-------|
| P1 | Camera permission denied shows the status and does not dim | Not run | Phase 5 |
| P2 | Camera shared with a video call | Not run | |
| P3 | Two cameras (RGB and IR): IR hidden, RGB used | Not run | |
| P4 | Dim after two checks and restore on return, latencies recorded | Not run | |
| P5 | Low light does not dim | Not run | |
| P6 | Screensaver protection | Not run | |
| P7 | Battery suspension | Not run | desktop, needs a laptop |

## Acceleration and efficiency

| # | Row | Status | Notes |
|---|-----|--------|-------|
| A1 | CPU inference on the fixture | Pass (2026-10-17) | 5.27 ms mean, confidence 0.822190; `measurements.md` |
| A2 | DirectML on the Radeon iGPU and the RTX GPU: equal result, latency, energy | Partial (2026-10-17) | equal result and latency recorded; energy not measured (desktop); `measurements.md` |
| A3 | NPU | Not run | no NPU on this machine, out of the first release |
| A4 | Forced Efficiency mode: latencies | Not run | Phase 7 |
| A5 | Idle CPU and memory with the full app | Partial (2026-10-17) | all protections off (default): 0.000% of one core over 60 s, 58.8 MB working set, 28 threads; presence protection on (camera running) not measured; `performance.md` |

## Keys

| # | Row | Status | Notes |
|---|-----|--------|-------|
| K1 | Volume keys follow the monitor sound output, system volume does not move | Not run | Phase 8 |
| K2 | Mute and restore | Not run | |
| K3 | Keys pass through when speakers are the default output | Not run | |
| K4 | Behavior with an elevated foreground window | Not run | |

## Laptop

| # | Row | Status | Notes |
|---|-----|--------|-------|
| L1 | Desktop: Laptop follow disabled with the right reason, no writes | Pass (2026-10-17) | `TelaViva.exe --probe-laptop`: available=false, reason noBuiltInDisplay, no WMI read, wrote_to_panel=false; the Automatic brightness page shows "Este computador não tem tela de notebook integrada" (read through UI Automation in pt-BR; rendered image not captured because the session was locked) |
| L2 | Laptop follow, calibration, lid pause, crash recovery | Not run | needs a laptop |
| L3 | Adaptive brightness read and write | Not run | needs a laptop |

## Install and launch

| # | Row | Status | Notes |
|---|-----|--------|-------|
| I1 | Launch at login on a real sign-in | Not run | |
| I2 | Install and uninstall scripts in a clean profile | Partial (2026-10-17) | zip built (32.7 MB); installed into an isolated folder with this user profile: 282 files, uninstall key with icon, Start menu shortcut `IconLocation` = `TelaViva.exe,0`, installed `--smoke-test` exit 0, uninstall removes folder, key and shortcut and leaves user data and the Run value untouched, also when the process working directory is the install folder; not run in a clean profile or VM; the icon as drawn in Start, taskbar and Settings > Apps is not checked |
| I3 | SmartScreen behavior of the unsigned build | Not run | |

## Controls handle

Cases for the controls-handle change (`specs/core-command-queue/plan.md`, T048). Every row starts as **Not run**; none of them is verified until someone runs it on the hardware.

The macOS adapter cases will be added when the macOS shell moves to the handle. No macOS case is listed here.

| # | Row | Status | Notes |
|---|-----|--------|-------|
| C1 | Slider drag on an external monitor: the control follows the pointer, only the final value is written, no jump back | Not run | |
| C2 | Hold the volume key: the value accumulates and does not bounce back on a lagging readback | Not run | |
| C3 | Volume mute and restore returns the remembered level | Not run | |
| C4 | Change brightness with the monitor's own buttons while idle, then press a key: the step starts from the new value | Not run | |
| C5 | Put the monitor to sleep and wake it: no pending write replays, and a new drag works after the 2 s grace | Not run | |
| C6 | Unplug and replug the monitor mid-drag: the draft is dropped and a new drag works | Not run | |
| C7 | Slow-waking monitor (DDC not ready after wake): the restore is retried and the total retry time stays within about 7 s | Not run | |
| C8 | One non-GSM monitor and one GSM monitor (LG UltraFine reports `GSM`): the duplicate write is sent only for GSM | Not run | needs the LG UltraFine |
| C9 | Two monitors dragged at the same time: both reach their final values and neither is starved | Not run | needs two monitors |
| C10 | Quit while a write is in flight: the app exits without hanging | Not run | |
