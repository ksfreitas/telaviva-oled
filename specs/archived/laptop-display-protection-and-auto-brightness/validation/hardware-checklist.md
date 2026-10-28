# Hardware checklist: Laptop display protection and automatic brightness

**Feature**: laptop-display-protection-and-auto-brightness
**Covers**: built-in screen protection (presence, screensaver), automatic brightness off during protection, recovery
**Success criteria**: SC-001 to SC-006 of [spec.md](../spec.md)
**Status**: Pass (every macOS and Windows row marked by the owner on 2026-10-27; no values recorded)

## Safety note

Read this before any section.

- Before each section that changes brightness, write down:
  - the built-in display brightness, from System Settings > Displays;
  - whether "Automatically adjust brightness" is on or off, from System Settings > Displays;
  - the brightness of each external monitor, from its own on-screen menu.
- Restore both the built-in display brightness and the "Automatically adjust brightness" setting when the section ends. Check them again with the same method.
- Before the first section, quit TelaViva OLED and copy `~/Library/Application Support/TelaVivaOLED/settings.json` to a safe place. Keep the copy until the checklist is finished.

## Prerequisites

- An Apple Silicon MacBook.
- The app built from this branch, `laptop-display-protection-and-auto-brightness`.
- Optionally, one external DDC-capable monitor. Record the monitor model and the port, adapter or dock used.

## Build and run commands

Run these from the repository root, `/Users/cassio/Documents/GitHub/telaviva-oled`.

One-command build:

```sh
./scripts/build-macos.sh
```

The script writes the app to `build/macos/TelaViva OLED.app`. Run the app from that produced bundle:

```sh
open 'build/macos/TelaViva OLED.app'
```

## A. Listing and lid (SC-009, FR-002)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: TelaViva OLED runs from the built bundle. One external DDC-capable monitor is connected; record its model and the port, adapter or dock used. The Mac is on AC power. Nothing is dimmed.

Steps:

1. Click the TelaViva OLED icon in the menu bar to open the popup. Check that a card named "MacBook screen" is listed, with a brightness slider, next to the external monitor cards.
2. Open Settings from the popup, then the General tab. In the "OLED protection" section, check that the "Monitors in protection" list shows a "MacBook screen" toggle below the external monitor toggles.
3. Close the lid with the external monitor still connected (clamshell). Wait 30 seconds, which covers one availability refresh.
4. Open the popup. Check that the "MacBook screen" card is absent and that only the external monitor cards are listed.
5. In Settings > General, "OLED protection", check that the "MacBook screen" toggle is absent from the list.
6. Open the lid. Wait up to 30 seconds. Check that the "MacBook screen" card is back in the popup and the toggle is back in Settings > General. Record the time it took.
7. Record the Mac model and the macOS version in Notes.

Expected (per SC-009 and FR-002):

- With the lid open, the "MacBook screen" card and its protection toggle are listed in the popup and in Settings > General.
- With the lid closed and an external monitor attached, the MacBook screen is absent from both places.
- After the lid opens, the MacBook screen reappears within one availability refresh.

Result:

Notes:

---

## B. Presence dim and restore (SC-001, SC-002)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: In Settings > General, "OLED protection", "Dim when no face is detected" is on and the "MacBook screen" toggle is on. Camera access is granted. The option "Turn off the system's automatic brightness while this screen is protected" stays off for this section; section E covers it. Before you start, record the MacBook screen level and the state of "Automatically adjust brightness" in System Settings > Displays.

Steps:

1. Sit in front of the camera. Set the MacBook screen brightness to a known level, for example 60%, with the brightness keys or the slider in System Settings > Displays. Record it as L.
2. Open the popup. In the "MacBook screen" card, check that the protection switch is on (it appears while presence or screensaver protection is on) and that the brightness slider is at about L.
3. Step away from the camera so that no face is seen. Wait up to about 30 seconds, which covers the confirmation time with the default check interval.
4. Read the screen level in System Settings > Displays. Check that the MacBook screen dims to 0%. The card's brightness slider is locked and shows "Locked while the screen is protected." The slider may keep showing the level from before the dim, so use System Settings for the reading.
5. Stay away for 60 seconds. Check that the level stays at 0%.
6. Sit in front of the camera. Wait up to about 30 seconds. Check that the MacBook screen returns to L, as read in System Settings > Displays, and that the card's slider unlocks.
7. Per-screen switch: with the screen at L and not dimmed, turn off the "MacBook screen" protection switch in the card. Step away from the camera for 60 seconds. Check that the screen does not dim.
8. Turn the card's protection switch back on, and sit in front of the camera.
9. Restore the starting values recorded before the section, and check them in System Settings > Displays.

Expected (per SC-001 and SC-002):

- Within the confirmation time after you step away, the MacBook screen dims to 0% and stays there while no face is seen.
- When you return, the screen goes back to L, not to the dimmed level, and the card's slider unlocks.
- With the MacBook screen protection switch off, the screen does not dim while no face is seen.

