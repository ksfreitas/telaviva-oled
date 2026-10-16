# Chore: Route hardware volume keys by the Mac's sound output instead of the pointer

## Chore Description

Change the opt-in macOS hardware-key routing so the keyboard volume-up/down keys no longer adjust the external monitor under the mouse pointer. They adjust an external monitor's speaker volume only while that monitor is the Mac's current default sound output device. When the default output is anything else (built-in speakers, wired or Bluetooth headphones, a USB DAC, AirPlay) or cannot be matched to exactly one eligible monitor, the whole key sequence passes through to macOS, so macOS keeps controlling the headphones. This removes the failure where a user wearing headphones moves the cursor over a monitor and a volume key silently changes the monitor's speaker instead of the headphones.

Brightness keys are unchanged: they still target the single external display under the pointer. Mute stays excluded. The step sizes, Shift+Option fine step, modifier pass-through, 0/100 boundary consumption, key-sequence latching, bounded coalescing intents, and the shared core/DDC command path stay as they are.

Matching the audio device to the monitor must be deterministic and must not guess by name or array order. Evidence gathered on the connected LG ULTRAFINE: CoreAudio lists the monitor as an output device named `LG ULTRAFINE`, transport `dprt` (DisplayPort), with `kAudioDevicePropertyDeviceUID` `1E6D8877-0000-0000-0722-0104B5462878`. That value equals the `EDID UUID` IORegistry property that `MacDDCController` already reads as `DisplayTransport.edidUUID` and embeds in the monitor ID. The built-in speaker device is `BuiltInSpeakerDevice` (`bltn`). A monitor is therefore the sound output when the default output device UID equals that monitor's `edidUUID` (case-insensitive). The match was observed on this one LG over this connection only; any monitor whose audio device UID differs, or whose `edidUUID` is missing or shared by more than one display, passes through instead of falling back to a name match.

The event callback must stay lightweight. It must not call CoreAudio. A new observer reads the default output UID off the callback path, only when a change notification fires, and hands an immutable value to the router under its existing state lock. The observer runs only while hardware-key routing is enabled and holds no polling timer.

When the default output changes during a held volume key, apply the existing disconnect/generation rule: stop future writes for that held key, keep consuming its already-handled repeats and key-up while the tap remains active, and never reinject an event. A volume key sequence whose initial press was passed through stays passed through for the whole hold, even if the output switches to the monitor mid-hold.

No new setting, no Rust core or C ABI change, and no new dependency. CoreAudio is a system framework. Windows remains deferred.

## Relevant Files

- `native/macos/TelaViva/HardwareKeyRouter.swift`: `HardwareKeyDisplay`, `HardwareKeyPolicy.target`, `HardwareKeySequence.process`, and `HardwareKeyRouter` own target selection, latching, and the state lock. Add the audio match here.
- `native/macos/TelaViva/MacDDCController.swift`: `publishHardwareKeyDisplays` builds the descriptors from `DisplayTransport`. Expose `edidUUID` for stable-identity displays as the audio match key. `displayName(adapterPath:)` is deliberately not used for matching.
- `native/macos/TelaViva/MenuBarController.swift`: starts and stops the router from the snapshot sink and shutdown. Wire the audio observer's lifecycle and its output to the router here.
- `native/macos/TelaViva/Localization.swift`: the English and Brazilian Portuguese `hardwareKeys.toggle` and `hardwareKeys.help` strings still say both feature types follow the pointer.
- `native/macos/Tests/HardwareKeyRouterTests.swift`: existing volume cases use pointer targeting and must be updated; add the audio-output cases.
- `native/macos/Tests/CameraLocalizationTests.swift`: existing localization-key coverage; confirm it still passes after the string change.
- `tools/tests/test-hardware-key-router.sh`, `tools/tests/test-camera-lifecycle.sh`, `tools/tests/test-core-bridge.sh`, `tools/tests/test-manual-monitor-controls.sh`: explicit `swiftc` source lists; add the new production file wherever `HardwareKeyRouter.swift` is compiled together with `MenuBarController`-independent code that now references it.
- `packaging/macos/build-app.sh`: explicit `SOURCES` array and `swiftc` `-framework` list; add the new file and `-framework CoreAudio`.
- `native/macos/TelaViva.xcodeproj/project.pbxproj`: matching file reference, build file, group entry, sources phase, and CoreAudio framework entry in the frameworks phase.
- `.github/workflows/macos.yml`: already runs the hardware-key suite; confirm nothing else is needed.
- `README.md` (Usage, keyboard routing paragraph), `api.md` (the sentence about keyboard intent targeting an external monitor under the cursor), `CHANGELOG.md` (Unreleased): describe the new volume behavior.
- `specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys/macos-acceptance.md`: the "External supported volume" row and new scenarios for headphones and output switching.
- `specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys.md`: add a short follow-up note pointing to this chore; do not rewrite its completed tasks.

