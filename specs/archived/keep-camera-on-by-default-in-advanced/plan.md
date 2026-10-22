# Chore: Keep camera on by default and move it to Advanced

## Chore Description

The `always_on_camera` setting (UI label "Keep camera on" in en-US and "Manter a câmera ligada" in pt-BR) changes in three ways, on macOS and Windows in the same change:

1. **Default becomes `true`.** New installs, and settings files without the key, start with the camera session kept between checks. Existing installs keep the value already saved. The core always serializes the field, so existing `settings.json` files already contain `"always_on_camera": false` and stay that way. The owner chose this over a one-time migration, which would need a schema bump or marker field and would make older builds reject the file (`deny_unknown_fields`, then `backup_corrupt` resets every setting). No schema change and no ABI change.
2. **The toggle moves from General > OLED protection to the Advanced page**, in a new "Camera" section above "Inference device". The existing `alwaysOnCamera` label and `alwaysOnCameraHelp` text stay as they are.
3. **New warning text under the toggle** (new key `alwaysOnCameraWarning`). It says that turning the option off can cause detection problems, depending on the camera, and can produce "insufficient light" events. The event name uses the label the app already shows for that status (`cameraStatus.insufficient_light`: "insufficient light" / "pouca iluminação"), so users can recognize it:
   - en-US: `Turning this off can cause detection problems, depending on the camera, and can produce "insufficient light" events, because the camera is then opened only for each check.`
   - pt-BR: `Desmarcar esta opção pode causar problemas de detecção, dependendo da câmera, e gerar eventos de "pouca iluminação", porque a câmera passa a ser aberta só a cada verificação.`

Runtime behavior does not change for a given value. `capture_mode` still comes from `core.rs:1838-1845`, and inference cadence does not change.

## Relevant Files

Rust core:

- `crates/telaviva-core/src/settings.rs`: field at :43 (`#[serde(default)]`, which resolves to `bool::default()` = false, not to `impl Default`) and `impl Default` at :261 (`always_on_camera: false`). The `yes()` helper at :207 already exists (it is used by `suspend_presence_on_battery`).
- `crates/telaviva-core/tests/core_contract.rs`: tests that assume the default is false:
  - :25 `assert!(!defaults.always_on_camera)`.
  - :50 `assert!(!old_schema.always_on_camera)`: a file without the key.
  - :129 `assert!(!settings.always_on_camera)`: an old file without the key.
  - :906 `always_on_capture_is_opt_in_and_keeps_return_mode`, whose :921 expects `single_frame` from default settings.
  - :2074 `returning_sample_restores_owned_brightness_and_cadence_is_one_second`, which expects `single_frame` at :2090 and :2112.
  - The per-monitor protection test that expects `single_frame` at :4114.
- `crates/telaviva-core/tests/ffi_contract.rs:103`: sets `always_on_camera: true` and asserts true at :113. True is now the default, so the test no longer proves that the value is transported.

macOS shell:

- `native/macos/TelaViva/SettingsView.swift`:
  - :23 `@State private var alwaysOnCamera = false`.
  - :169-170 the toggle inside General > `Section("protection")`.
  - :230-233 `advancedTab`, which is only `InferenceSettingsView`.
  - :305 load `?? false`.
  - :392-430 `InferenceSettingsView`, which wraps its own `Form { Section } .formStyle(.grouped).padding()`.
  - Caption pattern at :411-414 (`.font(.caption).foregroundStyle(.secondary)`).
- `native/macos/TelaViva/CoreBridge.swift:459`: `"always_on_camera": always_on_camera ?? false`.
- `native/macos/TelaViva/PresenceCamera.swift:158`: `snapshot.settings?.always_on_camera ?? false`.
- `native/macos/TelaViva/Localization.swift`: en-US dictionary on line 5 and pt-BR on line 6. `alwaysOnCamera` and `alwaysOnCameraHelp` exist, and so does `camera` ("Camera" / "Câmera"), which is reused as the section title.

Windows shell:

- `native/windows/Platform/SettingsModel.h:22`: `bool alwaysOnCamera = false;`.
- `native/windows/Platform/JsonCodec.h:52`: `bool always_on_camera = false;`.
- `native/windows/Platform/JsonCodec.cpp:876`: `boolOr(*settings, "always_on_camera", false)`.
- `native/windows/TelaViva/GeneralPage.xaml.cpp:407-413`: the toggle row to remove. `makeSwitch` at :117 is a file-local helper.
- `native/windows/TelaViva/AdvancedPage.xaml.cpp` and `.xaml.h`:
  - `RefreshStrings()` rebuilds `RowsPanel` (section header `inference.title`, then the device row and caption).
  - `SetHost()` calls `fillDeviceBox()`.
  - `edit()` at :160.
  - `suppress_` guards programmatic changes.
  - `host_` is null in the constructor's `RefreshStrings()` call.
