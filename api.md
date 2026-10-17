# Public API

This page records the consumer-facing native interfaces that exist in the repository. The Rust core C ABI is implemented; the inference C ABI is also implemented. The JSON snapshot schema is an implementation contract consumed by the native macOS shell and by the Windows shell in the repository; the Windows status is in the [windows-version](#windows-version) section. The two C headers are checked against the Rust ABI by the `header_contract` tests.

## external-monitor-control-and-oled-presence-protection

### Native shell core ABI

Header: [`include/telaviva_core.h`](include/telaviva_core.h). ABI version: `1` (`TV_CORE_ABI_VERSION`). The interface uses opaque `TvCore` handles, UTF-8 JSON inputs, a revision-only snapshot callback, and Rust-owned snapshot buffers. C++ callers may include the header under `extern "C"`.

| Function | Contract |
|---|---|
| `tv_abi_version()` | Returns the supported ABI version. |
| `tv_create(version, options, options_len, callback, userdata, out_core)` | Creates a core when the requested version matches. Optional UTF-8 JSON options accept `config_path` and `os_locale`; `NULL`/zero length selects defaults. `os_locale` is still accepted for ABI compatibility but no longer affects snapshot content. |
| `tv_submit(core, json, json_len)` | Submits a UTF-8 JSON command, with a maximum input size of 64 KiB. Commands after shutdown return `TV_CLOSED`, except hardware results needed to complete pending work. |
| `tv_snapshot(core, out_buffer)` | Copies the current revisioned UTF-8 JSON snapshot into a Rust-owned buffer, capped at 8 MiB. |
| `tv_shutdown(core)` | Stops new callbacks/commands and emits best-effort ownership-checked restore actions. |
| `tv_destroy(core)` | Destroys a handle after the host handles shutdown actions; callers must not race it with other calls. |
| `tv_buffer_free(buffer)` | Frees and clears a buffer returned by `tv_snapshot` or `tv_camera_lifecycle_step`. |
| `tv_camera_lifecycle_create(out)` | Creates an inactive `TvCameraLifecycle` handle (mode `disabled`, no deadlines). Additive to ABI version 1. |
| `tv_camera_lifecycle_step(handle, json, len, out_buffer)` | Applies one UTF-8 JSON lifecycle event (maximum 64 KiB) and copies its result into a Rust-owned buffer. Additive to ABI version 1. |
| `tv_camera_lifecycle_destroy(handle)` | Destroys a lifecycle handle; callers must not race it with other calls on the same handle. Additive to ABI version 1. |

Return codes are `TV_OK=0`, `TV_INVALID_ARGUMENT=1`, `TV_ABI_MISMATCH=2`, `TV_CLOSED=3`, `TV_INTERNAL=4`, and `TV_SIZE_LIMIT=5`. The callback receives a revision and caller-owned `userdata`; it does not transfer a snapshot. Core buffers and pointers must follow the lifetimes stated in the header. Do not destroy a core concurrently with another call.

Implemented command envelope types are `monitor_discovered`, `monitor_disconnected`, `monitor_observed`, `monitor_display_state`, `presence_observation`, `camera_status`, `builtin_display`, `power`, `screensaver`, `set_brightness`, `set_volume`, `update_settings`, `hardware_result`, and `shutdown`. Unknown fields and duplicate JSON keys are rejected. The snapshot currently includes `revision`, `settings`, `monitors`, `pending_actions`, `camera`, `power`, `screensaver`, `ambient`, and top-level `status`. The snapshot no longer carries a `messages` field: the core returns status codes and no translated text, and each native shell owns its UI strings (see [ADR 0015](adr/0015-native-shells-own-ui-strings.md)). Pending hardware actions are dispatched by the native host; automatic ownership checks use `read_brightness` and require the raw readback to be returned as a `hardware_result`.

Each monitor snapshot reports `id`, `name`, `identity_status`, `connection_path`, `generation`, `display_state`, `presence_dimmed`, `brightness`, `volume`, `follow`, and `protected`. Each brightness/volume feature reports `supported`, `normalized`, `raw`, `min`, `max`, `observed`, `desired`, `status`, `owned`, and `reasons`. Each pending action reports `id`, `monitor_id`, `generation`, `kind`, `value`, and `target`. Discovery identities are `stable` or `ambiguous`; camera status inputs are `unknown`, `ready`, `unavailable`, `busy`, `permission`, or `error`. Presence observations currently accept `present`, `absent`, `insufficient_light`, `unavailable`, and `usable_light`; observation freshness and camera generation gate whether an event is applied.

Settings schema version `1` uses locale `auto`, `en-US`, or `pt-BR` (the core stores this setting and each native shell reads it to choose its own UI strings; the core translates nothing); presence sampling interval 5–300 seconds (default 10); return sampling interval 0.25–5 seconds (default 1); Boolean presence, screensaver, battery-suspension, launch-at-login, `always_on_camera`, and `hardware_keys_enabled` options; integer `brightness_key_step` and `volume_key_step` options (1–15 percentage points, default 5; additive in schema 1 with no ABI change, and out-of-range values are rejected by `update_settings`); the numeric `presence_min_brightness_percent` threshold (10–90 percentage points, default 20; additive in schema 1 with no ABI change, a missing key loads as 20, and out-of-range values are rejected by `update_settings`; see the camera lifecycle section); optional selected camera ID and read-only `camera_history` (see below); per-monitor normalized screensaver targets 0–100; and the brightness follow and protection settings described in the brightness follow section below. Defaults disable presence and screensaver protection, enable battery suspension, and disable launch at login, always-on capture, and hardware-key routing. Missing `always_on_camera` and `hardware_keys_enabled` default to false when loading existing settings; older binaries that reject unknown fields may not accept files saved by this version. Brightness and volume control commands use normalized values 0–100; device actions/readbacks use adapter-reported raw values and ranges.

The implemented camera snapshot has mode `disabled`, `suspended`, `light_only`, or `face`; capture mode is `stopped`, `single_frame`, `continuous_return`, or `continuous_monitoring`. It reports status, display status, generation, and `next_sample_in_ms` (null while disabled or suspended). This is policy output; the native adapter performs capture. The core ABI carries no frame data. `display_status` is presentation-only: `disabled`, `checking`, `ready`, `insufficient_light`, `unavailable`, `busy`, `permission`, `error`, `suspended_battery`, `suspended_power_unknown`, `suspended_no_monitor`, `suspended_display_sleep`, `suspended_low_brightness`, or fallback `unknown`. Each native shell maps these codes to its own localized text. It must not be used to override acquisition policy; consumers use `mode` and `capture_mode` for that.

The `camera` object also reports `presence`, the latest presence sample for UI. It is additive to schema 1, and older shells ignore it. It is `present` after a face was detected, `absent` after a sample found no face (face mode only), and `insufficient_light` while the camera is in the light-only mode or after an insufficient-light sample. It is `null` when presence is disabled or suspended, before the first sample after a start or resume, after any camera generation change (settings change, monitor or display-state change, power change), and after an unavailable, busy, permission or error status. `presence` does not change `display_status`, hysteresis (two misses before dimming) or dimming; it only reports the latest sample.

### Camera lifecycle ABI

The camera lifecycle is a separate opaque `TvCameraLifecycle` handle, not a `TvCore`, and it carries no snapshot and no frame data. It owns only the session timing for the mode the core reports: the acquisition and assessment deadlines, the exposure settle window, the assessment token and the once-per-sample dark settle. The core's `camera_mode` and `capture_mode` remain the only policy for whether the camera runs; the shell passes them in through a `reconcile` event. The shell supplies every time value, in seconds from its own monotonic clock, with each event, and the core reads no clock. Native timers, the clock and AVFoundation stay in the shell. This surface is additive to core ABI version 1: `tv_abi_version()` still returns `1`, `TV_CORE_ABI_VERSION` is unchanged, and the return codes are the ones listed above.

Each event is one UTF-8 JSON object with a `type` string and the fields below. Unknown types, unknown fields, duplicate keys at any depth, and missing or wrongly typed fields are rejected. `now` is a finite number of seconds, `generation` and `token` are non-negative integers, and `camera_id` must be present but may be `null`.

| Event | Fields | Result `value` |
|---|---|---|
| `reconcile` | `mode` and `capture_mode` (strings), `generation` (integer), `camera_id` (string or `null`), `always_on` (Boolean) | Boolean: `true` when the existing session must stop before the new state is used; the deadlines are then cancelled. |
| `begin_acquisition` | `now` | `null` |
| `acquired_frame` | none | `null` |
| `begin_light_assessment` | `now` | `null` |
| `begin_dark_settle` | `now` | Boolean: `true` when a dark settle started. It starts only in `face` mode and only once per sample. |
| `is_current_assessment` | `token` | Boolean: `true` when the token is the latest one and its deadline is still armed. |
| `frame` | `now` | String: `discard_warmup`, `assess_light` (in `light_only` mode), `assess_presence` (in any other mode), or `assessment_expired`. |
| `timed_out` | `now` | Boolean: `true` when the acquisition or assessment deadline has passed; passing a deadline cancels all deadlines. |
| `cancel_deadlines` | none | `null` |
| `accepts_callback` | `generation` | Boolean: `true` when the mode is `face` or `light_only` and the generation is the current one. |

`begin_acquisition` arms a 5 s deadline for the first usable frame, and `acquired_frame` clears it. `begin_light_assessment` and each started dark settle run a 5 s exposure settle followed by a 5 s deadline for the post-settle frame, and each one supersedes the earlier token. A `frame` event returns `assessment_expired` and cancels the deadlines when the post-settle deadline has passed, returns `discard_warmup` while the settle window is still running, and otherwise clears the settle and the deadlines and returns the assessment kind.

Every successful event returns one object, `{"value": <event-specific>, "state": {...}}`. The `state` object is flat, has the same keys for every event, and is read after the event is applied: `generation`, `camera_id`, `mode`, `capture_mode`, `always_on`, `eligible` (true in `face` or `light_only` mode), `acquisition_pending`, `should_keep_session` (true when `eligible` and `always_on` are set and the capture mode is `continuous_monitoring` or `continuous_return`), `settling_until`, `assessment_deadline` and `acquisition_deadline` (seconds on the shell's clock, or `null` when not armed), and `assessment_token` (which wraps after `u64::MAX`).

A rejected event returns `TV_INVALID_ARGUMENT` and leaves the lifecycle unchanged, because each event is fully validated before any field changes. Input larger than 64 KiB returns `TV_SIZE_LIMIT` before parsing, as `tv_submit` does. A null handle or output pointer, or null input with a non-zero length, returns `TV_INVALID_ARGUMENT`. A panic or a poisoned internal lock returns `TV_INTERNAL`. On `TV_OK` the caller owns the returned buffer and frees it with `tv_buffer_free`. Do not use a handle after `tv_camera_lifecycle_destroy`.

### Camera lifecycle and display sleep

`monitor_display_state` accepts `monitor_id`, the current monitor connection `generation`, and `state` (`unknown`, `awake`, or `asleep`). Invalid states and unknown monitor IDs are rejected; stale connection observations are ignored. Each monitor snapshot reports `display_state`. Unknown state is not classified as asleep. When all eligible external monitors (stable identity, supported brightness, known baseline) are asleep, camera mode is `suspended`, capture is `stopped`, and the next-sample delay is null. Any awake eligible monitor above the brightness threshold permits monitoring. Brightness zero does not indicate display sleep.

The brightness threshold `presence_min_brightness_percent` (default 20, range 10 to 90; the comparison is strict, so a monitor exactly at the threshold does not count) applies in the same way. Presence checking needs at least one eligible monitor that is awake and above the threshold. Otherwise the camera is `suspended` with capture `stopped` and a null next-sample delay. Display status is `suspended_low_brightness` when an eligible monitor exists, and `suspended_no_monitor` when none does. When several suspension reasons apply, battery states take precedence, then `suspended_display_sleep`, then `suspended_low_brightness`, then `suspended_no_monitor`. The existing no-eligible-monitor suspension is unchanged. The level compared is the monitor's `baseline` while the app owns its brightness or a protection reason is active (presence or screensaver dimming, or a pending restore write), and the observed level otherwise. A monitor with `presence_dimmed` set always counts as above, so its return check keeps running; an unknown observed level also counts as above. A monitor that falls to or below the threshold restarts its absence count. A connected monitor that rises above the threshold, or a threshold lowered below it, starts a resume as described below. Newly discovered monitors do not trigger a resume.

`always_on_camera` retains an eligible capture session in `continuous_monitoring` mode during face and light-only checks. It does not bypass presence enablement, battery suspension, display sleep, camera errors, no-monitor suspension, or shutdown. Inference follows the configured intervals, and frames between checks are discarded. Existing return checks continue to hold capture regardless of this setting. Presence observations received while disabled or suspended, or carrying an obsolete camera generation, are ignored.

Light-only retries keep `next_sample_in_ms` at 10000. The native adapter then holds capture for five seconds before assessing a fresh frame; completed assessments are approximately 15 seconds apart plus acquisition latency. Warmup uses a monotonic clock, performs no face inference, and does not count as absence. Initial acquisition and acquisition after settling each have a separate bounded timeout. Two usable-light results resume face checks.

Known-asleep targets receive no hardware writes. Sleep invalidates outstanding target actions; wake uses fresh brightness readback and existing ownership rules before protection or restoration writes. Screensaver protection remains an independent reason. Native detection follows OS-reported sleep and cannot guarantee detection of manual power-off when a monitor retains its logical connection.

### Camera selection history and resume sampling

`camera_history` is a read-only settings list of up to 8 camera IDs that the user selected, most recent first. Its first entry always equals `selected_camera_id`. A payload containing `camera_history` is rejected by `update_settings` with the error `camera_history is read-only`. Selecting a camera moves it to the front without duplicates, and selecting the camera already at the front changes nothing. `update_settings` bumps the camera generation only when the settings actually changed. Setting `selected_camera_id` to `null` (the system default camera) clears both the selection and the history. Files without the key load with an empty history, and a file that contains only `selected_camera_id` loads with that ID as a one-entry history. The key is additive to schema 1 and does not change ABI 1, but it carries the same compatibility caveat as the settings paragraph above: an older binary that rejects unknown fields may back up and reset a settings file that contains `camera_history`.

The native macOS shell resolves the effective camera when capture starts and when a camera connects or disconnects. It uses the first history entry that is currently connected and otherwise the system default camera. This fallback is a runtime decision and is never written back to settings, so a temporarily disconnected camera keeps its place at the front of the history and is used again after it reconnects. The settings picker shows the same effective camera.

A resume is triggered by an event rather than by a camera-mode transition: an eligible display going from asleep to awake, a monitor rising above the brightness threshold (or the threshold being lowered below it), power returning from battery to AC while battery suspension is on, or presence protection being re-enabled. On a trigger the core clears stale low-light state and resets `busy` or `unavailable` camera status to `unknown`; `permission` and `error` are not reset. In face mode, the snapshot reports `next_sample_in_ms` as 1000 until the first accepted camera report, instead of the configured interval. Later samples keep the configured interval, and `presence_sampling_interval_s` is unchanged. `light_only` keeps its existing cadence.

In single-frame face mode, when a sample's frame is assessed as `insufficient_light`, the native adapter keeps the camera session open for five seconds and discards frames while exposure settles. It assesses one frame at the end of that window and reports only that final result to the core. A usable frame is assessed normally. A dark final frame is reported as `insufficient_light`, so `low_light` and the `light_only` cycle start only when the settled sample is also dark. This applies to single-frame face-mode samples only; continuous capture modes keep reporting an insufficient-light frame immediately. In a dark room each single-frame face sample keeps the camera on about five seconds longer before it reports.

Physical camera timing, wake latency and energy effects of these changes are not verified; the hardware checks for them are pending.

### Built-in display brightness follow

The native shell reports the built-in display with `builtin_display`, a runtime command that is not persisted. It takes `available` (Boolean, required), `brightness` (number or `null`), and `calibrating` (Boolean, required). `brightness` is a finite percentage from 0 to 100, or `null` when the level is unknown; the core keeps no stale level, so follow targets are computed only from a known level. An unknown key, a wrong type, or a `brightness` outside 0–100 or not finite returns a command error and leaves the state unchanged. A successful command increments the revision.

Follow is *active* when `ambient_follow_enabled` and `available` are both true. It is *applying* when it is active, `brightness` is known and `calibrating` is false. While applying, each eligible monitor gets `follow_raw = round(normalized_to_raw(curve(brightness)))`, clamped to the monitor's raw range. A monitor is eligible when its brightness feature is supported, its identity is stable, and `ambient_curves` holds a complete curve for its ID, meaning at least two points. `curve` is piecewise linear through the knots (MacBook level, monitor percent): the origin (0, 0), then each saved point as (`builtin`, `monitor`) in ascending order, where each `builtin` is the MacBook level at which the monitor looks equal to the monitor at `monitor`% of its range. Below the first point it interpolates from the origin. Above the last point the monitor stays at the last point's `monitor` percent, so a last point below 100 caps the monitor there; add a 100% point to let it reach full brightness. Inputs below 0 or above 100 are clamped.

When follow is not applying, `follow_raw` is cleared and the monitor keeps its current baseline; follow issues no writes. When `follow_raw` is set, the baseline becomes `follow_raw` and `manual_override` is unchanged. A write is queued only when the monitor is awake, not dimmed by a protection reason, not already within one raw unit of `follow_raw` (observed or desired), and the bounded hardware queue accepts it. Presence or screensaver dimming keeps the dimmed level and updates only the baseline; when protection ends, the existing restore writes the baseline, which is the mapped value.

`set_brightness` is unchanged and still sets `manual_override` and the baseline. The calibration uses it to set the monitor to each point; the calibration slider for the MacBook screen is written by the native shell, not through this command. While calibration is in use the shell reports `calibrating: true`, which pauses follow as described above. `monitor_discovered` also recomputes `follow_raw` for the new monitor.

The top-level `ambient` snapshot object reports `enabled` (the `ambient_follow_enabled` setting), `available`, `active`, `calibrating`, and `builtin_percent`:

```json
"ambient": { "enabled": true, "available": true, "active": true, "calibrating": false, "builtin_percent": 44 }
```

`active` equals `enabled && available`, and `builtin_percent` is `null` when unknown. Each connected monitor reports `follow` and `protected`. `follow` describes eligibility whether or not follow is enabled: `eligible` (supported, stable identity, complete curve), `uncalibrated` (supported and stable, curve missing or incomplete), `unsupported` (no DDC brightness), or `unstable` (ambiguous identity; writes are blocked). The popup hides the brightness slider when `ambient.active` is true and `follow` is `eligible`. `protected` is `false` when the monitor ID is in `protection_disabled`.

Settings add `ambient_follow_enabled` (Boolean, default `false`), `ambient_curves` (a map from monitor ID to an AmbientCurve, default `{}`), and `protection_disabled` (default `[]`). An AmbientCurve has `points`, a list of objects `{ monitor, builtin }`. `monitor` is a finite number greater than 0 and at most 100 (the monitor's brightness percent), and must be strictly ascending across the list, so there are no duplicates. `builtin` is a finite number from 0 to 100 (the MacBook level percent at which the monitor looks equal to the monitor at `monitor`%), and must be non-decreasing across the list. A curve holds at most 32 points and is complete with at least 2. Legacy objects with the fixed keys `p1`, `p5`, `p10`, `p15`, `p20`, `p25`, `p50`, `p75` and `p100` are accepted on input and read as points `{ monitor: N, builtin: pN }`, skipping null keys; they are never emitted. The map holds at most 128 entries, each key 1–512 bytes. `protection_disabled` is stored as a sorted, unique set of monitor IDs, at most 128 entries, each 1–512 bytes. An ID's absence from `protection_disabled` means protection is on for that monitor. Entries for monitors that are not connected are kept. A settings update larger than the 64 KiB `tv_submit` limit is rejected.

`update_settings` replaces `ambient_curves` and `protection_disabled` as a whole when present, as it does `screensaver_targets`. Omitted keys are untouched, and unknown keys remain an error. After a change to `ambient_follow_enabled`, `ambient_curves` or `protection_disabled`, follow targets and protection reasons are recomputed. A monitor newly added to `protection_disabled` has its presence dim, miss counter and screensaver dim cleared, and those are restored through the existing restore path. Removing an ID restores protection with no immediate dim; the next evaluation applies normally.

As with the earlier additive schema-1 fields, a build without these fields treats a `settings.json` that contains them as corrupt, because it rejects unknown fields. It renames the file to `settings.json.corrupt` and starts from defaults. Older files load with defaults in newer builds. The `points` curve form follows the same rule for builds from before this change.

Built-in display writes through DisplayServices and the automatic brightness setter are unverified on hardware. These interfaces do not establish them.

### Local inference C ABI

Header: [`include/telaviva_inference.h`](include/telaviva_inference.h). `TvInferenceResult` contains integer `status` and floating-point `confidence`. `tv_inference_create` and `tv_inference_create_with_provider` load absolute app-bundled runtime/model paths. Provider values currently implemented are CPU (`0`), CoreML candidate (`1`, macOS builds only) and DirectML candidate (`2`, see the [windows-version](#windows-version) section); candidate selection may fall back to CPU and does not promise that every operation runs on CoreML or DirectML.

`tv_inference_detect_bgra` accepts caller-owned top-down BGRA8 bytes with length, width, height, and stride, and returns status: absent `0`, present `1`, or insufficient light `2`; negative values indicate invalid argument `-1`, runtime `-2`, model `-3`, or panic `-4`. It never returns `3` (unavailable), which only the create functions return. `tv_inference_assess_light_bgra` performs quality-only light assessment, returning `0` for sufficient light, `2` for insufficient light, or a negative error. `tv_inference_coreml_available` reports runtime capability, while `tv_inference_selected_provider` reports the selected provider. Call `tv_inference_destroy` to release the handle. Frame data is processed synchronously and is not retained or logged.

Hardware camera behavior, DDC writes/restoration, and a measured DirectML energy benefit are not established by these interfaces or their source-level tests. NPU providers are not implemented.

## External monitor hardware keys

Settings schema 1 adds `hardware_keys_enabled` (Boolean, default false) and the integer `brightness_key_step` and `volume_key_step` settings (1–15 percentage points, default 5), which set how far each brightness or volume key press moves the monitor. Older files load with routing disabled and the default steps; older binaries that reject unknown fields may reject newly saved settings. Enabling keyboard routing does not enable presence protection or camera access. Shift+Option moves a fixed 1 point.

`set_brightness` and `set_volume` optionally accept `connection_generation`, an unsigned integer matching the target monitor's current connection generation. Missing values preserve the existing manual-control API. A malformed or stale supplied generation is rejected before any action, desired value, baseline, or revision changes. Manual state changes only after the bounded hardware queue accepts the action. This is additive to core ABI version 1.

The native macOS shell reserves brightness keyboard intent only for an eligible external monitor under the cursor, reserves volume keyboard intent only for the external monitor that is the current default sound output, and submits both through these manual-control commands. The mute key reserves a volume intent of 0 or the remembered level for the sound-output monitor. The initial down event selects the monitor; repeats retain that target through release. Supported brightness and speaker volume controls are independent. Values use normalized 0–100; retained intent accumulates repeat steps until readback catches up. Sleeping, disconnected, ambiguous, or unsupported targets do not receive keyboard writes. Brightness keys for a monitor that ambient follow controls (`follow` is `eligible` while `ambient.active` is true) are passed through unchanged instead. Input permission/status presentation and the event tap are native-shell concerns, outside the Rust snapshot contract. Brightness keys sent as plain key-down and key-up events with key codes 144 and 145 are recognized too; the event tap acts only on those key codes and never reads typed characters.

## windows-version

### Local inference C ABI additions

Header: [`include/telaviva_inference.h`](include/telaviva_inference.h). These additions are backward compatible: existing functions and values are unchanged, and the core ABI version (`TV_CORE_ABI_VERSION`) stays `1`.

Provider values:

| Constant | Value | Contract |
|---|---|---|
| `TV_INFERENCE_PROVIDER_CPU` | `0` | CPU. Always available. |
| `TV_INFERENCE_PROVIDER_COREML_CANDIDATE` | `1` | CoreML candidate. Only macOS builds register it; other builds fall back to CPU. |
| `TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE` | `2` | DirectML candidate. An unavailable or failing DirectML registration falls back to CPU. |
| `3` | reserved | Reserved for a future NPU provider. The create functions reject it with `-1`. The value `3` is also the create-function unavailable status and is not a provider value. |

```c
int32_t tv_inference_create_with_provider_device(const char *runtime_abs_path,
                                                 const char *model_abs_path,
                                                 int32_t provider,
                                                 uint32_t device_index,
                                                 TvInferenceHandle **out_handle);

int32_t tv_inference_directml_available(void);
```

`tv_inference_create_with_provider_device` is `tv_inference_create_with_provider` with a DirectML adapter index added. It has the same paths, return codes and handle rules as `tv_inference_create`. `device_index` is an index into the DXGI adapter enumeration order (`IDXGIFactory1::EnumAdapters1`) as seen by ONNX Runtime. It applies only to provider `2` and is ignored for providers `0` and `1`. Index `0` gives the same session as `tv_inference_create_with_provider`. An index that ONNX Runtime cannot register falls back to CPU: the return value is still `0` and `tv_inference_selected_provider` reports `0`. The index-to-adapter mapping is not yet confirmed against the DXGI order; the Windows measurement notes infer it from latency.

`tv_inference_directml_available` returns `1` only when all of these hold: the build was compiled with the `directml` cargo feature (off by default), the platform supports DirectML, a successful create call has initialized the runtime, and the loaded runtime lists the DirectML provider. In every other case it returns `0`, including builds without the `directml` feature and calls made before the runtime is initialized. It never returns an error and does not need a created detector.

`tv_inference_selected_provider` returns `2` when the session was built with DirectML.

The default Windows runtime is the ONNX Runtime CPU build. A DirectML-enabled runtime is a reserved variant that is not shipped by default. On the Windows test machine, a DirectML session returned the same status and confidence as CPU on the test fixture. Energy was not measured, so no energy benefit is claimed (see [the Windows measurements](specs/archived/windows-version/validation/measurements.md)).