Result:

Notes:

---

## C. Screensaver dim and restore, with Protection brightness (SC-002)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: In Settings > General, "OLED protection", "Dim while the screensaver is active" is on and the "MacBook screen" toggle is on. Presence protection can stay off for this section. Record the MacBook screen level L and the starting value of the card's "Protection brightness" slider.

Steps:

1. In Settings > General, "OLED protection", turn on "Dim while the screensaver is active". Keep the "MacBook screen" toggle on.
2. Open the popup. In the "MacBook screen" card, check that the "Protection brightness" row is shown. It appears only while screensaver protection is on and the screen is protected. Drag its slider to 30%, and check that its label reads 30%.
3. Close the popup. Set the screen to L, and record L in System Settings > Displays.
4. Start the screensaver with your usual method.
5. Wait for the screensaver to start, then check in System Settings > Displays that the MacBook screen dims to 30%. Record the delay in Notes.
6. Stop the screensaver by moving the mouse or pressing a key. Check that the MacBook screen returns to L, as read in System Settings > Displays.
7. Set "Protection brightness" back to its starting value, and record that value in Notes.

Expected (per SC-002):

- When the screensaver starts, the MacBook screen dims to the "Protection brightness" value, 30% in this section.
- The card's brightness slider is locked while the screen is dimmed.
- When the screensaver stops, the screen returns to L, the value before protection.

Result:

Notes:

---

## D. Option off, the default: automatic brightness is never changed (SC-006)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: The option "Turn off the system's automatic brightness while this screen is protected" is off, which is the default. Presence protection is on with the "MacBook screen" toggle on, as in section B. Before you start, record the MacBook screen level L and the state of "Automatically adjust brightness", call it A, in System Settings > Displays.

Steps:

1. In Settings > General, "OLED protection", check that the option is off. If it is on, turn it off. The option is disabled while the "MacBook screen" toggle is off.
2. Record A, on or off, and record L.
3. Run section B, steps 3 to 6: step away until the screen dims to 0%, stay away for 60 seconds, then return until it restores.
4. While the screen is dimmed, check that "Automatically adjust brightness" in System Settings > Displays still shows A.
5. Optional, read-only: in a terminal, run `/usr/libexec/corebrightnessdiag status-info` before the dim and while it is dimmed. Check that its automatic brightness state does not change.
6. While dimmed, run `ls "$HOME/Library/Application Support/TelaVivaOLED/"` in a terminal. Check that `auto-brightness-hold.json` is not listed.
7. Run `log show --last 1h --predicate 'subsystem == "com.telaviva.oled"' | grep auto_brightness`. Check that no line is printed, because the app never turned automatic brightness off.
8. After the restore, check that the MacBook screen is at L and that "Automatically adjust brightness" is still A.

Expected (per SC-006):

- The "Automatically adjust brightness" state is the same before, during and after the dim.
- `auto-brightness-hold.json` does not exist while the screen is dimmed.
- The log has no `auto_brightness_` line for this section.
- The MacBook screen dims and restores as in section B.

Result:

Notes:

---

## E. Option on: automatic brightness off while dimmed, held for 10 minutes, then restored (SC-001, SC-002)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: Presence protection is on with the "MacBook screen" toggle on. Record the MacBook screen level L. This section runs twice: first with automatic brightness initially on, then with it initially off. Record the starting state of the option, which should be off.

Steps, automatic brightness initially on:

1. In System Settings > Displays, turn "Automatically adjust brightness" on. Record it as A, on.
2. In Settings > General, "OLED protection", turn on "Turn off the system's automatic brightness while this screen is protected". Check that the option is enabled, not greyed out.
3. Set the MacBook screen to L, and record L.
4. Step away from the camera. Check that within the usual confirmation time the MacBook screen dims to 0% and "Automatically adjust brightness" turns off in System Settings > Displays. Record the delay.
5. In a terminal, run `ls "$HOME/Library/Application Support/TelaVivaOLED/"`. Check that `auto-brightness-hold.json` and `builtin-protection-session.json` are both listed.
6. Stay away for 10 minutes. Check at the start, at 5 minutes and at 10 minutes that the screen is still at 0% and "Automatically adjust brightness" is still off.
7. Sit in front of the camera. Check that the MacBook screen returns to L, as read in System Settings > Displays, and that "Automatically adjust brightness" is on again.
8. Run the `ls` command again. Check that both files are gone.

Steps, automatic brightness initially off:

9. In System Settings > Displays, turn "Automatically adjust brightness" off. Record it as A, off.
10. Set the screen to L. Step away until the screen dims to 0%. Check that "Automatically adjust brightness" stays off.
11. Run the `ls` command. Check that `auto-brightness-hold.json` is not listed.
12. Sit in front of the camera. Check that the screen returns to L and that "Automatically adjust brightness" is still off.
13. Set the option back to its starting state, off, and restore the starting values recorded before the section.

