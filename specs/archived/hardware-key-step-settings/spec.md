# Chore: Configurable hardware-key step for brightness and volume

## Chore Description

The brightness and volume hardware keys move the monitor by a fixed 6.25 percentage points per press
(`HardwareKeyPolicy.normalStep`, with a fixed 1.5625-point step for Shift+Option). The user finds that too coarse.

Add two Settings controls, one for the brightness keys and one for the volume keys, each an integer from 1 to 15
percentage points. One press of `+` adds that many points and one press of `-` subtracts them, for the
brightness keys and for the volume keys separately. Also make the Settings window a little taller so the two new rows fit
without cramping.

Decisions already made (change them here before implementing if they are wrong):

- **Two independent settings**, `brightness_key_step` and `volume_key_step`, stored in the Rust core's `Settings`, integer
  `u8`, valid range 1..=15. They are additive fields in settings schema 1 with serde defaults (same pattern as
  `always_on_camera` and `hardware_keys_enabled`); no schema bump and no ABI change (`tv_submit` and `tv_snapshot`
  already carry settings as JSON).
- **Default is 5 for both.** The current 6.25 is not an integer, so it cannot be represented by a 1..15 integer control.
  5 is slightly finer than today, which matches the complaint. Existing users therefore get a 5-point step after
  upgrading, with no migration needed (the missing field takes the default).
- **Shift+Option fine step becomes a fixed 1 point** (it was 1.5625). It is the smallest configurable step, so fine is never
  larger than the normal step, even when the user sets the normal step to 1. The fine step is not configurable.
- **Scope is hardware keys only.** The popup sliders are continuous (`Slider(value:in: 0...100)`) and are not affected.
  Mute keeps its emulation (volume 0 and a remembered level); only the "restore then step up" path of the volume key uses
  the volume step.
- **Router stays free of core types.** `HardwareKeyRouter.swift` is compiled alone by `tools/tests/test-hardware-key-router.sh`,
  so it receives plain `Double` steps through a new setter, never `CoreSettings`.
- **Settings window height** goes from 560 to 640 points (about 80 points for two new `LabeledContent` slider rows plus
  spacing). Width and the 420 minimum stay.

## Relevant Files

Rust core:

- `crates/telaviva-core/src/settings.rs`: add the two fields with defaults to `Settings`, the `Default` impl and
  `validate()` (range 1..=15). `deny_unknown_fields` and the "corrupt file is renamed and defaults load" behavior in
  `load()` already apply, so an out-of-range hand-edited value resets settings like an out-of-range interval does today.
- `crates/telaviva-core/src/core.rs`: `update_settings` (around line 443) needs one match arm per new key; unknown keys
  currently return `unknown setting`. The snapshot serializes `self.settings` whole, so the values reach Swift without
  further work.
- `crates/telaviva-core/tests/core_contract.rs`: extend the defaults/validation test (line 19), and add a test modeled on
  `hardware_key_setting_is_optional_default_false_persisted_and_type_checked` (line 156).

Swift shell:

- `native/macos/TelaViva/HardwareKeyRouter.swift`:
  - `HardwareKeyPolicy` (line 148): replace `normalStep = 6.25` / `fineStep = 1.5625` with `defaultStep = 5`, `minStep = 1`,
    `maxStep = 15`, `fineStep = 1`; `stepped(_:direction:fine:)` (line 180) gains a `step` parameter.
  - `reserveStep` (line 648): pick the step by `feature` (brightness or volume) from router state, including the mute
    restore path at line 659.
  - Router state and setter next to `setDefaultOutput(uid:)` (line 411): new `setSteps(brightness:volume:)` that clamps to 1...15
    (and falls back to the default for non-finite input) and writes under `stateLock`. The tap callback runs
    `reserveStep` while holding `stateLock`, so reads must also happen there.
- `native/macos/TelaViva/CoreBridge.swift`: `CoreSettings` (line 295) needs two `Int?` properties and entries in `json`
  (line 308); otherwise a save from the Settings window drops them. They must be optional, because synthesized
  `Decodable` requires keys that are not optional even when a default value exists. Use `?? 5` at use sites, like
  `hardware_keys_enabled`.