### New Files

- `native/macos/TelaViva/DefaultAudioOutputMonitor.swift`: CoreAudio observer for the default output device. It takes an injectable `readDefaultOutputUID: () -> String?` (default implementation reads `kAudioHardwarePropertyDefaultOutputDevice`, then `kAudioDevicePropertyDeviceUID`) so tests need no audio hardware, plus `start()`, `stop()`, and a deterministic refresh seam for tests.
- `native/macos/Tests/DefaultAudioOutputMonitorTests.swift` and `tools/tests/test-default-audio-output.sh`: only if the observer's injected-read lifecycle (start, change, stop, no callback after stop) is not covered cleanly inside `HardwareKeyRouterTests.swift`; otherwise fold into that suite and skip these two files.

## Step by Step Tasks

### 1. Add the audio match key to the display descriptor

- Add `audioDeviceUID: String?` to `HardwareKeyDisplay`. In `publishHardwareKeyDisplays`, set it from `transport.edidUUID` only when `transport.identityIsStable` is true and the display is external; otherwise `nil`.
- Update the `HardwareKeyDisplay` initializer call sites, including the `display(...)` helper in `HardwareKeyRouterTests.swift`.

### 2. Add the default-output observer

- Create `DefaultAudioOutputMonitor.swift`. Register `AudioObjectAddPropertyListenerBlock` on the system object for `kAudioHardwarePropertyDefaultOutputDevice` and `kAudioHardwarePropertyDevices`, delivered on a private serial queue. On each notification, read the default device UID with the injected reader and report it through an `onChange(String?)` closure only when it differs from the last reported value.
- Report `nil` when there is no default output, the UID cannot be read, or the read fails. `nil` always means pass-through.
- `start()` reads the current value once and installs the listeners; `stop()` removes exactly the listeners it installed, is idempotent, and guarantees no `onChange` after it returns.
- No polling timer and no work while idle. Never call it from the event tap callback.

### 3. Make volume targeting follow the sound output

- Add `defaultOutputUID: String?` to the router state, set by a new `setDefaultOutput(uid:)` that takes `stateLock`, stores the value, and clears nothing else. Clear it to `nil` on disable, permission loss, and shutdown.
- Add `HardwareKeyPolicy.volumeTarget(defaultOutputUID:displays:)`. It returns a display only when `defaultOutputUID` is non-nil, exactly one display has an equal `audioDeviceUID` (case-insensitive), that display is external, has a stable `monitorID` and `generation`, and its volume state is supported, awake, and has a finite 0...100 value. Otherwise `nil`.
- Pass `defaultOutputUID` into `HardwareKeySequence.process`. For a non-repeat down of a volume key, use `volumeTarget` instead of the pointer lookup; brightness keeps `HardwareKeyPolicy.target(at:…)`. Do not sample the pointer for volume.
- In the held-repeat branch for volume, add the condition that the latched display's `audioDeviceUID` still equals the current `defaultOutputUID`; otherwise mark the latch cancelled and keep consuming until key-up.
- Update `processForTesting` and the live `handle` call to supply `defaultOutputUID` from the state read under the same lock.