Expected (per SC-001 and SC-002):

- Within the usual confirmation time after you step away, the screen dims to 0% and automatic brightness turns off.
- For at least 10 minutes, the screen stays at 0% and automatic brightness stays off, with no brightening by macOS.
- After you return, the screen returns to L and automatic brightness is on again.
- With automatic brightness initially off, it stays off during and after protection, and no hold file is created.

Result:

Notes:

---

## F. Follow interplay with a calibrated external monitor (SC-004, FR-009)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: One external monitor with a complete calibration curve saved in Settings > Automatic brightness. Follow is on, using the switch "Follow the MacBook screen brightness" in the Automatic brightness tab. The option is on, presence protection is on with the "MacBook screen" toggle on, and "Automatically adjust brightness" is on at the start. If no complete calibration exists, calibrate the monitor first with section F of the archived checklist (`specs/archived/macbook-brightness-follow/validation/hardware-checklist.md`) and record that in Notes. Record the MacBook screen level L, and the monitor brightness M from its on-screen menu.

Steps:

1. In Settings > Automatic brightness, check that the monitor is listed as Calibrated and that follow is on. Record the curve in Notes.
2. Set the MacBook screen to L and wait 5 seconds. Record M, the monitor brightness from its on-screen menu.
3. Step away from the camera so that the MacBook screen dims to 0%. Note the time, and check that "Automatically adjust brightness" turns off.
4. For 60 seconds while the MacBook screen is dimmed, check the monitor's on-screen menu. Check that its brightness stays at M.
5. Sit in front of the camera. Wait until the MacBook screen returns to L.
6. Within 2 seconds of that, check that the monitor is at the value the curve gives for L.
7. Set the option back to its starting state, and restore L and the automatic brightness state recorded before the section.

Expected (per SC-004 and FR-009):

- While the MacBook screen is dimmed, the external monitor does not change because of the dimmed level.
- After the screen returns to L, follow resumes with a fresh reading, and the monitor is at the curve value for L within 2 seconds.

Result:

Notes:

---

## G. Calibration and protection share one owner of automatic brightness (SC-005, FR-008)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: One external monitor with at least one saved point. The option is on, "Automatically adjust brightness" is on at the start (record it as A), presence protection is on with the "MacBook screen" toggle on. Record the MacBook screen level L and the monitor brightness from its on-screen menu. Use Cancel, not Done, in the calibration dialog, so that no saved point changes.

Steps, protection first, then calibration refused:

1. Step away from the camera so that the MacBook screen dims to 0% and "Automatically adjust brightness" turns off.
2. While the screen is dimmed, open Settings > Automatic brightness and select the monitor. Press Add point, or Calibrate on a saved row.
3. Check that calibration is refused. The notice "Calibration is unavailable while the built-in screen is protected. Try again in a moment." appears in the Automatic brightness tab, the calibration dialog does not open, and the monitor does not move.
4. Check that "Automatically adjust brightness" is still off, because protection holds it.
5. Sit in front of the camera. Wait until the MacBook screen returns to L and "Automatically adjust brightness" returns to A.

Steps, calibration first, then protection:

6. With the MacBook screen at L and automatic brightness at A, open Settings > Automatic brightness, select the monitor and press Calibrate on a saved row. Check that the calibration dialog opens and that "Automatically adjust brightness" turns off.
7. Press Cancel. Check that the monitor, the MacBook screen and "Automatically adjust brightness" return to their starting values.
8. Step away from the camera. Check that the screen dims to 0% and "Automatically adjust brightness" turns off.
9. Sit in front of the camera. Check that the screen returns to L and "Automatically adjust brightness" returns to A.

Steps, run section E again:

10. Run section E in full, including its two halves.
11. At the end, check that "Automatically adjust brightness" is in state A, the state recorded at the start of this section.

Expected (per SC-005 and FR-008):

- Calibrate and Add point are refused with the protection notice while the MacBook screen is dimmed by protection, and no dialog opens.
- After a calibration ends with Cancel, automatic brightness returns to its state before the calibration, and the screen returns to L.
- After section E runs again, automatic brightness ends in state A.

Result:

Notes:

---

## H. Lid close and open, and display sleep and wake, while dimmed and after (SC-002, FR-011)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: The option is on, "Automatically adjust brightness" is on at the start (record it as A), and presence protection is on with the "MacBook screen" toggle on. Record the MacBook screen level L. The lid steps need an external monitor, with the Mac in clamshell use.

Steps, lid while dimmed:

1. Set the screen to L. Step away so that the screen dims to 0% and "Automatically adjust brightness" turns off. Check both in System Settings > Displays.
2. With the external monitor connected, close the lid. Wait 30 seconds. Open the popup and check that the "MacBook screen" card is absent. In Settings > General, "OLED protection", check that the "MacBook screen" toggle is absent.
3. Open the lid. Wait up to 30 seconds. Check that the card and the toggle come back. If you are still away, check that the screen is still dimmed and that "Automatically adjust brightness" is still off.
4. Sit in front of the camera. Check that the screen returns to L, as read in System Settings > Displays, and that "Automatically adjust brightness" returns to A, with no leftover dim.

Steps, display sleep while dimmed:

5. Step away again so that the screen dims. Put the displays to sleep with your usual method, or run `pmset displaysleepnow` in a terminal. Wait 30 seconds.
6. Wake the displays by moving the mouse. Check that no visible brightness change happens on wake. If you are still away, check that the screen is still dimmed.
7. Sit in front of the camera. Check that the screen returns to L and "Automatically adjust brightness" returns to A.

Steps, after the restore:

8. With the screen restored to L, put the displays to sleep, wake them, and check that the screen is at L with no dim left behind.
9. Close the lid and open it again, with the screen at L. Check that the level stays at L.
10. Restore the starting values recorded before the section.

Expected (per SC-002 and FR-011):

- With the lid closed, the "MacBook screen" card and its toggle are absent from the popup and from Settings > General.
- While the lid is closed or the displays are asleep, the screen keeps its protection level, and the app does not write to an asleep display.
- After the screen returns, it is at L and automatic brightness is at A, with no visible dim left behind after the lid or the display sleep and wake.

Result:

Notes:

---

## I. Quit while dimmed (SC-002, FR-011)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: The option is on, "Automatically adjust brightness" is on at the start (record it as A), and presence protection is on with the "MacBook screen" toggle on. Record the MacBook screen level L.

Steps:

1. Set the screen to L. Step away so that the screen dims to 0% and "Automatically adjust brightness" turns off. Check both in System Settings > Displays.
2. In a terminal, run `ls "$HOME/Library/Application Support/TelaVivaOLED/"`. Check that `builtin-protection-session.json` and `auto-brightness-hold.json` are both listed.
3. Quit TelaViva OLED from its menu-bar menu, with Quit.
4. Check that within a few seconds the MacBook screen returns to L, and that "Automatically adjust brightness" is on again (A).
5. Run the `ls` command again. Check that both files are gone.
6. Relaunch the app from the repository root with `open 'build/macos/TelaViva OLED.app'`. Check that the screen is still at L and that "Automatically adjust brightness" is still A.
7. If the screen did not return to L on quit, record the level it is at in Notes. Section J covers recovery at the next launch.

Expected (per SC-002 and FR-011):

- Quit restores the MacBook screen to L and automatic brightness to A, with no action beyond Quit.
- Both recovery files are gone after the quit restore.
- Relaunching changes nothing.

Result:

Notes:

---

## J. Force quit while dimmed with the option on: recovery at the next launch (SC-003, FR-010)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: The option is on, presence protection is on with the "MacBook screen" toggle on, and "Automatically adjust brightness" is on at the start (record it as A). Record the MacBook screen level L. Keep a terminal open for the file checks.

Steps:

1. Set the screen to L. Step away so that the screen dims to 0%, and check that "Automatically adjust brightness" turns off.
2. Run `ls "$HOME/Library/Application Support/TelaVivaOLED/"`. Check that `builtin-protection-session.json` and `auto-brightness-hold.json` both exist.
3. In Activity Monitor, select TelaViva OLED and choose Force Quit. As an alternative, run `pgrep -x TelaViva` in a terminal and then `kill -9` with the process ID it prints.
4. Look at the screen. It may stay dimmed until the next launch. Record what you see in Notes.
5. Relaunch the app with `open 'build/macos/TelaViva OLED.app'` from the repository root. Wait up to 10 seconds, with no other action.
6. In System Settings > Displays, check that the MacBook screen returns to L, not to 0%, and that "Automatically adjust brightness" returns to A, on.
7. Run the `ls` command again. Check that both files are gone.
8. Run `log show --last 1h --predicate 'subsystem == "com.telaviva.oled"'`. Check for a `builtin_recovery baseline=` line. If automatic brightness was set back on at launch, expect an `auto_brightness_recovered` line too.

Expected (per SC-003 and FR-010):

- Both recovery files exist while the screen is dimmed, and both are gone after the next launch.
- After the relaunch, with no user action, the MacBook screen returns to L, the baseline stored before the dim, and not to 0%.
- "Automatically adjust brightness" returns to A, on, with no user action.

Result:

Notes:

---

## K. Setter failure and notices (FR-012, FR-014)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: Run this section only if a private setter misbehaves on this Mac. That is the case when the setter probe in section A2 of the archived checklist returns NO or an error, or when "Automatically adjust brightness" does not turn off, or cannot be read, while the screen is dimmed. Do not force a failure. If no failure occurs, record "Not run (no setter failure on this Mac)" in Notes. The option is on, presence protection is on with the "MacBook screen" toggle on, and "Automatically adjust brightness" is on at the start (record it as A). Record the MacBook screen level L.

