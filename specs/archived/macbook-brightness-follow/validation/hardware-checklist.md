# Hardware validation checklist: MacBook brightness follow, popup sizing and Settings

**Feature**: macbook-brightness-follow
**Covers**: plan tasks T001 to T006, T020, T024, T031, T046, T053 and T058 (see [plan.md](../plan.md))
**Success criteria**: SC-001 to SC-007, SC-009 and SC-010 (see [spec.md](../spec.md)). SC-008 is covered by automated tests, not by this checklist.
**Status**: Not run. Every item starts as "Not run". Nothing here has been run yet.

Work through the sections in order. A person must be at the Mac for every step. Record each result in the matching validation note named in plan.md (for example `spikes.md`, `popup-and-settings.md`, `protection.md`, `follow.md`, `calibration.md`). Do not mark a criterion as passed until its steps have been run on hardware.

## Safety note

Read this before any section.

- The setter checks in section A change the built-in display brightness and the macOS automatic brightness setting. Sections E and F also change them. Both are restored afterwards, but only if you record the starting values first.
- Before each section that changes brightness, write down:
  - the built-in display brightness, from System Settings > Displays;
  - whether automatic brightness is on or off, from System Settings > Displays;
  - the brightness of each external monitor, from its own on-screen menu.
- Restore the starting values when the section ends. Check them again with the same method.
- Run the setter probes in A1 and A2 only with TelaViva OLED quit, so the app does not change brightness during the probe.
- Quit TelaViva OLED and copy `~/Library/Application Support/TelaVivaOLED/settings.json` to a safe place before sections D, E, F and G. Keep the copy until the checklist is finished.
- The probes in section A are scratch code outside the repository. Do not add them to the repository.

## Prerequisites

- An Apple Silicon Mac running macOS 14 or later. Record the Mac model and the exact macOS version in each Notes line.
- At least one external DDC-capable monitor. Section B needs up to four. DDC support depends on the monitor and the connection, so record the monitor model and the port, adapter or dock used.
- Camera access, for presence protection in section D.
- Accessibility and Input Monitoring access for keyboard routing, for sections E and G. See the Permissions part of [packaging/macos/README.md](../../../packaging/macos/README.md).

## Build and run commands

Run these from the repository root, `/Users/cassio/Documents/GitHub/telaviva-oled`.

One-command build (from README.md):

```sh
./scripts/build-macos.sh
```

Lower-level steps (from packaging/macos/README.md), if the one-command build is not used:

```sh
cargo build --workspace --release --target aarch64-apple-darwin --locked
TARGET_DIR="$(cargo metadata --locked --no-deps --format-version 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["target_directory"])')"
TELAVIVA_CORE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_core.a" \
TELAVIVA_INFERENCE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_inference.a" \
  packaging/macos/build-app.sh
```

Open the built app (from README.md):

```sh
open 'build/macos/TelaViva OLED.app'
```

Diagnostics (from packaging/macos/README.md). `--diagnostics` prints discovery and ID information. `--list-displays` performs read-only DDC reads of brightness and volume and does not change monitor settings:

```sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --diagnostics
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --list-displays
```

Quit the app from its menu-bar menu. Use Activity Monitor only where a step says to force quit.

---

## A. Spikes (plan Phase 1: T001 to T006)

Each spike below is a subsection. Record every result in `specs/archived/macbook-brightness-follow/validation/spikes.md` (T006). If a spike fails, the matching Decision in `architecture.md` must be amended. Do not amend it from this checklist.

### A1. S1: DisplayServicesSetBrightness on the built-in display (T001)

Status: Not run

Preconditions: TelaViva OLED is quit. A scratch probe, outside the repository, calls `DisplayServicesGetBrightness` and `DisplayServicesSetBrightness` on the built-in display. The level is a float from 0.0 to 1.0.

Steps:

1. Quit TelaViva OLED.
2. Read the built-in display level with the probe (`DisplayServicesGetBrightness`). Also note the percentage shown in System Settings > Displays. Call this starting level L.
3. If L is 95 or less, set the level to L + 5 points. If L is above 95, set it to L - 5 points. Record the return status of `DisplayServicesSetBrightness` (0 means success).
4. Read the level back with the probe immediately, and again after 2 seconds. Record both values.
5. Watch the built-in screen and System Settings > Displays. Record whether the level moved, and by how much.
6. Restore level L with the same call. Record the return status.
7. Read the level back. Confirm it matches L and that System Settings shows L.