- `native/windows/TelaViva/Host/SettingRow.h`: `MakeSettingRow`, `MakeCaption` and `MakeSectionHeader`.
- `native/windows/TelaViva/Strings/en-US/Resources.resw` and `Strings/pt-BR/Resources.resw`: `alwaysOnCamera` and `alwaysOnCameraHelp` at :178-182. `camera` is at :214.
- `native/windows/Tests/settings_payload.cpp`: :212 expects `"false"` for a default draft. The round-trip test at :431 and :442 uses `true`, which is now the default.
- `native/windows/Tests/strings_keys.cpp`: generic en-US/pt-BR key parity. It covers the new key automatically.

Docs:

- `docs/user-guide.md`:
  - :40 `- **Keep camera on:** off by default.`
  - :45 lists the Advanced page items.
  - :60 "(off by default)".
  - :75-77 "### Low light".
- `api.md:36`: "Defaults … disable … always-on capture" and "Missing `always_on_camera` and `hardware_keys_enabled` default to false".
- `ARCHITECTURE.md:122` and :132: "opt-in" wording.
- `adr/0003-camera-lifecycle-and-display-suspension.md:14`: "Add an opt-in `always_on_camera` setting, defaulting to false." It already has a dated amendment (2026-10-16), and `adr_policy: full` applies.
- `CHANGELOG.md`: the `## Unreleased` section is empty.

## Step by Step Tasks

### 1. Core default (Rust)

- In `settings.rs:43`, change `#[serde(default)]` on `always_on_camera` to `#[serde(default = "yes")]`.
- In `settings.rs:261`, change `always_on_camera: false` to `always_on_camera: true`.

### 2. Core tests (Rust)

- In `core_contract.rs:25`, change the assertion to `assert!(defaults.always_on_camera);`.
- In `core_contract.rs:50`, change it to `assert!(old_schema.always_on_camera);`. Keep the explicit `true` parse at :53-56, and add the opposite case: `{"schema_version":1,"always_on_camera":false}` parses to false, so an existing saved false survives.
- In `core_contract.rs:129`, change it to `assert!(settings.always_on_camera);`, because the old file has no key.
- In `core_contract.rs:906`:
  - Rename the test to `always_on_capture_is_default_and_keeps_return_mode`.
  - The first `update_settings` (presence on) now expects `continuous_monitoring`.
  - Then send `{"always_on_camera":false}` and expect `single_frame`.
  - Then send `{"always_on_camera":true}` and keep the existing `face` and `continuous_monitoring` assertions and the return-mode tail.
- In `core_contract.rs:2074` and the test around :4114, add `"always_on_camera":false` to the `update_settings` that enables presence, so the tests keep checking single-frame behavior.
- In `ffi_contract.rs:103`, send `"always_on_camera":false` and assert `snapshot["settings"]["always_on_camera"] == false`, so the test proves a non-default value is transported.
- Run `cargo test -p telaviva-core`. For any other failure caused only by the new default, pin `"always_on_camera":false` in that test's settings when it tests single-frame behavior. Do not change production logic.

### 3. macOS defaults

- In `SettingsView.swift:23`, change the initial value to `= true`.
- In `SettingsView.swift:305`, change `?? false` to `?? true`.
- In `CoreBridge.swift:459`, change `?? false` to `?? true`.
- In `PresenceCamera.swift:158`, change `?? false` to `?? true`.

### 4. macOS UI: move to Advanced and add the warning

- In `Localization.swift`, add `"alwaysOnCameraWarning"` right after `"alwaysOnCameraHelp"` in both dictionaries, with the en-US text on line 5 and the pt-BR text on line 6 (texts are in the description above). Escape the inner quotes as `\"`.
- In `SettingsView.swift`, remove the `Toggle` and `.help` at :169-170 from the General tab.
- Change `InferenceSettingsView.body` to return only its `Section(...) { ... }`. Move the `Form`, `.formStyle(.grouped)` and padding out to the caller.
- Change `advancedTab` to:

  ```swift
  Form {
      Section(AppText.string("camera", locale: locale)) {
          Toggle(AppText.string("alwaysOnCamera", locale: locale), isOn: $alwaysOnCamera)
              .help(AppText.string("alwaysOnCameraHelp", locale: locale))
          Text(AppText.string("alwaysOnCameraWarning", locale: locale))
              .font(.caption).foregroundStyle(.secondary)
      }
      InferenceSettingsView(selection: $inferenceDevice, report: inferenceReport, locale: locale)
  }
  .formStyle(.grouped)
  .padding(20)
  ```

  Keep the doc comment, and update it to mention the camera option.