Steps:

1. Set the screen to L and "Automatically adjust brightness" to A, on.
2. Step away so that the screen dims. Wait the usual confirmation time plus 30 seconds.
3. If "Automatically adjust brightness" did not turn off, check for the warning notice under the "MacBook screen" card and under the option in Settings > General. The text is "The system's automatic brightness could not be turned off. The screen is still dimmed, but macOS may brighten it again."
4. If "Automatically adjust brightness" could not be read, check for the notice "The system's automatic brightness could not be read, so it was left as is."
5. Check that the MacBook screen still dims to 0%, whichever notice is shown.
6. Sit in front of the camera. If the screen cannot be restored, check for the notice "The screen brightness could not be restored yet. The app will keep trying." Wait a few minutes, and record in Notes whether the restore completes.
7. Run `log show --last 1h --predicate 'subsystem == "com.telaviva.oled"'`. Check for the lines that occurred: `auto_brightness_not_changed`, `auto_brightness_unknown`, `builtin_dim unverified attempt=`, or `builtin_restore_failed`.
8. Check that "Automatically adjust brightness" is in state A at the end. If it is not, set it back by hand in System Settings > Displays, and record that in Notes.

Expected (per FR-012 and FR-014):

- The matching notice is shown under the "MacBook screen" card and under the option in Settings > General, in the app language.
- The MacBook screen still dims to 0% when the setter fails.
- The matching log line is present.
- When the app cannot read or change automatic brightness, it leaves that setting as it was, and it does not change it during restore.

Result:

Notes:

---

## L. Laptop alone, with no external monitor (FR-002, FR-003)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: Disconnect every external monitor. The Mac is on AC power, and camera access is granted. Record the starting values of "Dim when no face is detected", "Check presence only above brightness", and "Suspend presence checks on battery", and the MacBook screen level L.

Steps:

1. Open the popup. Check that the only card listed is "MacBook screen". In Settings > General, "OLED protection", check that the only entry in "Monitors in protection" is the "MacBook screen" toggle, and that "No monitors connected" is not shown.
2. In Settings > General, "OLED protection", turn on "Dim when no face is detected". Set "Check presence only above brightness" to 40%.
3. Set the MacBook screen to 80%. In Settings > General, in the "Availability" section, check that the camera status line does not read "suspended at low brightness".
4. Set the MacBook screen to 40%, which is the threshold. Check that the camera status line reads "suspended at low brightness". Set it to 45%, and check that the status no longer reads suspended.
5. With the screen at 80%, step away from the camera. Check that the MacBook screen dims to 0%. Sit in front of the camera, and check that it returns to 80%.
6. Unplug the AC power. With "Suspend presence checks on battery" on, check that the camera status line reads "suspended on battery". Turn that setting off, and check that the status no longer reads "suspended on battery". Plug the AC power back in.
7. Restore the starting values of the settings and the MacBook screen level recorded before the section.

Expected (per FR-002 and FR-003):

- With no external monitor, the popup and Settings > General list the MacBook screen, with its protection switch and brightness slider.
- With presence on and AC power, the camera runs while the MacBook screen is above the threshold.
- At or below the threshold, the camera status reads "suspended at low brightness", and the camera does not check presence.
- On battery with the suspension on, the status reads "suspended on battery". With the suspension off, the camera keeps running.
- Stepping away dims the MacBook screen to 0%, as in section B.

Result:

Notes:

---

## M. Sleep while dimmed: the same monitor returns and gets its brightness back

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: An external DDC monitor (the Dell) is connected, presence protection is on, and the monitor's protection switch is on. Do not turn on follow. Record the monitor's brightness B from its on-screen menu, and the card's Protection brightness value (1% by default).

Steps, return at the dimmed level:

1. Set the Dell to B, and record B from its on-screen menu.
2. Step away from the camera. Wait until the Dell dims to the Protection brightness value, as read in its on-screen menu.
3. Put the displays to sleep with your usual method, or run `pmset displaysleepnow` in a terminal. Wait 30 seconds.
4. Wake the displays by moving the mouse, and sit in front of the camera.
5. Check the Dell in its on-screen menu. It should be at B, not at the Protection brightness value.
6. Run `/usr/bin/log show --last 5m --predicate 'subsystem == "com.telaviva.oled"'`. In zsh, `log` alone is a builtin, so use the full path. Check the lines for the Dell, in this order: `monitor_disconnected`, `monitor_discovered`, `dim_resumed` with `baseline=` matching B (the log uses device units), `ownership_verified`, `restore_enqueued`.

Steps, variant: change the brightness yourself before waking:

