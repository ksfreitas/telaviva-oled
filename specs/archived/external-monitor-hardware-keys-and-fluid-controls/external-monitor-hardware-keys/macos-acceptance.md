# Hardware-Key Routing Physical Acceptance

Status: not run. Automated tests do not establish physical key interception, TCC behavior, or DDC writes. Record macOS version, keyboard, monitor model, cable/dock, and results for each configuration.

| Scenario | Expected behavior | Result |
|---|---|---|
| Fresh settings | Routing off; no Accessibility prompt at launch. | Not run |
| Enable routing | Explicit permission flow; status reflects permission/tap availability. | Not run |
| External supported brightness | Brightness keys change only that monitor under the cursor. | Not run |
| External supported volume, monitor is the Mac sound output | With the cursor anywhere (including another display or a gap), volume keys change only that monitor's speaker volume without changing Mac volume. | Not run |
| Headphones or built-in speakers as the output, cursor over the monitor | Volume keys pass through to macOS and the monitor volume is unchanged. | Not run |
| Output switched while a volume key is held | No further monitor writes; the held key stays consumed until release. | Not run |
| Headphones plugged in or removed while the monitor is the output | Volume routing follows the output within one key press. | Not run |
| Brightness keys with the sound output elsewhere | Brightness keys still follow the cursor regardless of the sound output. | Not run |
| Brightness keys (plain key codes 144/145) over the LG and over the built-in display | Brightness-up/down with the cursor over the LG change only the LG; macOS must not also change another display. With the cursor over the built-in display the keys pass through. Shift+Option uses the fine step, and holding the key repeats. | Not run |
| Default output device UID on the tested connection | Equals the monitor's EDID UUID. | Not run |
| Unsupported feature, internal display, or cursor gap | Entire key sequence passes through to macOS. | Not run |
| Hold key and move cursor | Repeats keep the original monitor; next press selects the new one. | Not run |
| Rapid repeats during slow DDC | Steps accumulate and pending work stays bounded. | Not run |
| Bounds and Shift+Option | Clamp at 0–100 without redundant writes; fine step works on actual key payload. | Not run |
| Modifier shortcuts and ordinary function keys | Original OS behavior remains available. | Not run |
| Multiple displays, negative coordinates, mixed scaling, mirroring | Exactly one eligible target or pass-through on ambiguity. | Not run |
| Reconnect, sleep, or permission loss during a hold | No stale write; pending intents clear safely. | Not run |
| Slider after queued key command | Latest accepted slider adjustment wins over older keyboard work. | Not run |
| Presence dimming and screensaver protection | Shared manual-override and ownership rules preserve user adjustments. | Not run |
| Disable or quit with pending work | Listener and keyboard intents stop; late callbacks cannot restart them. | Not run |
| Both permissions granted | Settings shows Accessibility and Input Monitoring as Granted with their request buttons disabled. | Not run |
| Grant a permission in System Settings while the app runs | Returning to the app updates the permission state without restarting. | Not run |
| Rebuild the app | Permissions still apply without re-granting after the one-time re-add. | Not run |
| Input Monitoring missing | Status explains that Input Monitoring is required and keys pass through to macOS. | Not run |
| Mute key, monitor is the sound output | Volume goes to 0 and the next press restores the previous level; Mac volume untouched. | Not run |
| Mute then volume up | Mute restores the volume, then volume up steps from the restored level. | Not run |
| Mute then change the volume on the monitor's own controls, then press mute | Mute sets the volume to zero from the new level; the next press restores it. | Not run |
| Mute with headphones as output | Mute passes through to macOS and the monitor volume is unchanged. | Not run |
| Hold the mute key | Toggles once; repeats do not toggle again. | Not run |

`Pass` evidence is not recorded for any row.

macOS Accessibility authorization and actual media-event delivery are platform requirements; no system event is synthesized or reinjected to validate this feature. Windows remains deferred.