- Keep `.onChange(of: alwaysOnCamera) { _, _ in save() }` at :101. The state stays in `SettingsView`.

### 5. Windows defaults

- In `SettingsModel.h:22`, change the value to `bool alwaysOnCamera = true;`.
- In `JsonCodec.h:52`, change the value to `bool always_on_camera = true;`.
- In `JsonCodec.cpp:876`, change the call to `boolOr(*settings, "always_on_camera", true)`.

### 6. Windows UI: move to Advanced and add the warning

- In both `Resources.resw` files, add `<data name="alwaysOnCameraWarning" xml:space="preserve"><value>…</value></data>` right after `alwaysOnCameraHelp`, with the en-US and pt-BR texts from the description above. The quotes can stay literal inside `<value>`.
- In `GeneralPage.xaml.cpp:407-413`, remove the `alwaysOnHeader` and `alwaysOnSwitch` block and its `Append`.
- In `AdvancedPage.xaml.h`, add a member `winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch alwaysOnSwitch_{ nullptr };` and a private `void syncAlwaysOn();`.
- In `AdvancedPage.xaml.cpp` `RefreshStrings()`, before the inference header:
  - Append `MakeSectionHeader(loc("camera"))`.
  - Create `alwaysOnSwitch_` with empty On and Off content and the automation name `loc("alwaysOnCamera")`, matching `makeSwitch` in `GeneralPage.xaml.cpp:117`. Add a local equivalent in the anonymous namespace; do not create a shared header.
  - In the `Toggled` handler, return when `suppress_ || !host_`. Otherwise call `edit([enabled](telaviva::SettingsDraft& draft) { draft.alwaysOnCamera = enabled; })`.
  - Append `MakeSettingRow(loc("alwaysOnCamera"), loc("alwaysOnCameraHelp"), alwaysOnSwitch_)`, then `MakeCaption(loc("alwaysOnCameraWarning"))`.
  - Call `syncAlwaysOn()` next to `fillDeviceBox()` at the end of `RefreshStrings()`.
- In `syncAlwaysOn()`, return when `!alwaysOnSwitch_ || !host_`. Otherwise set `suppress_ = true`, then `alwaysOnSwitch_.IsOn(host_->currentSettings().always_on_camera)`, then `suppress_ = false`.
- In `SetHost()`, call `syncAlwaysOn()` after `fillDeviceBox()`.

### 7. Windows tests

- In `settings_payload.cpp:212`, expect `std::string("true")`.
- In `settings_payload.cpp:431` and :442 (`settings_payload_round_trip_non_default_values`), change `"always_on_camera":true` to `"always_on_camera":false` in both the input and the expected payload.
- Add a case that parses a snapshot `settings` object without `always_on_camera` and checks that `draftFromSnapshotSettings(...).alwaysOnCamera` is true, so the `JsonCodec` fallback is covered.

### 8. Docs, ADR and changelog

- `docs/user-guide.md`:
  - :40: remove the "Keep camera on" bullet from the General list.
  - :45: add the item to the Advanced list: `- **Keep camera on** (Advanced page): on by default. Turning it off can cause detection problems, depending on the camera, and can produce insufficient-light events. See [Presence protection](#presence-protection).`
  - :60: change "(off by default)" to "(on by default, in Settings > Advanced)", and state that with it off, ordinary checks use one-frame sessions.
  - Low light (:75-77): add one sentence saying that one-frame sessions (Keep camera on turned off) are more likely to see a dark frame while the camera adjusts its exposure, depending on the camera.
