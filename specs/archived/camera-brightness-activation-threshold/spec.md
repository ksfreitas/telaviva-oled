# Chore: Camera brightness activation threshold

## Chore Description

Two requests about when presence checking (the camera) may run:

1. **Run only while a compatible monitor is active.** Confirmed while scoping: the core already does this. `Core::camera_mode` (`crates/telaviva-core/src/core.rs`) returns `suspended` (capture `stopped`, `next_sample_in_ms` null, display status `suspended_no_monitor` or `suspended_display_sleep`) unless at least one monitor is camera-eligible (`monitor_is_camera_eligible`: protected, stable identity, supported brightness, known baseline) and not every eligible monitor is asleep (`all_eligible_displays_asleep`). One awake eligible monitor is enough; the camera does not need exactly one monitor. This is covered by `camera_display_status_tracks_capture_success_and_policy_states` and `display_sleep_suspends_only_when_all_eligible_displays_sleep_and_ignores_stale_state`. No behavior change is needed for item 1, but the new threshold below builds on the same rule, so the plan keeps these tests passing and adds the wording to the docs. (The request text says "só houver um monitor"; it is read as "at least one compatible active monitor", matching the "talvez já faz isso" remark. If "exactly one" was meant, say so before implementing.)

2. **Brightness activation threshold.** New setting, a percentage slider in Settings, default **20%**. The camera is activated only while at least one eligible, awake monitor has a brightness **above** the threshold; otherwise presence checking is suspended (camera stopped). Example: at 20%, a monitor at 15% and another at 20% keep the camera off; raising either to 21% turns it on. Rationale: at very low brightness there is nothing to protect, so the camera need not run.

Design decisions (made here so implementation needs no second round):