### 4. Wire the lifecycle

- In `MenuBarController`, create the observer, set `onChange` to call `hardwareKeyRouter.setDefaultOutput(uid:)`, start it when `hardware_keys_enabled` becomes true, and stop it when it becomes false or the app terminates, next to the existing `hardwareKeyRouter.apply` and shutdown paths. Starting twice or stopping twice must be harmless.

### 5. Update user-facing text and documentation

- Update both languages' `hardwareKeys.toggle` and `hardwareKeys.help` in `Localization.swift`. English intent: brightness keys adjust the external display under the pointer; volume keys adjust an external display only while it is the Mac's sound output, otherwise macOS controls the volume (for example headphones). Keep the step-size sentence and the Accessibility requirement. Mark every Portuguese string as the repository requires and keep both languages in the same shape.
- Update the `README.md` keyboard paragraph, the `api.md` sentence, and add an Unreleased `CHANGELOG.md` entry (behavior change: volume no longer follows the pointer).
- Update `macos-acceptance.md`: reword the volume row and add rows listed under Notes. Add the follow-up note to `external-monitor-hardware-keys.md`.

### 6. Register the new source in every build path

- Add `DefaultAudioOutputMonitor.swift` (and the test files if created) to `packaging/macos/build-app.sh` `SOURCES`, add `-framework CoreAudio` to its `swiftc` call, add the file and CoreAudio framework to `project.pbxproj` consistently with the existing entries, and add the file to every `tools/tests/*.sh` source list that compiles code referencing it. Run `plutil -lint` on the project file.

### 7. Update and add tests

Deterministic, with no audio hardware, event tap, or DDC writes:

- Update the existing volume cases in `HardwareKeyRouterTests.swift` to the new rule.
- Default output equals a monitor's `audioDeviceUID` with the pointer over another display or a gap: volume key is handled and targets that monitor; pointer position is irrelevant.
- Default output is the built-in speakers or an unmatched UID, with the pointer over a supported monitor: whole down/repeat/up sequence passes through.
- `defaultOutputUID == nil`: pass-through. Duplicate `audioDeviceUID` across two displays: pass-through. Case-insensitive match works.
- Matched monitor with unsupported volume, sleeping, unstable identity (`audioDeviceUID == nil`), stale generation, or built-in: pass-through.
- Output switches away mid-hold: no further writes, repeats and key-up still consumed, nothing reinjected. Output switches to the monitor mid-hold of a passed-through key: stays passed through until key-up. Next distinct press re-evaluates.
- Boundary at 0 and 100 consumes without a write; Shift+Option fine step and disallowed modifiers behave as before.
- Brightness keys still follow the pointer and ignore the audio output; brightness and volume latches are independent.
- Observer: `start` reports the current UID once, a change reports once, an unchanged value does not repeat, `stop` removes listeners and silences later notifications, double `start`/`stop` are safe, and a read failure reports `nil`.
- Disabling routing, losing permission, and shutdown clear `defaultOutputUID` and pending intents.

### 8. Run Validation Commands

Run every command below, then fix regressions. Record results in this plan's Notes; keep physical acceptance separate from automated results.

## Validation Commands

```sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-manual-monitor-controls.sh
tools/tests/test-core-bridge.sh
tools/tests/test-ddc-transaction.sh
tools/tests/test-camera-lifecycle.sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
plutil -lint native/macos/TelaViva.xcodeproj/project.pbxproj
./scripts/build-macos.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
lipo 'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' -verify_arch arm64
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
git diff --check
```

The Rust workspace is unchanged, but its checks guard against accidental drift. The native suites are the real proof of the routing rule.

## Notes