- `native/macos/TelaViva/MenuBarController.swift`: the snapshot sink (line 56) forwards the two steps to the router
  next to `apply(enabled:displays:)`; the settings window `NSWindow(contentRect:)` at line 202 changes `height: 560` to `640`.
  The window is created once and reused, so the new height applies to a newly created window (the app restart for the test).
- `native/macos/TelaViva/SettingsView.swift`: two `@State` values, two slider rows in the "Keyboard controls" section,
  `.onChange` hooks, and `load()` / `save()`. Slider precedent: `LabeledContent` with `Slider(..., step: 1)` and a
  `monospacedDigit` value (lines 55-56).
- `native/macos/TelaViva/Localization.swift`: new keys in both the en-US (line 5) and pt-BR (line 6) dictionaries, and
  the stale `hardwareKeys.help` text that still states 6.25 / 1.5625 points in both languages.
- `native/macos/Tests/HardwareKeyRouterTests.swift`: literal expectations derived from 6.25 (56.25, 62.5, 26.25, 36.25,
  46.25, 48.4375 at lines 245, 265, 308, 319, 358, 383, 411, 432, 576, 654) and the step-math assertions (lines 243-246)
  need recomputing for the new defaults, plus new cases for custom steps.
- `native/macos/Tests/CoreBridgeTests.swift`: line 204 uses `HardwareKeyPolicy.normalStep`; the real-core hardware key tests
  (lines 186-210 and `hardwareBrightnessKeyDuringProtections`) must keep passing, and a new `CoreSettings`
  round-trip test is added (no test encodes or decodes `CoreSettings` today).

Documentation:

- `README.md`: line 33 (Settings list) and line 37 ("Normal steps are 6.25 percentage points; Shift+Option uses 1.5625 points").
- `api.md`: the settings field list (around line 27) and the hardware keys section (around lines 49-55); additive under ABI 1.
- `ARCHITECTURE.md`: the hardware keys section (around lines 129-133).
- `CHANGELOG.md`: Unreleased, Added (new setting) and Changed (default step 6.25 to 5, Shift+Option 1.5625 to 1).