7. Dim the Dell as in steps 2 and 3, and put the displays to sleep.
8. Before you wake the displays, set the Dell to a different level N with its own controls, and record N. If the Dell does not respond while the display sleeps, record that in Notes and skip this variant.
9. Wake the displays and sit in front of the camera. Check the Dell in its on-screen menu.
10. Run the same `/usr/bin/log show` command and check the Dell lines.

Expected:

- After the wake and return, the Dell is at B, not at the Protection brightness value. The log shows the sequence in step 6.
- Variant: the Dell stays at N. The log has no `dim_resumed` line for the Dell, and protection does not restore B.

Result:

Notes:

---

## N. No read/write loop after a slider set

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: One DDC monitor is connected and not dimmed. Record its brightness from its on-screen menu. Keep the app idle except for the steps below.

Steps:

1. Open the popup and drag the monitor's brightness slider to a position between two whole percents, if the slider allows it, and release it. Record the position in Notes.
2. Close the popup and leave the app idle for one minute, with no other input.
3. Run `/usr/bin/log show --last 5m --predicate 'subsystem == "com.telaviva.oled"'`. In zsh, `log` alone is a builtin, so use the full path.
4. In Activity Monitor, watch the CPU of TelaViva OLED for that minute, and record the reading in Notes.

Expected:

- During the minute, no `ownership_verified` or `restore_enqueued` line repeats. Only the lines from the change itself appear.
- CPU stays low and does not climb.

Result:

Notes:

---

## Windows

Status: Pass (marked by the owner on 2026-10-27; no observed values recorded). The Windows implementation exists in code with passing unit tests (2026-10 work in specs/windows-laptop-parity/plan.md); the owner marked every Windows row below as passed. For the owner, on a Windows laptop, after the work in ../windows-handoff.md.

Names: the Windows UI says "Laptop" in en-US and "Notebook" in pt-BR. Use the name the app shows. The rows below cover the built-in laptop panel only, except W-L10, which covers an external DDC monitor. Their steps are written for the owner to run after the Windows work is done, and they are not verified.

### W-L4. Protection dims and restores the laptop panel (FR-016, SC-001, SC-002)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: A Windows laptop whose panel the app can read and set through WMI. Presence or screensaver protection is on, and the Laptop (Notebook in pt-BR) protection switch is on. Record the panel brightness L from Windows Settings > System > Display.

Steps:

1. In the app's Settings, turn on screensaver protection, or presence protection. In the popup or the protection list, turn on the Laptop (Notebook in pt-BR) protection switch.
2. Set the panel brightness to a known level L, and record it in Notes.
3. Start the screensaver, or step away from the camera, and wait for the dim.
4. Check that the panel dims to its protection level, 0% by default or the configured protection brightness. Record the delay in Notes.
5. Stop the screensaver, or return to the camera. Check that the panel returns to L.
6. Set Protection brightness on the Laptop card to 1, then to 50. At each value, repeat steps 3 to 5. Check that the slider stays within 1% to 50%, and record in Notes the level Windows Settings shows while dimmed. The panel accepts only its supported levels, so the reading can differ slightly from the value set.
7. With an external display connected, close the lid. Wait 30 seconds, and check that the Laptop card and its protection entry are absent, as in W-L7. Open the lid again.
8. Restore the starting values recorded before the section.

Expected (per FR-016, SC-001 and SC-002):

- The laptop panel dims to its protection level after the trigger.
- The panel returns to L when the trigger ends.
- The protection level stays between 1% and 50%. A write of 0% lands on the panel's lowest supported level, which can be above 0%.
- With the lid closed, the Laptop card and its protection entry are absent, and they return when the lid opens.

Result:

Notes:

### W-L5. Adaptive brightness off during protection when the option is on, restored after, per AC and DC (FR-006, FR-007, FR-016)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: The Laptop (Notebook in pt-BR) option that turns off Windows adaptive brightness during protection is available and off by default. Record the active power plan name, and the values of "Enable adaptive brightness" for "Plugged in" and "On battery", from Power Options > Change plan settings > Change advanced power settings > Display.

Steps, plugged in (AC):

1. Plug the laptop into AC power. Record the "Enable adaptive brightness" values for Plugged in and On battery.
2. In the app, turn the Laptop (Notebook in pt-BR) option on. Trigger protection by stepping away or starting the screensaver.
3. Open Power Options, the advanced settings of the active plan, while the panel is dimmed. Check that "Enable adaptive brightness" for Plugged in shows Off.
4. End the protection. Check that the Plugged in value returns to its recorded value, and that the panel returns to its level.

Steps, on battery (DC):

5. Unplug the laptop. Repeat steps 2 to 4, and check the On battery value.

Steps, option off:

6. Turn the option off. Trigger protection on AC power. Check that "Enable adaptive brightness" is not changed during protection.
7. Check that the active power plan name is the same before, during and after protection.
8. Restore the recorded values and the active plan, and record the result in Notes.

Expected (per FR-006, FR-007 and FR-016):