- `api.md:36`: "disable launch at login, always-on capture, and hardware-key routing" becomes "disable launch at login and hardware-key routing, and enable always-on capture". "Missing `always_on_camera` and `hardware_keys_enabled` default to false" becomes "Missing `always_on_camera` defaults to true and missing `hardware_keys_enabled` defaults to false". Keep the sentence about older binaries.
- `ARCHITECTURE.md:122` and :132: replace "opt-in" with "default-on (since this change; existing saved values are kept)". Link the new ADR 0003 amendment.
- `adr/0003-camera-lifecycle-and-display-suspension.md`: append `## Amendment 2026-10-22: on by default` in the style of the existing 2026-10-16 amendment. It covers:
  - The default is now true for new settings and for files without the key, and existing saved values are kept.
  - Why no migration: no schema bump, and the downgrade safety given `deny_unknown_fields`.
  - The setting moved to the Advanced page on both platforms.
  - The owner's observation that one-frame sessions can, depending on the camera, hurt detection and produce insufficient-light results.
  - Hardware not measured.
- `CHANGELOG.md` under `## Unreleased`, add a `### Changed` bullet: `- Keep camera on is now on by default and moved to Settings > Advanced on macOS and Windows, with a note that turning it off can cause detection problems, depending on the camera, and "insufficient light" events. Hardware checks are pending.` Then add `Upgrade note: existing installs keep their saved value; turn the option on in Settings > Advanced.`

### 9. Run the Validation Commands

Run every command below and fix any failure before finishing.

## Validation Commands

- `cargo fmt --all -- --check`
- `cargo clippy --workspace --all-targets --locked -- -D warnings`
- `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu`
- `cargo test -p telaviva-core --test core_contract --test ffi_contract`
- `tools/tests/test-core-bridge.sh` and `tools/tests/test-camera-lifecycle-bridge.sh` (after `cargo build --workspace --release --target aarch64-apple-darwin --locked`), then `scripts/build-macos.sh` to confirm `SettingsView.swift` and `Localization.swift` compile.
- Windows (on a Windows machine or the Windows CI workflow, `scripts/build-windows.ps1`): build `native/windows` and run the `Tests` executable, including `settings_payload`, `strings_keys` and `camera_lifecycle_driver`.
- `rg -n "alwaysOnCamera" native/macos/TelaViva/SettingsView.swift native/windows/TelaViva/GeneralPage.xaml.cpp`: the only remaining matches are the `@State`, `onChange`, load and save lines in `SettingsView.swift` and none in `GeneralPage.xaml.cpp`.
- `rg -n "off by default" docs/user-guide.md`: there is no match for Keep camera on.
- Manual check on both platforms: in a fresh profile, Settings > Advanced shows Camera > Keep camera on, checked, with the warning caption under it in en-US and pt-BR. General no longer shows it. Toggling it persists across a restart.

## Notes

- **Existing installs are not migrated** (the owner's decision). The CHANGELOG upgrade note tells them where to turn it on.
- **Wording of the event:** the request said "baixa luminosidade". The app's visible status is "pouca iluminação" / "insufficient light", so the warning uses that label. Change both `alwaysOnCameraWarning` strings if the owner prefers the literal wording.
- **The warning is an owner observation, not a measured result.** ARCHITECTURE.md:217 documents a 5 s dark-settle for single-frame face samples, which was meant to prevent false low-light events. The ADR amendment should say that camera-dependent problems remain and that hardware has not been measured.
- **Battery and energy:** default-on keeps the camera session (and LED) open between checks at the capped low frame rate (ARCHITECTURE.md:124). Battery suspension still applies by default. Record this trade-off in the ADR amendment, given AGENTS.md's minimal-resource priority.
- `core_contract.rs:1220` (`multiple_monitors_keep_camera_on_…`) is unrelated to this flag; leave it alone.
- No change to `Cargo.toml` versions; this is an Unreleased entry.

## Implementation Notes (2026-10-22)

All nine steps are done on both platforms.

- Beyond the plan: `core_contract.rs` also pins `"always_on_camera":false` in the `none` core of `protection_monitor_unprotected_only_camera_matches_no_monitor_state`; the two Windows snapshot fixtures (`snapshot-basic.json`, `snapshot-with-action.json`) now say `"always_on_camera":true`, because the round-trip tests compare them with the default draft; and the stale "opt-in" and "default off" sentences in ADR 0003 (Alternatives, Consequences) were updated.
- Verified on macOS: `cargo test -p telaviva-core --locked`, `cargo fmt --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, the Swift typecheck of the app sources, `tools/tests/test-camera-lifecycle.sh`, `test-status-item-icon.sh`, `test-core-bridge.sh` and `test-camera-lifecycle-bridge.sh`. The en-US and pt-BR `.resw` key sets match.
- Not verified: the Windows build and test run (needs Windows or its CI), the full `cargo test --workspace`, and the manual UI checks on both platforms. Archived specs still say the default was false; they are historical and were left alone.