Expected:

- Both return statuses are 0.
- Each read-back matches the requested value to within 1 point. If the difference is larger, record the actual difference.
- The built-in screen moves by about 5 points and returns to L.
- No external monitor changes during the probe.

Result: 

Notes: 

### A2. S2: Automatic brightness through the CoreBrightness setter (T002)

Status: Not run

Preconditions: TelaViva OLED is quit. A scratch probe, outside the repository, calls `setAutoBrightnessEnabled:error:` and `isAutobrightnessEnabledWithError:` on `CBDisplayBrightnessClient`, reached through `CBClient`.

Steps:

1. Quit TelaViva OLED.
2. Open System Settings > Displays. Record whether "Automatic brightness" is on or off. Call this state A.
3. In a terminal, run `/usr/libexec/corebrightnessdiag status-info` and save the output to a file outside the repository. This command is read-only.
4. With the probe, read the current automatic brightness state. Then set it to the opposite of A. Record the returned BOOL and any NSError.
5. Check System Settings > Displays. Record whether the checkbox changed.
6. Run `/usr/libexec/corebrightnessdiag status-info` again. Save the output and record whether it changed.
7. With the probe, restore state A. Record the returned BOOL and any NSError.
8. Check System Settings and `corebrightnessdiag status-info` again. Both must show state A.

Expected:

- The setter returns YES with no error, in both directions.
- System Settings and `corebrightnessdiag status-info` agree on the new state after each change.
- After the restore, both show state A.
- If the setter returns NO or an error, or the two sources disagree, record it. The calibration notice path (FR-022) then applies, and the result is a fail for the setter.

Result: 

Notes: 

### A3. S3: brightness-change notification (optional, T003)

Status: Not run (optional). Polling is used today (architecture Decision 6). This spike only decides whether push notification can be added.

Preconditions: TelaViva OLED is quit. A scratch probe, outside the repository, registers `DisplayServicesRegisterForBrightnessChangeNotifications` for the built-in display and logs every callback.

Steps:

1. Quit TelaViva OLED and start the probe.
2. Record the callback signature you determined from the header or from reflection.
3. Press the MacBook brightness keys three times up and three times down. Note the time of each press and the callbacks logged.
4. Change the level with the Control Center brightness slider. Note the callbacks logged.
5. If automatic brightness is on, move the Mac to a brighter or darker place, or cover and uncover the ambient light sensor. Note the callbacks logged during the ramp.
6. Stop the probe.

Expected:

- Record whether callbacks fire for key presses, for Control Center changes and for automatic ramps, and how many fire per change.
- If callbacks fire reliably for all three, record that push notification with a slow safety poll (10 seconds, Decision 6) is possible. If not, record that polling stays.
- The decision is written in `spikes.md`.

Result: 

Notes: 

### A4. S4: monitor ID across two ports and a dock (T004)

Status: Not run

Preconditions: one external monitor. A dock is optional.

Steps:

1. Connect the monitor directly to port P1 on the Mac, not through a dock.
2. Run `'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --diagnostics` and save the output to `/tmp/diag-port1.txt`.
3. Note the monitor ID printed, and each component of it that the output shows (for example vendor, product, serial, EDID or display UUID, connection path).
4. Disconnect the monitor. Connect it to port P2. Use a different port if the Mac has one that is not P1.
5. Run `--diagnostics` again and save the output to `/tmp/diag-port2.txt`.
6. If a dock is available, connect the monitor through the dock, run `--diagnostics` again and save the output to `/tmp/diag-dock.txt`.
7. Compare the saved outputs. List each ID component that changes between connections.

Expected:

- A list of the ID components that change between P1, P2 and the dock, and those that stay the same.
- This is not a pass or fail check. The result feeds architecture Decision 4. If a dock is not available, record that the dock case was not run.

Result: 

Notes: 

### A5. S5: NSPopover resize with SwiftUI content (T005)

Status: Not run

Preconditions: a scratch SwiftUI app, outside the repository, that shows an `NSPopover` whose content is an `NSHostingController` with `sizingOptions = .preferredContentSize`. Run it on macOS 14 or later and record the version.

Steps:

1. Start the scratch app and open the popover from its status item.
2. Add a button that adds rows to the SwiftUI content, and another that removes them.
3. With the popover open, add rows, then remove them. Observe whether the popover height follows the content after each change.
4. Repeat on the macOS version you use for the other sections, if it differs.