- Physical acceptance, to be added to `macos-acceptance.md` and run by a person with the keyboard and the LG: (1) default output is the built-in speakers or headphones with the cursor over the LG: volume keys pass through and the LG volume does not change; (2) output set to the LG in System Settings with the cursor over the built-in display or a gap: volume keys change only the LG, with no macOS volume change; (3) switch output while holding a key; (4) plug in and remove headphones while the LG is the output; (5) brightness keys still follow the pointer; (6) the Mac output list and default UID match the LG `edidUUID` on the tested connection. Record macOS version, keyboard, monitor, cable or dock, and permission state. A passing build does not establish this.
- The UID match was observed only on the connected LG ULTRAFINE over DisplayPort, with the monitor not currently selected as output. It was a read-only CoreAudio enumeration; nothing was written to the monitor or the Mac's output selection. Other monitors, docks, HDMI, and USB audio paths are unverified; unmatched cases pass through, so the failure mode is "macOS keeps the key", never "the wrong device changes".
- No name-based fallback on purpose: identical monitor models share a name, which would reintroduce the wrong-target problem.
- Dropping the pointer for volume means a user with a monitor as sound output and the cursor on another screen now controls the monitor's speakers, which is the requested behavior.
- If the LG is the sound output but its DDC volume is unsupported or fixed, the key passes through to macOS, as for any unsupported control.
- No Rust, settings schema, or C ABI change, and no new setting. If an opt-out is wanted later, it would be a separate default-false-compatible settings field following the existing `hardware_keys_enabled` pattern.
- Planning only: this document is the output of `codeadvisor-dev-fast-chore`. No source change, build, hardware write, commit, or push was made.

### Implementation results (2026-10-16)

- Tasks 1–7 are implemented: `audioDeviceUID` on `HardwareKeyDisplay` (from `edidUUID`, stable external displays only); `DefaultAudioOutputMonitor` (CoreAudio listeners on the default output and device list, injectable reader, no polling, idempotent start/stop, silent after stop); `HardwareKeyPolicy.volumeTarget` plus a pointer-independent volume path in `HardwareKeySequence`, with held-key cancellation when the output changes; lifecycle wiring in `MenuBarController`; localized toggle/help text; build, Xcode, and CI registration (including CoreAudio linkage and a new CI step); README, api.md, CHANGELOG, and acceptance-checklist updates.
- Plan adjustment: the router first cleared its stored output UID on permission loss and tap failure. Because the observer keeps running in those cases and only reports on change, regaining permission would have left volume keys in pass-through until the next audio change. The UID is now cleared only on disable and shutdown, where the observer is stopped and re-reports on restart.
- Automated validation passed: hardware-key router, default-audio-output, manual-monitor-controls, core-bridge, ddc-transaction, and camera-lifecycle native suites; `cargo fmt --check`; 38 Rust tests; strict clippy; `plutil -lint`; `./scripts/build-macos.sh` (CoreAudio linked); `--smoke-test`; `--diagnostics` (LG `transport_ready`); arm64 `lipo`; strict `codesign`; `git diff --check`.
- Physical acceptance is not run. The new rows in `external-monitor-hardware-keys/macos-acceptance.md` must be exercised by a person: headphones or built-in speakers with the cursor over the LG, the LG as sound output with the cursor elsewhere, switching output during a held key, and brightness still following the cursor. The EDID-UUID match was observed read-only on one LG only.

### Follow-up: keys not reaching the app (2026-10-16)

The first physical test showed neither volume nor brightness keys being routed, and the permission buttons stayed active although Accessibility was granted. macOS `tccd` logs showed the cause:

- The WindowServer checks `kTCCServiceListenEvent` (Input Monitoring) for `com.telaviva.oled` before delivering keyboard events to the event tap, and it was not granted. The code only checked and requested Accessibility.
- The one Accessibility check in the log was attributed to another app (`com.smartlayouts.smartlayouts`, the responsible process) and was allowed through that app's grant, not the app's own.
- `tapCreationFailed` was sticky and the router status only refreshed on core snapshots, so granting a permission in System Settings never took effect.
- The ad-hoc signature's designated requirement was `cdhash H"..."`, so every rebuild invalidated earlier grants while System Settings still showed the toggle.