- With the option on, adaptive brightness shows Off for Plugged in during the dim, and for On battery during a dim on battery.
- When protection ends, each value returns to its recorded value, if the app turned it off and it is still off.
- If the active power plan changed during protection, the app does not restore the value over the new plan.
- With the option off, protection never changes the adaptive brightness value.
- The active power plan and its other values are unchanged before, during and after protection.

Result:

Notes:

### W-L6. Force-quit recovery (US-006, FR-010, SC-003)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: The Laptop (Notebook in pt-BR) option is on, and presence or screensaver protection is on. Record the panel level L, and the "Enable adaptive brightness" values for Plugged in and On battery.

Steps:

1. Set the panel to L, and trigger protection so that the panel dims. Check that adaptive brightness is off for the power source in use.
2. Check that the recovery record written by the Windows build exists in the app data folder, next to the app settings.
3. Open Task Manager, go to the Details tab, select the TelaViva OLED process, and choose End task.
4. Look at the panel. It may stay dim until the next launch. Record what you see in Notes.
5. Launch the app again. Wait up to 10 seconds, with no other action.
6. Check that the panel returns to L, not to the dimmed level, and that "Enable adaptive brightness" returns to its recorded value for Plugged in and On battery.
7. Check that the recovery record is gone.
8. Calibration recovery with an external DDC monitor: in Settings > Automatic brightness, start a calibration on that monitor, and record the monitor's brightness from its own menu first. While the calibration dialog is open, end the app with End task in Task Manager.
9. Disconnect the external monitor before the relaunch. Relaunch the app, and check that adaptive brightness is restored at once.
10. Reconnect the external monitor within 90 seconds of the relaunch. Check that it returns to the brightness recorded in step 8.
11. Record the result, including any notice shown, in Notes.

Expected (per SC-003 and FR-010):

- The recovery record exists while the panel is dimmed, and it is gone after the next launch.
- After the relaunch, with no user action, the panel returns to L and adaptive brightness returns to its recorded values, on AC and on battery.
- Calibration recovery: adaptive brightness is restored at the launch, and the external monitor returns to its recorded brightness within 90 seconds of the launch, once it is connected again.

Result:

Notes:

### W-L7. Laptop screen absent from the lists with the lid closed (FR-002, SC-009)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: A Windows laptop with an external display connected. In Power Options, under "When I close the lid", set "Plugged in" to "Do nothing", so the laptop keeps running with the lid closed. Record the current value first.

Steps:

1. Connect the external display. Open the app's popup, and check that the Laptop (Notebook in pt-BR) card is listed.
2. Close the lid. Wait 30 seconds, which covers one availability refresh.
3. Open the popup, and check that the Laptop (Notebook in pt-BR) card is absent.
4. Open the app's Settings, then the protection list on the General page. Check that the Laptop (Notebook in pt-BR) entry is absent.
5. Open the lid. Wait up to 30 seconds. Check that the card and the entry come back.
6. Restore the lid setting in Power Options to the value recorded before the section.

Expected (per FR-002 and SC-009):

- With the lid closed, the Laptop (Notebook in pt-BR) screen is absent from the popup and from the protection list.
- After the lid opens, the screen reappears within one availability refresh.

Result:

Notes:

### W-L8. Monitored display picker decides the camera (FR-021, architecture Decision 14)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: A Windows laptop whose built-in panel the app can read and set through WMI, and at least one external monitor that the app controls through DDC. Presence protection is on. Record which display is the primary display in Windows Settings > System > Display, and the current Monitored display value in the app.

Steps:

1. Make the external monitor the primary display. Set Monitored display to Automatic.
2. Set the Laptop (Notebook in pt-BR) screen above the presence threshold, and the external monitor at or below it. Check the camera status in the popup.
3. Select Laptop (Notebook in pt-BR) as Monitored display. Check the camera status again.
4. Select Automatic. Make the laptop panel the primary display. Check that Automatic now follows the external monitor.
5. Select the external monitor as Monitored display, then disconnect it. Check that the camera follows the Laptop (Notebook in pt-BR) screen.
6. Select the external monitor as Monitored display and set its presence threshold to 50%. Then select Laptop (Notebook in pt-BR) as Monitored display and set its threshold to 0%. Check that the threshold box shows each display's own value after each selection. With the Laptop screen at 1%, check that the camera keeps running, because its threshold is 0%.
7. Restore the primary display, the Monitored display value and the thresholds recorded in the preconditions.

Expected (per FR-021):

- Only the monitored display decides whether the camera runs at the threshold. Absence still dims every protected screen, including the Laptop (Notebook in pt-BR) screen.
- Automatic uses the primary external monitor, then the first external monitor, then the Laptop (Notebook in pt-BR) screen.
- If the chosen display is disconnected, the next remembered choice is used, then Automatic.
- The Laptop (Notebook in pt-BR) entry is listed only while the screen is available and adjustable.
- Each display keeps its own presence threshold, from 0% to 90%. At 0%, the camera stops only when the monitored display is at 0%.