Not needed: `include/telaviva_core.h` (no ABI change), `locales/*.ftl` (core snapshot messages only), `native/macos/TelaViva.xcodeproj` and
`packaging/macos/build-app.sh` (no new Swift file), archived specs (historical; they keep the old 6.25 text), a new ADR
(additive setting that does not change ADR 0004's decision).

### New Files

None.

## Step by Step Tasks

### 1. Add the settings fields to the Rust core

- In `settings.rs`, add `brightness_key_step: u8` and `volume_key_step: u8`, each `#[serde(default = "default_key_step")]`,
  with `fn default_key_step() -> u8 { 5 }`, in the struct after `hardware_keys_enabled`, and in `impl Default` (both 5).
- In `validate()`, after the interval checks, reject a value outside `1..=15` with `"brightness key step out of range"` and
  `"volume key step out of range"`.
- In `core.rs` `update_settings`, add arms `"brightness_key_step"` and `"volume_key_step"`:
  `val.as_u64().ok_or("key step must be integer")` then `u8::try_from(..).map_err(|_| "key step out of range")?`. Reject
  fractional numbers (`5.5`), strings, booleans and negatives (`as_u64` already returns `None` for them). `validate()` then
  enforces 1..=15 and the update is not persisted on failure.
- Keep comments in English.

### 2. Rust tests

- `core_contract.rs`: defaults are 5 and 5 and validate; `validate()` rejects 0 and 16 for each field; a settings file
  without the fields loads with 5/5; `update_settings` accepts 1 and 15, persists to disk, and returns the value in the snapshot;
  it rejects 0, 16, 5.5, "5", true, null and -1 for each key without changing stored settings; one key can be updated
  without touching the other.

### 3. Make the router step configurable

- In `HardwareKeyPolicy`: `defaultStep = 5.0`, `minStep = 1.0`, `maxStep = 15.0`, `fineStep = 1.0`; remove `normalStep`.
  Add `static func clampedStep(_ value: Double) -> Double` (non-finite returns the default, otherwise clamps and rounds
  to the nearest integer). Change `stepped` to `stepped(_ value:, direction:, fine:, step:)` using `fine ? fineStep : step`.
- In `HardwareKeyRouter`: two private vars `brightnessStep` and `volumeStep` initialized to `defaultStep`, and
  `func setSteps(brightness: Double, volume: Double)` that clamps through `clampedStep` and assigns under `stateLock`
  (copy the `setDefaultOutput` locking pattern). The setter does not clear intents or the mute memory.
- In `reserveStep`, compute `let step = feature == .volume ? volumeStep : brightnessStep` once and pass it to both
  `stepped` calls (the mute restore path and the normal path). Do not change `reserveMute`, which has no step.
- Keep the router free of `CoreSettings`/`CoreBridge` references so `tools/tests/test-hardware-key-router.sh` still compiles it alone.

### 4. Router tests

- Replace `HardwareKeyPolicy.normalStep` uses and recompute the literal expectations for the 5-point default
  (for example 50 + 5 = 55) and the 1-point fine step. Keep each test's intent.
- Add cases: brightness 1 and volume 15 applied independently in the same session; `setSteps` with 0, 99, NaN and 7.4 is
  clamped (1, 15, 5, 7); Shift+Option moves exactly 1 point regardless of the configured step; a step change between
  presses applies to the next press and a key already held keeps working; clamping at 0 and 100 with a step of 15; mute
  then a volume-up press restores the remembered level and adds the volume step; a brightness step change does not
  alter volume.

### 5. Wire Swift settings and the router

- `CoreBridge.swift`: add `var brightness_key_step: Int?` and `var volume_key_step: Int?` to `CoreSettings` and
  `"brightness_key_step": brightness_key_step ?? 5`, `"volume_key_step": volume_key_step ?? 5` to `json`.
- `MenuBarController.swift`: in the snapshot sink, before `apply(enabled:displays:)`, call
  `hardwareKeyRouter.setSteps(brightness: Double(settings.brightness_key_step ?? 5), volume: Double(settings.volume_key_step ?? 5))`
  using `snapshot.settings` (defaults when absent). Change `height: 560` to `height: 640` on line 202.
- Add a `CoreBridgeTests.swift` test: `CoreSettings` decodes a snapshot with and without the new keys (nil becomes 5 at the
  use site) and `json` emits both, and a real-core round trip (`updateSettings` with 3 and 12 reaches the snapshot, and the
  router steps by 3 for brightness and 12 for volume through `processForTesting`). Update line 204 to the new constant.

### 6. Settings UI and localization

- `SettingsView.swift`: `@State private var brightnessKeyStep = 5.0` and `volumeKeyStep = 5.0`. In the hardware keys section,
  below the status caption and above the permission rows, add two rows, always visible and `.disabled(!hardwareKeys)`:
  `LabeledContent(AppText.string("hardwareKeys.brightnessStep", locale: locale)) { HStack { Slider(value: $brightnessKeyStep, in: 1...15, step: 1); Text("\(Int(brightnessKeyStep)) pt").monospacedDigit() } }`
  and the same for `volumeKeyStep`. Add `.onChange` hooks to `save()`, read the values in `load()` (`Double(value.brightness_key_step ?? 5)`)
  and write them in `save()` (`Int(brightnessKeyStep.rounded())`).
- `Localization.swift`: add `hardwareKeys.brightnessStep` ("Brightness key step" / "Passo da tecla de brilho"),
  `hardwareKeys.volumeStep` ("Volume key step" / "Passo da tecla de volume") and, if the row needs a hint,
  `hardwareKeys.stepHelp` ("Percentage points per press (1 to 15)." / "Pontos percentuais por toque (1 a 15)."). Rewrite
  `hardwareKeys.help` in both languages to drop the 6.25 / 1.5625 numbers: the step is configured below and Shift+Option
  moves 1 point. Keep each dictionary on its single line, as is.
- The value label uses "pt" in both languages (a short percentage-point unit); do not mix it with the `%` of the sliders.

### 7. Documentation

- `README.md`: add the two step controls to the Settings list (line 33) and replace the 6.25 / 1.5625 sentence (line 37) with
  "The step for each key pair is set in Settings (1 to 15 percentage points, 5 by default); Shift+Option uses 1 point."
  Keep the existing caveats (single LG ULTRAFINE hardware verification; physical acceptance pending).
- `api.md`: list `brightness_key_step` and `volume_key_step` (integer, 1..=15, default 5, additive in schema 1) next to
  `hardware_keys_enabled`, and fix any statement of the old step sizes.
- `ARCHITECTURE.md`: update the hardware keys section for configurable steps, the fixed 1-point fine step and the stateLock-guarded setter.
- `CHANGELOG.md`: under Unreleased, Added: configurable key steps; Changed: default step 6.25 to 5 and fine step 1.5625 to 1.

### 8. Run Validation Commands

Run every command in the Validation Commands section and fix any failure. Then rebuild the app for the manual check.

## Validation Commands

```sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo build --release --target aarch64-apple-darwin -p telaviva-core
tools/tests/test-hardware-key-router.sh
tools/tests/test-core-bridge.sh
tools/tests/test-manual-monitor-controls.sh
tools/tests/test-camera-lifecycle.sh
tools/tests/test-default-audio-output.sh
tools/tests/test-media-key-probe.sh
tools/tests/test-ddc-transaction.sh
./scripts/build-macos.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
grep -rn "6\.25\|1\.5625\|normalStep" README.md api.md ARCHITECTURE.md native crates
```

The last command must find no remaining reference to the old step values outside `CHANGELOG.md` and archived specs.
Verify the smoke-test executable name against `.github/workflows/macos.yml` before running it.

Manual check (hardware, run by the user after the build):

1. Open Settings: the window is taller, both new sliders show 5 pt, and nothing is clipped in English and in Portuguese.
2. Set brightness to 1 and volume to 15, press brightness up and volume up on the LG ULTRAFINE: one point and fifteen points per press.
3. Hold a key: repeats use the configured step. Shift+Option moves 1 point.
4. Quit and reopen the app: both values persist.
5. With the keyboard routing switch off, the sliders are disabled and the keys keep normal macOS behavior.

## Notes

- **Default value risk:** 5 is a judgment call because 6.25 cannot be an integer. If 6 or 3 feels better, change `default_key_step`,
  `Default`, `HardwareKeyPolicy.defaultStep`, the `?? 5` fallbacks and the documentation in one pass; search for `5` near
  `key_step`.
- **Settings window size:** `openSettings` creates the window once, and `NSHostingController` may impose the SwiftUI content's
  preferred size on assignment, so 640 might not be the final height. Confirm in the running app (step 1 of the
  manual check) and adjust the `minHeight: 420` frame, or the content, if the window does not grow. `App.swift` also declares a SwiftUI
  `Settings` scene with `minHeight: 420`; it is unrelated to the AppKit window and stays as is.
- **Step units:** steps are in normalized percentage points (0-100). The DDC layer converts to raw units with rounding,
  so on a monitor whose raw maximum is not 100 the actual hardware change can differ by one raw unit; read-back
  verification already handles this.
- **Out-of-range file edits:** a hand-edited `settings.json` with a step outside 1..15 is treated like an invalid interval:
  the file is renamed to `settings.json.corrupt` and defaults load. This is existing core behavior; no special case is added.
- **Verification limits:** the Swift UI has no automated test (no test compiles `SettingsView.swift`), so the
  window height and the layout rely on the manual check. Hardware behavior has been verified so far only on one LG
  ULTRAFINE over DisplayPort with one keyboard; do not extend that claim in the docs.
- The fine step stays derived from nothing: 1 point fixed. If Shift+Option should scale with the configured step, make
  `fineStep` a function of `step` and update tasks 3 and 4.