Checked with real data before changing anything: with the LG as the default output, its `audioDeviceUID` equals the CoreAudio device UID, `HardwareKeyPolicy.volumeTarget` resolves to the LG, and brightness targeting at the display center resolves to the LG. The routing policy is correct; the failure was the tap and permissions.

Changes: Input Monitoring is now checked, requested, and shown next to Accessibility with each request button disabled once granted; permissions are re-read when the app activates, when Settings opens, and after the system's accessibility notification (no polling); a failed tap is retried after a permission change and the tap is recreated when Input Monitoring is granted later; local builds sign with `designated => identifier "com.telaviva.oled"` so grants survive rebuilds (existing grants must be removed and added once). Automated validation passed again (all native suites, 38 Rust tests, clippy, build, smoke, arm64, strict codesign). End-to-end key delivery is not verified: it needs both permissions granted to the rebuilt app and a person at the keyboard.

### Follow-up: mute and brightness (2026-10-16)

After granting both permissions to the rebuilt app, the user reported that the volume keys work with the headphones and with the monitor as the sound output. Brightness keys did not adjust the monitor, and the mute key was not handled for the monitor (it was out of the original scope). The mute key was added as a separate chore, see [mute key restores volume](hardware-mute-key-restores-volume.md). For brightness, a `--probe-media-keys` diagnostic records which events the keyboard really sends. Other acceptance rows remain unrecorded.

### Follow-up: stale build during the first mute and brightness test (2026-10-16)

- The app running during the user's test was the build from 14:21. That build is older than the mute support (router source edited at 15:09) and the `--probe-media-keys` diagnostic (15:08), so the observation that mute does not work was made against a binary without mute support.
- The app was rebuilt at 15:30 and again at 15:42 with the same designated requirement, `identifier "com.telaviva.oled"`, so existing permissions keep applying.
- Brightness is still reported as not working. The probe output at `/tmp/telaviva-keys.txt` has not been collected yet, so the cause is a hypothesis, not a finding: the router decodes only `NX_SYSDEFINED` auxiliary-control events, while some keyboards send F1 and F2 key events.
- Lesson for acceptance: compare the binary timestamp with the last source edit before a physical test.

### Follow-up: brightness keys are plain key events (2026-10-16)

- Probe evidence: a key probe (`--probe-media-keys`) on the user's real keyboard, with Accessibility and Input Monitoring granted and the cursor over the LG ULTRAFINE, showed that brightness-up arrives as ordinary `keyDown` and `keyUp` events with key code 144, and brightness-down with key code 145, both with the `fn` flag set. Neither arrived as an `NX_SYSDEFINED` media event.
- Volume arrived as `NX_SYSDEFINED` (`subtype=8 key=0` for volume up). That explains the first physical result: volume and mute worked, while brightness keys did nothing, because the router decoded brightness only from `NX_SYSDEFINED` events.
- Fix: the event tap also listens to `keyDown` and `keyUp`, and decodes only key codes 144 (brightness up) and 145 (brightness down) as brightness keys. The rules are unchanged: the single external display under the pointer is the target; normal step 6.25 points; Shift+Option fine step 1.5625 points; Command, Control and other modifiers pass through; the `fn` flag does not disqualify the key; key sequences stay latched; the key passes through over the built-in display or an unsupported control. Volume and mute routing are unchanged.
- Privacy: the tap now receives every keystroke. The handler reads only the key code, the autorepeat flag and the modifier flags, returns immediately for every other key code, never reads characters, and never stores or logs anything. This widens the event mask beyond the media-event-only mask recorded in the original notes.
- Status: the user confirmed on 2026-10-16 that volume and mute work with the monitor as the sound output. The brightness fix is awaiting physical confirmation; see the [macOS acceptance checklist](external-monitor-hardware-keys/macos-acceptance.md).