Expected:

- On macOS 14 or later, the popover height follows the SwiftUI content.
- If it does not, record that. The documented fallback is to set `popover.contentSize` from the measured height (architecture Decision 11 and Risks table). Record which approach is used.

Result: 

Notes: 

---

## B. Popup sizing with 0, 1, 2, 3 and 4 external monitors (SC-001; plan T020)

Status: Not run

**If fewer monitors are available.** Run the counts you have, in any order. Record every count you did not run as "Not run (monitors unavailable)" in Notes. Do not simulate extra monitors. SC-001 stays pending acceptance for the counts that were not run.

Steps, repeated for each count N (0, 1, 2, 3, 4):

1. Connect N external monitors. For N = 0, disconnect every external monitor. Wait until the monitors appear, or disappear, in the popup.
2. Click the monitor icon in the menu bar to open the popup.
3. For N = 1, 2 and 3: check that every card is visible without scrolling, and that no empty band below the last card is larger than a normal margin.
4. For N = 4: check that the popup height equals the height of three cards, and that the list scrolls to reach the fourth card.
5. For N = 0: check that the empty state shows and that the popup keeps a sensible minimum size (FR-003).
6. Check that the popup never extends below the visible screen area (FR-005).
7. Turn screensaver protection on in Settings, then reopen the popup. Check that the card height changes consistently with the protection row. Turn protection back to its starting value.
8. Close the popup and reopen it. Check that the size is unchanged.
9. Record N, the height behavior, and any screenshot path in Notes.

Expected (per SC-001):

- With 1, 2 and 3 monitors, every card shows with no scrolling and no large empty band.
- With 4 or more monitors, the popup shows exactly three cards and scrolls.
- The popup stays within the visible screen area.

Result: 

Notes: 

---

## C. Settings window: tabs, resize, save round trip and restart (SC-002; plan T024)

Status: Not run

Steps:

1. Open Settings from the popup or the menu-bar menu.
2. Check that the window opens at about 640 by 720 points.
3. Check that the window can be resized. Shrink it until it stops, and check that the minimum is about 560 by 560. Return it to the default size.
4. Check that the tabs are General, Keyboard and Automatic brightness, in that order.
5. In General, check that the language, launch at login, OLED protection (including the connected-monitor list with its toggles, when present) and camera availability are all visible without clipped controls or inner scrolling.
6. In Keyboard, check that the hardware keys section is fully visible at the default size.
7. In Automatic brightness, check that the enable switch, the monitor list, the explanation text and any calibration controls are fully visible at the default size.
8. Save round trip. In General, change one value. In Keyboard, change the brightness key step to a different value. If the OLED protection monitor list is shown, toggle one monitor's protection. Use the save action in Settings.
9. Close the Settings window and reopen it. Check that all three changes are still there.
10. Quit TelaViva OLED, relaunch it and open Settings. Check that all three changes are still there.
11. Restore the starting values and save again.

Expected (per SC-002):

- Each tab shows its full content at the default size, with no clipped control and no inner scrolling.
- The window resizes within the stated limits, and content stays usable at the minimum size.
- The changes survive closing the window and restarting the app.

Result: 

Notes: 

---

## D. Per-monitor protection (SC-009, SC-010; plan T031)

Status: Not run

Preconditions: two external monitors, A and B. With one monitor, run the toggle, restart and reconnect steps only, and record the two-monitor steps as Not run. Presence protection needs camera access. Screensaver protection is turned on in Settings. Back up `settings.json` first (see the safety note).

Steps, screensaver case:

1. In Settings, turn screensaver protection on.
2. Open the popup. Keep protection on for both monitors.
3. Start the screensaver with your usual method. Check that both monitors dim.
4. While the monitors are dimmed, turn monitor A's protection toggle off in the popup. Check that A returns to its brightness from before the dim within one evaluation cycle. Check that B stays dimmed.
5. Stop the screensaver. Check that B restores.
6. Turn A's protection back on. Check that A does not dim at once.

Steps, presence case:

7. In Settings, turn presence protection on and grant camera access if asked. Sit in front of the camera.
8. Turn monitor A's protection toggle off in the popup. Confirm that A's "Protection brightness" row is hidden.
9. Move out of the camera view for two valid no-face samples. With the default 10-second interval, allow about 30 seconds. Check that B dims and A does not.
10. Turn A's protection back on while B is still dimmed. Check that A does not dim immediately.
11. Return to the camera. Check that B restores.

