# Chore: Pause MacBook brightness follow while the built-in display is off

## Chore Description

The "Automatic brightness" feature (Settings > Automatic brightness, "Follow the MacBook screen brightness") links calibrated external monitors to the MacBook's built-in display. If the MacBook's lid is closed, or its built-in screen is off, the feature must be disabled temporarily and resume by itself when the lid is opened or the built-in screen turns on. While it is paused, manual brightness control of the external monitors returns: the popup shows the brightness slider again and the brightness keys control the external monitor again (when keyboard routing is enabled). The saved setting and the calibration curves stay untouched; only the runtime behavior pauses.

Current behavior, found while scoping:

- Lid closed with an external display: the built-in display leaves the online list or goes inactive, so `DisplayServicesBackend.status()` already reports `available: false`. The core then sets `ambient.active = false`, follow stops, sliders return and the key pass-through set empties. This part works, but the Settings message can say "This Mac has no built-in display" (reason `noBuiltInDisplay`) because a closed lid removes the display from the list, which is misleading on a MacBook.
- Built-in screen off with the lid open (display sleep): `status()` stays `available: true` because the display is still online and active, and `BuiltInBrightnessMonitor` only skips reads while `isAsleep()`. Follow therefore stays active, the popup keeps hiding the brightness slider and the brightness keys keep passing through to macOS for a screen the user cannot see. This is the gap to close.
- Availability is refreshed only on `didChangeScreenParametersNotification` and `NSWorkspace.didWakeNotification`. Display sleep (`screensDidSleepNotification`) only ends calibration, and display wake (`screensDidWakeNotification`) is not observed, so a screen that turns off or on without a display reconfiguration would not be noticed.

Approach: treat a closed lid or a sleeping built-in display as "built-in unavailable" with reason `displayOffline`, using the existing availability path end to end. No core (Rust) change is needed for the pause itself: `builtin_display { available: false }` already stops follow, clears `follow_raw`, returns `ambient.active = false` and keeps monitors at their last value; when availability returns, the core recomputes the target from the current built-in level, so the mapped value is written again (a manual change made during the pause is overwritten, which is the intended "resume"). A Rust test locks that sequence in. No new polling is added.

## Relevant Files

- `native/macos/TelaViva/BuiltInDisplayBackend.swift`: `DisplayServicesBackend.status()` decides availability and the reason. Add the lid state (IOKit `AppleClamshellState`) and the asleep check, through a small pure classification function that tests can call.
- `native/macos/TelaViva/BuiltInBrightnessMonitor.swift`: consumes `status()`, disarms its timer and reports `available: false` on change; also resumes with a fresh read when availability returns. Expected to need no change; its tests get a new case.
- `native/macos/TelaViva/MenuBarController.swift`: registers the availability refresh observers (about lines 213-221) and removes them on termination (about lines 248-254). Add refresh on display sleep and a new display-wake observer.
- `native/macos/TelaViva/Localization.swift`: `ambient.unavailable.displayOffline` in en-US and pt-BR must also describe a switched-off screen, not only a mirrored or lid-closed one.
- `native/macos/TelaViva/AmbientBrightnessView.swift`: shows the unavailable reason; read only, no change expected.
- `native/macos/TelaViva/MonitorPopup.swift` and `native/macos/TelaViva/HardwareKeyRouter.swift`: already return the brightness slider and clear the pass-through set when `ambient.active` is false; read only, verify no change is needed.
- `native/macos/Tests/BuiltInBrightnessMonitorTests.swift` and `tools/tests/test-builtin-brightness-monitor.sh`: Swift test script that compiles the backend file with the monitor; add the classification and pause/resume cases here.
- `crates/telaviva-core/tests/core_contract.rs`: follow tests (`follow_stops_when_builtin_is_unavailable_or_follow_is_disabled` near line 3279); add the pause/resume test.
- `crates/telaviva-core/src/core.rs`: `builtin_display`, `recompute_follow`, `follow_target`; read only, behavior relied on.
- `README.md`: the "Follow the MacBook brightness" paragraph in Usage; add the pause behavior.
- `CHANGELOG.md`: Unreleased entry.
- `specs/archived/macbook-brightness-follow/validation/hardware-checklist.md`: add manual lid and screen-off checks (hardware acceptance stays pending).

## Step by Step Tasks

### 1. Classify the built-in state, including lid and sleep, in the backend

In `BuiltInDisplayBackend.swift`:

- Add `import IOKit` and a static `lidClosed()` on `DisplayServicesBackend` that reads `AppleClamshellState` from `IOPMrootDomain` (`IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"))` and `IORegistryEntryCreateCFProperty`), releases the service and property, and returns `false` when the key is missing (Mac mini and other desktops) or unreadable. Fail open on this signal only: the existing online, active and asleep checks still apply.
- Add a pure static function, for example `static func offlineReason(builtInFound: Bool, lidClosed: Bool, online: Bool, active: Bool, asleep: Bool) -> BuiltInUnavailableReason?`, with these rules: lid closed gives `.displayOffline`; no built-in display found and lid open gives `.noBuiltInDisplay`; found but not online, not active or asleep gives `.displayOffline`; otherwise `nil`.
- Make `status()` use it before the symbol and `canChange` checks. Keep `displayID` set whenever a built-in display was found, and keep the existing `.apiUnavailable` and `.brightnessNotAdjustable` results after it. Keep `isAsleep()` as it is (the monitor still uses it to skip reads).
- Comments in English, matching the file's doc-comment style.

### 2. Refresh availability when the built-in screen sleeps or wakes

In `MenuBarController.swift`:

- In the existing `screensDidSleepNotification` observer, call `builtInMonitor.refreshAvailability()` in addition to `calibration.end(.displaySleep)`.
- Add a `screensDidWakeNotification` observer (new `screensWakeObserver` property next to `wakeObserver`) that calls `builtInMonitor.refreshAvailability()` and once more after about 2 seconds with `DispatchQueue.main.asyncAfter`, because the display state can still read asleep at the instant of the notification. Remove it where the other observers are removed.
- Keep the same comment about availability being event driven with no polling; do not add a timer.

### 3. Update the unavailable messages

In `Localization.swift` change `ambient.unavailable.displayOffline` in both locales:

- en-US: "The built-in display is off, asleep or mirrored, for example with the lid closed. Following resumes on its own."
- pt-BR: "A tela integrada está desligada, em repouso ou espelhada, por exemplo com a tampa fechada. O recurso volta sozinho."

Mark the Portuguese strings the way the file already does (`// I18N` where the project convention requires it). Keep key parity between the two locales (checked by `CameraLocalizationTests.swift`).

### 4. Swift tests

In `BuiltInBrightnessMonitorTests.swift`:

- Table test for `offlineReason`: lid closed with the built-in missing from the list gives `.displayOffline` (not `.noBuiltInDisplay`); lid open with no built-in gives `.noBuiltInDisplay`; found but asleep gives `.displayOffline`; found, online, active and awake gives `nil`; lid closed with everything else healthy gives `.displayOffline`.
- Monitor pause/resume: with the monitor enabled and following at 40%, set the fake `statusValue` to unavailable with `.displayOffline`, call `refreshAvailability()`, and expect one `available: false` report with reason `.displayOffline`, no live repeating timer and no further reads; then set the status back to available with the level at 60% and call `refreshAvailability()` again, and expect one `available: true` report carrying 60, one live repeating timer, and no duplicate reports from repeated `refreshAvailability()` calls with unchanged status.
- Run `tools/tests/test-builtin-brightness-monitor.sh` and `tools/tests/test-camera-lifecycle.sh` (localization parity).

### 5. Rust contract test for pause and resume

In `core_contract.rs`, next to `follow_stops_when_builtin_is_unavailable_or_follow_is_disabled`, add `follow_pauses_while_builtin_is_unavailable_and_resumes_with_the_current_level`, using the existing `follow_core`/`follow_builtin`/`follow_pending`/`follow_ack` helpers:

1. Follow active and one write acknowledged.
2. Send `builtin_display` with `available: false`. Assert `ambient.active` is false, no pending actions, and the monitor's `follow` is still `"eligible"` (so the popup shows its slider again).
3. While paused, send a manual `set_brightness` for the monitor and assert it is accepted and produces a write (manual control is back).
4. Send `builtin_display` with `available: true` and a different level. Assert `ambient.active` is true and that the follow target for the new level is written (after the owned-monitor read if the core asks for one), overwriting the manual value.

If this reveals that the core does not behave this way, stop and report it instead of changing core policy inside this chore.

### 6. Documentation

- `README.md`: in the "Follow the MacBook brightness" paragraph, add that following pauses while the MacBook lid is closed or its screen is off, that manual brightness control (popup slider and, with keyboard routing, the brightness keys) returns during the pause, and that following resumes by itself when the lid opens or the screen turns on; monitors keep their last brightness during the pause.
- `CHANGELOG.md`: under Unreleased > Changed, one line in the style of its neighbors (hardware checks pending acceptance).
- `specs/archived/macbook-brightness-follow/validation/hardware-checklist.md`: add manual checks (lid closed with an external monitor; screen off with the lid open, for example display sleep; lid reopened; verify the popup slider and the brightness keys return and then disappear/pass through again). Mark them pending.

### 7. Run Validation Commands

Run every command in the Validation Commands section and fix any failure, then review `git diff` for unintended changes.

## Validation Commands

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
cargo build --release --target aarch64-apple-darwin -p telaviva-core
tools/tests/test-builtin-brightness-monitor.sh
tools/tests/test-ambient-calibration.sh
tools/tests/test-camera-lifecycle.sh
tools/tests/test-core-bridge.sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-popup-layout.sh
scripts/build-macos.sh
```

## Notes

- Status: steps 1 to 7 implemented and verified by automated checks (fmt, clippy, 87 core tests, the Swift scripts and `scripts/build-macos.sh`). Hardware acceptance of the new section I of the hardware checklist is pending.
- Hardware is not verified: the lid and screen-off behavior (and that `CGDisplayIsAsleep` and `AppleClamshellState` change as expected on Apple Silicon, including the clamshell case where the built-in display is removed from the online list) needs a MacBook with an external monitor. Report it as pending acceptance, not as done.
- `screensDidSleepNotification` also fires when only external displays sleep along with the rest. That is fine: the built-in display sleeps with them, follow pauses, and it resumes on wake with a fresh read. The core already withholds writes to sleeping monitors.
- On resume the follow target overwrites a manual brightness set during the pause. This is intentional ("until the lid is opened or the screen is turned on") and is stated in the README.
- If a MacBook has the lid closed and no external display, macOS sleeps the whole system; the existing system wake observer already refreshes availability then.
- Out of scope: the `specs/archived/snapshot-publish-amplification/plan.md` chore; calibration behavior (it already ends on display sleep and built-in unavailable); the archived `contracts.md` and `architecture.md` (they already describe `available:false` for lid closed; no ADR is needed because no decision changes).
- When implementing, follow the user's global Haiku-subagent rule (no end date): the orchestrator designs and verifies, Haiku agents read and edit. The project's Luna delegation guidance also applies.