- Setting name: `presence_min_brightness_percent` (f64, 10 to 90, default 20), additive to schema 1 like `brightness_key_step`; no ABI change. Missing in an old file loads as 20. Comparison is strict (`level > threshold`), so the highest setting (90) needs a monitor above 90%, and the lowest (10) a monitor above 10%.
- Level used for a monitor, normalized 0 to 100 from `min`/`max`: the `baseline` (the user's intended level) while the monitor is `owned` by the app or has protection reasons (dimmed by presence or screensaver, or a restore write is pending); otherwise the `observed` value. Using the baseline keeps the camera from stopping when the app itself dims the monitor (the screensaver, or a presence dim whose return check must keep running) and from flapping between a presence restore request and its hardware result. An unknown level (`observed` is `None`) counts as above the threshold, so an unreadable monitor never silently disables protection.
- A monitor with `presence_dimmed == true` always counts as above the threshold: it was dimmed only because it was above, and the return check must keep running to restore it.
- Transitions: when a monitor enters the below-threshold set, its miss counter resets; when a monitor that is still connected leaves it (brightness rises above the threshold, or the threshold is lowered), the camera starts a clean resume (`begin_camera_resume`: first sample after about 1 second, light state cleared). Either transition bumps `camera_generation` and clears `last_presence`, so in-flight samples are dropped. A newly discovered monitor never triggers a resume by itself (discovery already bumps the generation), which keeps startup cadence unchanged.
- New `camera.display_status` value `suspended_low_brightness`, used when the camera is suspended only because no eligible awake monitor is above the threshold. Precedence: battery states, then `suspended_display_sleep` (all eligible asleep), then `suspended_low_brightness` (an eligible monitor exists but none is above), then `suspended_no_monitor`.
- The shell does not poll monitor brightness (`monitor_observed` is not sent by the macOS app; levels refresh on discovery, display change, wake, the popup refresh button and the app's own writes). A brightness change made on the monitor's own buttons is therefore noticed only at the next refresh. This is a known limit, flagged in Notes; the plan does not add DDC polling.

## Relevant Files

- `crates/telaviva-core/src/settings.rs`: `Settings` struct, `Default`, `validate`; add the field with a serde default of 20 and a 10 to 90 finite check.
- `crates/telaviva-core/src/core.rs`: `update_settings` (add the key), `camera_mode`, `camera_display_status`, `reconcile_camera`, `submit_json` (hook for the transition sync), `Monitor`/`Core` runtime state, `feature_json` (normalization formula to reuse). Core of the change.
- `crates/telaviva-core/tests/core_contract.rs`: settings default/range tests near line 20 and 217, camera tests near lines 560 to 810; add the threshold tests and check existing fixtures (one uses brightness `0`; the rest use 25 to 80).
- `crates/telaviva-core/tests/ffi_contract.rs`: transports settings through the ABI; add the new key to a round-trip assertion.
- `native/macos/TelaViva/CoreBridge.swift`: `CoreSettings` (properties, `CodingKeys`, `json`); add the optional field, sent only when set, like the key steps.
- `native/macos/TelaViva/SettingsView.swift`: General tab, protection section; add the slider, `@State` draft, `load()`, `save()`, `onChange`.
- `native/macos/TelaViva/Localization.swift`: new strings for the slider label and help, and `cameraStatus.suspended_low_brightness`, in en-US and pt-BR (key parity is checked by the Swift tests).
- `locales/en-US.ftl` and `locales/pt-BR.ftl`: add `camera-suspended-low-brightness` next to `camera-suspended-display-sleep`, for parity.
- `native/macos/Tests/CoreBridgeTests.swift`: settings decode/encode tests (see lines 260 and 444 for the pattern).
- `native/macos/Tests/CameraLocalizationTests.swift`: confirms the new status string exists in both locales.
- `api.md`: settings list (line 27), `display_status` list (line 29), and the camera lifecycle section.
- `ARCHITECTURE.md`: camera lifecycle section (around lines 117 to 129); add the threshold rule.
- `README.md`: the paragraph under "Presence protection" and the Usage list that describe when capture stops.
- `CHANGELOG.md`: Unreleased entries.
- `native/macos/TelaViva/MacDDCController.swift`, `MenuBarController.swift`, `CameraLifecycle.swift`: read only. The lifecycle already follows `camera.mode`/`capture_mode` from the snapshot, so suspension needs no shell change.

## Step by Step Tasks

### 1. [x] Add the setting to the core

In `settings.rs`:
- Add `#[serde(default = "default_presence_min_brightness")] pub presence_min_brightness_percent: f64` to `Settings`, a `default_presence_min_brightness()` returning `20.0`, and `presence_min_brightness_percent: 20.0` in `impl Default`.
- In `validate`, return `Err("presence brightness threshold out of range")` unless the value is finite and within `10.0..=90.0`.

In `core.rs` `update_settings`, add a match arm `"presence_min_brightness_percent"` that reads `val.as_f64()` (error "threshold must be number") into `next`. `validate()` already runs on `next` before storing.

### 2. [x] Gate the camera on brightness in the core

In `core.rs`:
- Add `fn percent(f: &Feature, raw: f64) -> f64` (or a small free function `normalized(raw, min, max)`) using the same formula as `feature_json` (`(raw - min) * 100 / (max - min)`, 0 when `max <= min`), and make `feature_json` reuse it.
- Add `fn monitor_level_percent(m: &Monitor) -> Option<f64>`: use `m.baseline` when `m.brightness.owned || !m.brightness.reasons.is_empty()`, else `m.brightness.observed`; `None` when the chosen value is `None`.
- Add `fn monitor_below_threshold(&self, m: &Monitor) -> bool`: `Self::monitor_is_camera_eligible(m, self.is_protected(&m.id)) && !m.presence_dimmed && monitor_level_percent(m).is_some_and(|level| level <= self.settings.presence_min_brightness_percent)`.
- Add `fn monitor_is_camera_active(&self, m: &Monitor) -> bool`: eligible, `display_state != "asleep"`, and not `monitor_below_threshold`.
- In `camera_mode`, replace the "no eligible monitor" and "all eligible displays asleep" conditions with `!self.monitors.values().any(|m| self.monitor_is_camera_active(m))`. The battery and camera-error conditions stay as they are. Keep `all_eligible_displays_asleep` for `reconcile_camera` and for the display status.
- In `camera_display_status` add the `suspended_low_brightness` arm described under Design decisions, between `suspended_display_sleep` and `suspended_no_monitor` (true when any eligible monitor exists).
- Add a runtime field `below_threshold: BTreeSet<String>` to `Core` (initialized empty, never persisted) and `fn sync_brightness_gate(&mut self)`: compute the new set of monitor IDs for which `monitor_below_threshold` is true; compute `entered` (in new, not in old) and `left` (in old, not in new, and still present in `self.monitors`); store the new set; if both are empty, return. Otherwise `camera_generation += 1`, `last_presence = None`, reset `misses = 0` for the entered monitors, call `begin_camera_resume()` when `left` is not empty, then `reconcile_camera()`.
- Call `sync_brightness_gate()` in `submit_json` after the command `match` and before `self.revision` is incremented. Because the arms use `?`, a rejected command skips it, which is intended. Verify that `begin_camera_resume` does nothing while presence protection is disabled (it returns early), so a disabled feature is unaffected.
- Comments in English, concise, in the style of the surrounding code.

### 3. [x] Core tests

In `core_contract.rs` (follow the existing `send`, `capture_time` and discovery JSON helpers):
- Settings: default is `20.0`; values `9.9`, `90.1`, `0`, `100`, `-1` and a non-number are rejected by `update_settings`; `10` and `90` accepted; a settings file without the key loads as 20 and one with the key round-trips (`Settings::from_json` and the persisted file, as the key-step tests do).
- Threshold gating: with presence enabled and a monitor at brightness 20, camera mode is `suspended`, capture `stopped`, display status `suspended_low_brightness`; observing 21 (a `monitor_observed` with matching generation) resumes `face` mode with `next_sample_in_ms` 1000 and a higher `camera.generation`; lowering the setting to 10 with the monitor at 20 does the same (10 is the minimum); raising it to 90 with the monitor at 20 suspends again.
- Strict comparison: a monitor at exactly the threshold is below; one just above is active.
- Multiple monitors: two eligible monitors, one at 10 and one at 50, keep the camera on; setting the second to 10 suspends it. A monitor that is asleep or has protection turned off (`protection_disabled`) does not count even when bright, while a bright awake one does.
- Dimming does not suspend: after two `absent` samples dim a monitor at 70 (observed becomes 0 after the hardware result), camera mode stays `face` with `continuous_return`; after the `present` sample and the restore write result, mode stays `face` with no intermediate `suspended` snapshot. A screensaver dim (monitor at 70, target 5%) also keeps the camera running.
- Manual set below the threshold (`set_brightness` to 10) suspends once the write's hardware result arrives (until then the observed level still counts); setting it back above resumes.
- Entering below-threshold resets absence progress: one `absent` sample at 70, then brightness set to 10 and back to 70, then one `absent` sample does not dim (miss count restarted).
- Unknown brightness (`observed` null, supported) does not suspend.
- Existing tests keep passing; adjust only fixtures whose brightness is at or below 20 (the `0` fixture) by raising it.

In `ffi_contract.rs`, send `{"type":"update_settings","settings":{"presence_min_brightness_percent":35}}` in the existing settings transport test and assert the snapshot reports 35.

### 4. [x] Shell: setting, slider and strings

- `CoreBridge.swift`: add `var presence_min_brightness_percent: Double?` to `CoreSettings`, add it to `CodingKeys` (raw name already snake_case), and in `json` send `presence_min_brightness_percent: value ?? 20` next to the key steps so an unrelated save never alters it.
- `SettingsView.swift`: add `@State private var presenceMinBrightness = 20.0`; in the protection section, after the presence toggle, add a `LabeledContent` with `Slider(value: $presenceMinBrightness, in: 10...90, step: 1)` and `Text("\(Int(presenceMinBrightness)) %").monospacedDigit()` plus a caption with the help text; disable it while `presence` is off; `.onChange` calls `save()`; `load()` and `save()` read and write the field (`value.presence_min_brightness_percent ?? 20`).
- `Localization.swift`: add to both locales `presenceMinBrightness` (en-US "Check presence only above brightness", pt-BR "Verificar presença só acima do brilho"), `presenceMinBrightnessHelp` (en-US "The camera stays off while every monitor is at or below this brightness.", pt-BR "A câmera fica desligada enquanto todos os monitores estiverem neste brilho ou abaixo."), and `cameraStatus.suspended_low_brightness` (en-US "suspended at low brightness", pt-BR "suspensa com brilho baixo"). Follow the project's `// I18N` marking for Portuguese strings if the surrounding code uses it.
- `locales/en-US.ftl` and `locales/pt-BR.ftl`: add `camera-suspended-low-brightness` with the same two texts.
- Swift tests: in `CoreBridgeTests.swift` assert the field decodes when present, is nil when absent, and is emitted as 20 when unset and as the stored value when set; in `CameraLocalizationTests.swift` (or the existing parity check) assert the new status key exists in both locales.

### 5. [x] Documentation

- `api.md`: add `presence_min_brightness_percent` (10 to 90, default 20, strict "above" comparison, additive to schema 1, missing loads as 20) to the settings paragraph; add `suspended_low_brightness` to the `display_status` list; in the camera lifecycle section describe the rule (any awake eligible monitor above the threshold permits monitoring; baseline is used while the app owns or dims the value; dimmed monitors keep the return check) and that item "no eligible monitor" suspension is unchanged.
- `ARCHITECTURE.md`: short paragraph in the camera lifecycle section with the date 2026-10-17 and the shell polling limit.
- `README.md`: in the presence paragraph add the threshold to the list of conditions where capture stops ("or every eligible monitor is at or below the brightness threshold, 20% by default") and mention the Settings slider in Usage step 3 or 4. Keep the "hardware acceptance pending" wording.
- `CHANGELOG.md`: Unreleased "Added": the configurable activation threshold; and a "Changed" line that presence checking now stays off while all eligible monitors are at or below 20% by default. Say that hardware acceptance is pending.

### 6. [x] Run the Validation Commands

Run every command below from the repository root; fix any failure at its cause.

## Validation Commands

Run from the repository root (the ONNX Runtime integration test is skipped as in CI):

```sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
cargo clippy --workspace --all-targets --locked -- -D warnings
tools/tests/test-camera-lifecycle.sh
tools/tests/test-core-bridge.sh
tools/tests/test-popup-layout.sh
tools/tests/test-manual-monitor-controls.sh
tools/tests/test-hardware-key-router.sh
```

`tools/tests/test-core-bridge.sh` needs `libtelaviva_core.a` built first (`cargo build --release -p telaviva-core`, as in `.github/workflows/macos.yml`). Then build the app to prove the Swift target compiles:

```sh
./scripts/build-macos.sh
```

## Notes

- Implementation notes (2026-10-17): the range was narrowed to 10 to 90 by the user before implementation, so the slider cannot be set to disable the check. A monitor with unknown (null) brightness has no baseline, so it was never camera-eligible; it neither counts toward activation nor suspends the camera when another monitor is above the threshold. A manual brightness change takes effect on the gate at once when the app already owns the monitor's value, and when the write result arrives otherwise. `tools/tests/test-core-bridge.sh` reads `target/aarch64-apple-darwin/release/libtelaviva_core.a`, so build the core with `--target aarch64-apple-darwin` before running it.

- Item 1 needs no code: the existing eligibility and display-sleep rules already suspend the camera without a compatible active monitor. The plan only adds regression coverage (the new tests keep those cases) and documentation.
- Behavior change for existing users: with the default 20%, a monitor kept at 20% or lower no longer gets presence protection (screensaver protection is unaffected, it does not use the camera). Lower the slider to its minimum (10%) to keep protection for most brightness levels. This is called out in the changelog.
- Brightness refresh limit: the macOS shell reads monitor brightness on discovery and after its own writes, not on a timer, so a level changed with the monitor's own buttons is noticed at the next display change, wake or popup refresh. Raising it above the threshold that way does not start the camera until then, and lowering it does not stop the camera. Adding periodic DDC reads was left out because it adds bus traffic and can disturb some monitors; it can be a follow-up if hardware use shows it matters.
- The threshold applies per eligible monitor, so a monitor with protection turned off, an ambiguous identity or unsupported brightness never counts toward activating the camera.
- The setting is limited to 10 to 90 (user decision, 2026-10-17), in both the core validation and the slider, so presence checking can neither be switched off by the threshold nor made to depend on near-zero brightness. A settings file with a stored value outside 10 to 90 is treated as invalid by the existing load path (backed up as corrupt, defaults used), like other out-of-range settings; no such file exists because the field is new.
- Hardware acceptance is not part of this chore; keep the README and checklist wording that physical behavior is unverified.