Steps, restart and reconnect:

12. Set A's protection to off. Quit TelaViva OLED and relaunch it. Check that A's toggle is still off, and B's toggle is still on.
13. Unplug monitor A and plug it back in to the same port. Check that A reappears with the same toggle state.
14. If A is connected to a different port or through a dock than before, record the new ID as the expected limitation in architecture Decision 4. This is not a failure.
15. Restore the starting protection settings.

Expected (per SC-009 and SC-010):

- A monitor with protection off is never dimmed by presence or screensaver protection.
- Turning protection back on resumes it within one evaluation cycle, with no immediate dim.
- The toggle state survives restart and reconnection on the same port with no user action.

Result: 

Notes: 

---

## E. Follow the MacBook brightness (SC-003; plan T046)

Status: Not run

Preconditions: one calibrated monitor, with a complete curve. Either finish section F first, or, with TelaViva OLED quit, hand-edit the settings file as plan T046 describes:

1. Quit TelaViva OLED. Back up `~/Library/Application Support/TelaVivaOLED/settings.json`.
2. Get the monitor ID from the `--diagnostics` output (section A4 or section B).
3. Add the entries below to the settings file, using the real monitor ID. Keep the file valid JSON.

```json
"ambient_follow_enabled": true,
"ambient_curves": { "<monitor-id>": { "points": [ { "monitor": 25, "builtin": 18 }, { "monitor": 50, "builtin": 40 }, { "monitor": 75, "builtin": 66 }, { "monitor": 100, "builtin": 100 } ] } }
```

4. Start the app. If the app reports that the settings file is corrupt, quit it and restore the backup.

In the Automatic brightness tab, turn the follow switch on. Record the curve used in Notes.

Steps:

1. In the Automatic brightness tab, check that the monitor is listed as eligible for follow. Record the result in Notes.
2. Record the MacBook built-in level and the monitor's brightness, read from its on-screen menu.
3. Press the MacBook brightness down key five times. Wait 3 seconds. Check that the monitor moves to the value the curve gives for the new MacBook level (the Notes line shows the curve). Time the change from the last key press.
4. Press the brightness up key five times and repeat the check.
5. Change the brightness with the Control Center brightness slider. Repeat the check and timing.
6. In System Settings > Displays, turn on automatic brightness. Move the Mac to a place with a different light level, or cover and uncover the ambient light sensor. Watch the monitor follow the ramp. Check that it stops within about 2 seconds after the built-in level stops changing.
7. After the ramp ends, keep the MacBook level fixed for 60 seconds. Check that the monitor does not move. Watch for repeated small up and down steps, which would be oscillation.
8. Check the idle CPU. With the feature on, leave the Mac idle for 60 seconds. Open Activity Monitor and record TelaViva OLED's CPU use over 30 seconds.
9. Turn the follow switch off in the Automatic brightness tab. Repeat the idle CPU reading. Turn follow back on afterwards.
10. Lid closed with an external display. Keep the Mac awake with the lid closed and the external display connected, as in your normal setup. Check the following: the Automatic brightness tab explains that the built-in display is unavailable; the monitor keeps its last brightness; the popup shows brightness sliders again for the monitor.
11. Open the lid. Change the MacBook brightness. Check that follow resumes and the monitor moves within 2 seconds.
12. Display sleep and wake. Put the displays to sleep with your usual method, or wait for display sleep. Check that the monitor does not change while the displays are asleep. After wake, check that the monitor returns to the value for the current MacBook level within 2 seconds.
13. Quit TelaViva OLED and relaunch it. Check that follow is still on, and that the monitor matches the curve for the current MacBook level within 2 seconds of the first update.

Expected (per SC-003):

- Each calibrated monitor reaches its mapped value within 2 seconds of a MacBook change, from keys, Control Center or an automatic ramp.
- There is no oscillation: the monitor does not hunt or flicker after a change ends.
- Idle CPU is low and does not stay high with the feature on. Record the values for both states.
- With the built-in display unavailable, follow stops, the monitor keeps its last value, and the popup shows its brightness slider again.

Result: 

Notes: 

---

## F. Calibration: add, re-measure and remove user-managed points, two-point minimum, cancel, done, switch, window close, force quit and relaunch, screen off or lid closed (SC-004, SC-006; plan T053)

Status: Not run