Result:

Notes:

### W-L9. Lid closed: built-in camera not used, camera choice kept (FR-020)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: A Windows laptop with a built-in camera. An external USB camera is optional. Presence protection is on. In Power Options, under "When I close the lid", set "Plugged in" to "Do nothing", as in W-L7. Record the current value, and the camera selected in the Settings camera picker.

Steps:

1. Select the built-in camera in the camera picker. Check that presence protection shows the camera running.
2. Close the lid. Wait 30 seconds. Check that the built-in camera is not used. If an external camera is connected, check that it is used.
3. Open the camera picker and record what it shows.
4. Open the lid. Wait up to 30 seconds. Check that the built-in camera runs again, without restarting the app.
5. Close the lid, quit the app, and start it again with the lid still closed. Record whether the built-in camera is used at launch.
6. Restore the lid setting in Power Options to the value recorded before the section.

Caveat: the lid rule identifies the built-in camera by its reported enclosure location (front or back). A built-in camera that reports no enclosure location is not treated as built-in, so the rule does not hide it. If that happens, record the camera's name and whether it was used with the lid closed in Notes.

Expected (per FR-020):

- With the lid closed, the built-in camera is not used, as if it were disconnected. Without another camera, presence protection reports the camera as unavailable.
- Closing and opening the lid does not change the camera selection the user made.
- The built-in camera is used again within one availability refresh after the lid opens.

Result:

Notes:

### W-L10. Sleep while dimmed with a DDC monitor (wake rediscovery)

Status: Pass (marked by the owner on 2026-10-27; no values recorded)

Preconditions: A Windows 11 x64 PC with an external monitor that the app controls through DDC. Presence or screensaver protection is on, and the monitor's protection switch is on. Include a monitor whose DDC brightness maximum is not 100, if you have one, and record its model and maximum if you can find it. Record the monitor's brightness B from its own on-screen menu, and the protection brightness value.

Steps:

1. Set the monitor to B, and record B.
2. Start the screensaver, or step away from the camera, and wait for the monitor to dim to the protection level.
3. Put the display to sleep with your usual method, or turn it off and on. Wait 30 seconds, then wake it and sit in front of the camera. Record whether the monitor was dropped and listed again. Open `%LOCALAPPDATA%\TelaVivaOLED\telaviva.log` and look for `monitor_disconnected`, `monitor_discovered` and `dim_resumed`. If they are missing, set `TELAVIVA_LOG_LEVEL` to `info` before you start the app, and repeat.
4. Check the monitor in its on-screen menu. It should be at B, not at the protection level.
5. Check that the brightness slider in the popup works after the wake. If the monitor shows as unsupported, record it in Notes, and record whether unplugging and replugging it is the only way to get the slider back. See `../windows-handoff.md`, 7.6 (d).

Expected:

- After the wake and return, the monitor is at B, not at the protection level.
- The brightness slider works after the wake.
- With a monitor whose DDC maximum is not 100, the restored level matches the level you set in step 1, as the slider shows it.

Result:

Notes:

---

---

## Summary

| Section | Scope | Status | Date |
|---------|-------|--------|------|
| A | Listing and lid: popup and Settings list, lid closed and reopened (SC-009) | Pass | 2026-10-27 |
| B | Presence dim and restore, per-screen switch off (SC-001, SC-002) | Pass | 2026-10-27 |
| C | Screensaver dim, Protection brightness and restore (SC-002) | Pass | 2026-10-27 |
| D | Option off, the default: automatic brightness unchanged (SC-006) | Pass | 2026-10-27 |
| E | Option on: automatic brightness off while dimmed, held 10 minutes, restored (SC-001, SC-002) | Pass | 2026-10-27 |
| F | Follow interplay with a calibrated external monitor (SC-004, FR-009) | Pass | 2026-10-27 |
| G | Calibration and protection share one owner of automatic brightness (SC-005, FR-008) | Pass | 2026-10-27 |
| H | Lid close and open, display sleep and wake, while dimmed and after (SC-002, FR-011) | Pass | 2026-10-27 |
| I | Quit while dimmed (SC-002, FR-011) | Pass | 2026-10-27 |
| J | Force quit while dimmed with the option on, recovery at next launch (SC-003, FR-010) | Pass | 2026-10-27 |
| K | Setter failure and notices (FR-012, FR-014) | Pass | 2026-10-27 |
| L | Laptop alone, no external monitor (FR-002, FR-003) | Pass | 2026-10-27 |
| M | Sleep while dimmed: the same monitor returns and gets its brightness back, a new level is kept | Pass | 2026-10-27 |
| N | No read/write loop after a slider set | Pass | 2026-10-27 |
| Windows | Rows W-L4 to W-L10, for the owner on Windows (FR-016) | Pass | 2026-10-27 |