Preconditions: one external monitor to calibrate. Before starting, record:

- the built-in display level, L;
- automatic brightness, state A, from System Settings > Displays;
- the monitor's brightness, from its on-screen menu.

Direction: each point is a **monitor** brightness (any whole percentage from 1 to 100% of the monitor's range, up to 32 points). Adding a point or choosing a saved point sets the monitor to that brightness and shows a MacBook brightness slider. Moving that slider sets the built-in display. The value saved for a point is the MacBook level at which the two screens look the same.

Steps, add, re-measure and remove points:

1. Open Settings > Automatic brightness and select the monitor. If the monitor was calibrated with an earlier development build, recalibrate it in this section before relying on follow. The points saved by that build are MacBook levels, so they give wrong monitor levels until the monitor is calibrated again. Record in Notes whether a recalibration was needed. Record the saved points before you start. Check that each saved point, including points saved in the previous fixed-key format, appears as a row in ascending order of monitor percentage, with its MacBook percentage.
2. Add a point. In the Add point row, set the stepper to a whole percentage that is not saved yet; by default it is the first unused of 25, 50, 75 and 100%. Check that Add is disabled when the chosen percentage is already saved. Press Add. Check that the monitor moves to that percentage of its brightness, read from its on-screen menu, that the calibration dialog opens with the MacBook brightness slider, and that automatic brightness is off in System Settings. The built-in display moves to the slider's starting value: the saved level for a saved point, or the level when the session started for a new point. If automatic brightness cannot be turned off, check that the notice says the level may drift. Move the MacBook brightness slider until the MacBook screen looks the same as the monitor. Check that Done stays disabled, with a waiting hint, until the monitor has reached the chosen percentage, and is enabled after that. Press Done. Check that the dialog closes, that the monitor, the built-in display and automatic brightness return to their starting values, and that a row shows the new point with the MacBook percentage set on the slider.
3. Out of order, re-measure and existing points. Add a percentage lower than an existing row and save it. Check that the new row is inserted at its sorted position and that the MacBook percentages do not decrease down the list. Press Calibrate on a saved row, move the slider in the dialog, and press Done. Check that only that row's MacBook percentage changes and that the number of rows stays the same. Close Settings and reopen it. Check that all saved points are still listed in ascending order with the same values.
4. Remove and the two-point minimum. Press Remove on a saved row that is not open in the dialog. Check that the row disappears and the other rows are unchanged. Open a saved point in the dialog and check that Remove is disabled for that row. Start a point on a monitor with one saved point. Check that the hint that at least two points are needed is shown in the list, and that Done is enabled once the monitor has reached the point's brightness, without a second saved point. Press Done and check that the point is saved and the hint is no longer shown.
5. Removing down to one point stops following. With follow on and exactly two saved points, remove one. Check that the Automatic brightness tab reports the monitor as not calibrated, that the popup shows its brightness slider again, and that the monitor stops following the MacBook brightness. Add the point back and check that the monitor is eligible again.
6. Neighbor bounds and the cap above the last point. Select a point that has a saved neighbor below it, and move the MacBook brightness slider below that neighbor's MacBook level. Check that the slider stops at that level. Repeat at the upper neighbor if there is one. With follow on and the last point below 100%, set the MacBook brightness above that point's MacBook level. Check that the monitor stays at that point's percentage, read from its on-screen menu, and does not go to 100%. Add a 100% point and check that the monitor reaches 100% when the MacBook level is at its maximum.

Steps, cancel:

7. Select a point that is not yet calibrated, or a calibrated point. Move the MacBook brightness slider. Press Cancel. Then repeat the steps on another point and press Escape instead of Cancel.
8. Check that the point is unchanged. Check that the monitor returns to its starting brightness, that the built-in display returns to L, and that automatic brightness returns to state A.

Steps, done:

9. Select a saved point, move the MacBook brightness slider to a new value and press Done.
10. Check that the point now shows the MacBook percentage of the slider value, and that the monitor, the built-in display and automatic brightness return to their starting values.

Steps, switch monitor:

11. Select a point on monitor A. Switch to monitor B, or move to another tab.
12. Record in Notes what happens to the session: whether it continues for B, or ends and restores. Then press Done or Cancel as appropriate, and check that the built-in display, automatic brightness and both monitors return to their starting values.

Steps, window close:

13. Select a point. Close the Settings window before pressing Done or Cancel.
14. Reopen Settings. Check that the built-in display, automatic brightness and the monitor have returned to their starting values.

Steps, force quit and relaunch:

15. Select a point, so the monitor is set to that level, the MacBook brightness slider is shown and automatic brightness is off.
16. In Activity Monitor, select TelaViva OLED and choose Force Quit.
17. Relaunch the app. Check that at launch the built-in display returns to L, automatic brightness returns to state A, and the monitor returns to its starting brightness.
18. Check that `~/Library/Application Support/TelaVivaOLED/calibration-session.json` no longer exists. Use `ls` in a terminal to check.
19. Record in Notes whether the app reported a recovery failure in the Automatic brightness tab.

Steps, MacBook screen off or lid closed:

20. Turn the MacBook screen off (display sleep, with your usual method or `pmset displaysleepnow`), or close the lid, with the external monitor still connected. Open Settings > Automatic brightness and select the monitor. Check that the calibration controls (the point rows, the Add point row, and the calibration dialog with its MacBook brightness slider, Done and Cancel) are hidden, and that the reason for the built-in display being unavailable is shown.
21. With the screen still off or the lid still closed, try to start a point. Check that no point can be started. If a Calibrate or Add button is still visible because the app has not yet noticed the change, press it. Check that the notice "Calibration is not possible while the MacBook screen is off or the lid is closed." is shown, and that the monitor, the built-in display and automatic brightness do not change.
22. Turn the MacBook screen on or open the lid. Check that the calibration controls appear again.

Steps, MacBook screen off or lid closed during a calibration:

23. Select a point, so a calibration is running: the monitor is set to that level, the MacBook brightness slider is shown and automatic brightness is off. Then turn the MacBook screen off or close the lid, with the external monitor still connected.
24. Check that the calibration ends and that the Automatic brightness tab shows the notice "Calibration is not possible while the MacBook screen is off or the lid is closed." Check that the point is unchanged (not saved).
25. Check that the monitor returns to its starting brightness. Once the screen is back or the lid is open, check that the built-in display returns to L, that automatic brightness returns to state A, and that `calibration-session.json` no longer exists. Record in Notes whether the built-in display returned to L. The restore write to the built-in display can fail while the screen is off; if the level did not return, record the level it is at and do not count the other restores as failed.

Expected for the user-managed points (no SC criterion yet; pending hardware acceptance):

- A monitor with fewer than two saved points does not follow the MacBook brightness, and the list shows the hint that at least two points are needed.
- Points are listed in ascending order of monitor percentage, and a point added out of order is inserted at its sorted position.
- Re-measuring a point replaces only its MacBook percentage. Removing a point deletes only that point.
- Removing points down to one stops following, and the monitor reports as not calibrated.
- Each chosen percentage sets the monitor to that percentage of its brightness, and Done is enabled only after the monitor has reached it.
- Above the last saved point, the monitor stays at that point's percentage.
- Points saved in the previous fixed-key format are listed and kept, without recalibration.

Steps, final check:

26. Check the built-in display level, automatic brightness state and monitor brightness against the values recorded before the section started.

Expected (per SC-004 and SC-006):

- At each saved point, the monitor is set to that point's percentage of its brightness, and the saved MacBook levels are monotonic: each point is at or above the point below it.
- Save is enabled only after the monitor has reached the point's level.
- After each of done, cancel, window close, switch and force quit, the built-in display, automatic brightness and monitor return to their starting values.
- No calibration leaves the monitor at an unintended level after cancel.
- The session file is removed after each end.
- With the MacBook screen off or the lid closed, the calibration controls are hidden and no point can be started.
- Turning the screen off or closing the lid during a calibration ends it with the notice, leaves the point unsaved and restores the monitor. The built-in display and automatic brightness are restored once the screen is back; whether the built-in display level returned is recorded in Notes.

Result: 

Notes: 

---

## G. Popup and keys in follow mode (SC-007, FR-016, FR-026, FR-027; plan T058)

Status: Not run

Preconditions: follow is on, with at least one followed monitor (calibrated, eligible). For the uncalibrated check, you need a second monitor that is not calibrated or does not support DDC brightness. With one monitor, record the uncalibrated checks as Not run. Keyboard routing is enabled in Settings, with Accessibility and Input Monitoring granted.

Steps:

1. Open the popup with follow on. Check that the followed monitor's card shows its volume control, and shows the "Protection brightness" row only if screensaver protection is on. Check that it shows no brightness slider.
2. Check that the uncalibrated monitor's card still shows its brightness slider.
3. Turn follow off in the Automatic brightness tab. Check that the followed monitor's brightness slider returns. Turn follow back on.
4. Keys over the followed monitor. Move the cursor over the followed monitor. Press the brightness keys down and up. Check that the MacBook screen changes, and that the followed monitor follows the new level within 2 seconds. Check that the other monitor does not change.
5. Keys over the uncalibrated monitor. Move the cursor over the uncalibrated monitor. Press the brightness keys. Check that this monitor changes by the step set in Keyboard (5 points by default), as it did before this feature, and that the MacBook screen does not change.
6. Keys over the built-in display. Move the cursor over the MacBook screen. Press the brightness keys. Check that normal macOS behavior applies, as the README describes.
7. Volume and mute. With the cursor over a different display, press the volume keys and the mute key while an external monitor is the Mac's sound output (System Settings > Sound). Check that the routing matches the README description, and that it is unchanged from before this feature.
8. Protection brightness on a followed monitor. With screensaver protection on, change the "Protection brightness" value of a followed monitor. Check that the slider works without a brightness control, and that it cannot exceed the monitor's current brightness.
9. Card height. Check that the followed monitor's card has no empty band where the brightness row was, and that the popup height matches the rule in section B.

Expected (per SC-007):

- A followed monitor shows volume, and the protection row if protection is on, but no brightness slider.
- An unfollowed monitor keeps its brightness slider, so it is never uncontrollable.
- Brightness keys over a followed monitor change only the MacBook, and the monitor follows.
- Brightness keys over an unfollowed monitor behave as they did before this feature.
- Volume and mute are unchanged.

Result: 

Notes: 

---

## H. Mac without a built-in display (SC-005)

Status: Not run. Unverified unless a Mac without a built-in display is available (for example a Mac mini). Mark this section as unverified in the validation notes if no such Mac is available. Do not infer the result from another Mac.

Steps:

1. Run the app on a Mac without a built-in display, with at least one external monitor connected.
2. Open Settings > Automatic brightness.
3. Check that the enable switch is shown but disabled.
4. Check that the explanation is shown, for example "This Mac has no built-in display with adjustable brightness".
5. Try to turn the switch on with the mouse and with the keyboard. Check that it cannot be turned on.
6. Check that the General and Keyboard tabs are unchanged in this state.
7. Open the popup. Check that every monitor shows its brightness slider.

Expected (per SC-005):

- The feature cannot be enabled.
- The reason is displayed.
- Brightness sliders stay on every monitor.

Result: 

Notes: 

---

## I. Pause while the built-in screen is off

Status: Not run

Steps:

1. Turn on follow with a calibrated monitor. Check that the popup shows no brightness slider for it.
2. Close the lid with the external monitor still connected. In the popup, reached from the external display, check that the brightness slider is back, and that the Automatic brightness tab in Settings explains that the built-in display is off.
3. With keyboard routing enabled, press the brightness keys. Check that they now change the external monitor.
4. Open the lid. Check that the brightness slider disappears again, that the brightness keys pass through to macOS again, and that the monitor goes to the level matching the MacBook.
5. Repeat with the lid open. Let only display sleep turn the screen off (for example with a short display sleep or `pmset displaysleepnow`), then wake it. Check the pause and resume as in steps 2 to 4.
6. Change the monitor brightness manually during the pause, from its on-screen menu. Check that following replaces that change when it resumes.

Expected:

- Follow pauses and resumes by itself, and manual control returns during the pause.

Result: 

Notes: 

---

## Summary

| Section | Scope | Status | Date |
|---------|-------|--------|------|
| A | Spikes S1 to S5 (T001 to T006) | Not run | |
| B | Popup sizing, 0 to 4 monitors (SC-001, T020) | Not run | |
| C | Settings tabs, resize, save round trip and restart (SC-002, T024) | Not run | |
| D | Per-monitor protection (SC-009, SC-010, T031) | Not run | |
| E | Follow the MacBook brightness (SC-003, T046) | Not run | |
| F | Calibration (SC-004, SC-006, T053) | Not run | |
| G | Popup and keys in follow mode (SC-007, T058) | Not run | |
| H | Mac without a built-in display (SC-005) | Not run (unverified unless such a Mac is available) | |
| I | Pause while the built-in screen is off | Not run | |
