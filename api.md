# Public API

This page records the consumer-facing native interfaces that exist in the repository. The Rust core C ABI is implemented; the inference C ABI is also implemented. The JSON snapshot schema is an implementation contract consumed by the native macOS shell, not a claim that the planned Windows consumer exists.

## external-monitor-control-and-oled-presence-protection

### Native shell core ABI

Header: [`include/telaviva_core.h`](include/telaviva_core.h). ABI version: `1` (`TV_CORE_ABI_VERSION`). The interface uses opaque `TvCore` handles, UTF-8 JSON inputs, a revision-only snapshot callback, and Rust-owned snapshot buffers. C++ callers may include the header under `extern "C"`.

| Function | Contract |
|---|---|
| `tv_abi_version()` | Returns the supported ABI version. |
| `tv_create(version, options, options_len, callback, userdata, out_core)` | Creates a core when the requested version matches. Optional UTF-8 JSON options accept `config_path` and `os_locale`; `NULL`/zero length selects defaults. |
| `tv_submit(core, json, json_len)` | Submits a UTF-8 JSON command, with a maximum input size of 64 KiB. Commands after shutdown return `TV_CLOSED`, except hardware results needed to complete pending work. |
| `tv_snapshot(core, out_buffer)` | Copies the current revisioned UTF-8 JSON snapshot into a Rust-owned buffer, capped at 8 MiB. |
| `tv_shutdown(core)` | Stops new callbacks/commands and emits best-effort ownership-checked restore actions. |
| `tv_destroy(core)` | Destroys a handle after the host handles shutdown actions; callers must not race it with other calls. |
| `tv_buffer_free(buffer)` | Frees and clears a buffer returned by `tv_snapshot`. |

Return codes are `TV_OK=0`, `TV_INVALID_ARGUMENT=1`, `TV_ABI_MISMATCH=2`, `TV_CLOSED=3`, `TV_INTERNAL=4`, and `TV_SIZE_LIMIT=5`. The callback receives a revision and caller-owned `userdata`; it does not transfer a snapshot. Core buffers and pointers must follow the lifetimes stated in the header. Do not destroy a core concurrently with another call.

Implemented command envelope types are `monitor_discovered`, `monitor_disconnected`, `monitor_observed`, `monitor_display_state`, `presence_observation`, `camera_status`, `power`, `screensaver`, `set_brightness`, `set_volume`, `update_settings`, `hardware_result`, and `shutdown`. Unknown fields and duplicate JSON keys are rejected. The snapshot currently includes `revision`, `settings`, `messages`, `monitors`, `pending_actions`, `camera`, `power`, `screensaver`, and top-level `status`. Pending hardware actions are dispatched by the native host; automatic ownership checks use `read_brightness` and require the raw readback to be returned as a `hardware_result`.

Each monitor snapshot reports `id`, `name`, `identity_status`, `connection_path`, `generation`, `display_state`, `presence_dimmed`, `brightness`, and `volume`. Each brightness/volume feature reports `supported`, `normalized`, `raw`, `min`, `max`, `observed`, `desired`, `status`, `owned`, and `reasons`. Each pending action reports `id`, `monitor_id`, `generation`, `kind`, `value`, and `target`. Discovery identities are `stable` or `ambiguous`; camera status inputs are `unknown`, `ready`, `unavailable`, `busy`, `permission`, or `error`. Presence observations currently accept `present`, `absent`, `insufficient_light`, `unavailable`, and `usable_light`; observation freshness and camera generation gate whether an event is applied.

Settings schema version `1` uses locale `auto`, `en-US`, or `pt-BR`; presence sampling interval 5–300 seconds (default 10); return sampling interval 0.25–5 seconds (default 1); Boolean presence, screensaver, battery-suspension, launch-at-login, `always_on_camera`, and `hardware_keys_enabled` options; integer `brightness_key_step` and `volume_key_step` options (1–15 percentage points, default 5; additive in schema 1 with no ABI change, and out-of-range values are rejected by `update_settings`); optional selected camera ID; and per-monitor normalized screensaver targets 0–100. Defaults disable presence and screensaver protection, enable battery suspension, and disable launch at login, always-on capture, and hardware-key routing. Missing `always_on_camera` and `hardware_keys_enabled` default to false when loading existing settings; older binaries that reject unknown fields may not accept files saved by this version. Brightness and volume control commands use normalized values 0–100; device actions/readbacks use adapter-reported raw values and ranges.

The implemented camera snapshot has mode `disabled`, `suspended`, `light_only`, or `face`; capture mode is `stopped`, `single_frame`, `continuous_return`, or `continuous_monitoring`. It reports status, display status, generation, and `next_sample_in_ms` (null while disabled or suspended). This is policy output; the native adapter performs capture. The core ABI carries no frame data. `display_status` is presentation-only: `disabled`, `checking`, `ready`, `insufficient_light`, `unavailable`, `busy`, `permission`, `error`, `suspended_battery`, `suspended_power_unknown`, `suspended_no_monitor`, `suspended_display_sleep`, or fallback `unknown`. It must not be used to override acquisition policy; consumers use `mode` and `capture_mode` for that.

### Camera lifecycle and display sleep

`monitor_display_state` accepts `monitor_id`, the current monitor connection `generation`, and `state` (`unknown`, `awake`, or `asleep`). Invalid states and unknown monitor IDs are rejected; stale connection observations are ignored. Each monitor snapshot reports `display_state`. Unknown state is not classified as asleep. When all eligible external monitors (stable identity, supported brightness, known baseline) are asleep, camera mode is `suspended`, capture is `stopped`, and the next-sample delay is null. Any awake eligible monitor permits monitoring. Brightness zero does not indicate display sleep.

`always_on_camera` retains an eligible capture session in `continuous_monitoring` mode during face and light-only checks. It does not bypass presence enablement, battery suspension, display sleep, camera errors, no-monitor suspension, or shutdown. Inference follows the configured intervals, and frames between checks are discarded. Existing return checks continue to hold capture regardless of this setting. Presence observations received while disabled or suspended, or carrying an obsolete camera generation, are ignored.

Light-only retries keep `next_sample_in_ms` at 10000. The native adapter then holds capture for five seconds before assessing a fresh frame; completed assessments are approximately 15 seconds apart plus acquisition latency. Warmup uses a monotonic clock, performs no face inference, and does not count as absence. Initial acquisition and acquisition after settling each have a separate bounded timeout. Two usable-light results resume face checks.

Known-asleep targets receive no hardware writes. Sleep invalidates outstanding target actions; wake uses fresh brightness readback and existing ownership rules before protection or restoration writes. Screensaver protection remains an independent reason. Native detection follows OS-reported sleep and cannot guarantee detection of manual power-off when a monitor retains its logical connection.

### Local inference C ABI

Header: [`include/telaviva_inference.h`](include/telaviva_inference.h). `TvInferenceResult` contains integer `status` and floating-point `confidence`. `tv_inference_create` and `tv_inference_create_with_provider` load absolute app-bundled runtime/model paths. Provider values currently implemented are CPU (`0`) and CoreML candidate (`1`); candidate selection may fall back to CPU and does not promise that every operation runs on CoreML.

`tv_inference_detect_bgra` accepts caller-owned top-down BGRA8 bytes with length, width, height, and stride, and returns status: absent `0`, present `1`, insufficient light `2`, unavailable `3`; negative values indicate invalid argument `-1`, runtime `-2`, model `-3`, or panic `-4`. `tv_inference_assess_light_bgra` performs quality-only light assessment, returning `0` for sufficient light, `2` for insufficient light, or a negative error. `tv_inference_coreml_available` reports runtime capability, while `tv_inference_selected_provider` reports the selected provider. Call `tv_inference_destroy` to release the handle. Frame data is processed synchronously and is not retained or logged.

Windows OpenVINO integration, hardware camera behavior, DDC writes/restoration, and measured acceleration benefit are not established by these interfaces or their source-level tests.

## External monitor hardware keys

Settings schema 1 adds `hardware_keys_enabled` (Boolean, default false) and the integer `brightness_key_step` and `volume_key_step` settings (1–15 percentage points, default 5), which set how far each brightness or volume key press moves the monitor. Older files load with routing disabled and the default steps; older binaries that reject unknown fields may reject newly saved settings. Enabling keyboard routing does not enable presence protection or camera access. Shift+Option moves a fixed 1 point.

`set_brightness` and `set_volume` optionally accept `connection_generation`, an unsigned integer matching the target monitor's current connection generation. Missing values preserve the existing manual-control API. A malformed or stale supplied generation is rejected before any action, desired value, baseline, or revision changes. Manual state changes only after the bounded hardware queue accepts the action. This is additive to core ABI version 1.

The native macOS shell reserves brightness keyboard intent only for an eligible external monitor under the cursor, reserves volume keyboard intent only for the external monitor that is the current default sound output, and submits both through these manual-control commands. The mute key reserves a volume intent of 0 or the remembered level for the sound-output monitor. The initial down event selects the monitor; repeats retain that target through release. Supported brightness and speaker volume controls are independent. Values use normalized 0–100; retained intent accumulates repeat steps until readback catches up. Sleeping, disconnected, ambiguous, or unsupported targets do not receive keyboard writes. Input permission/status presentation and the event tap are native-shell concerns, outside the Rust snapshot contract. Brightness keys sent as plain key-down and key-up events with key codes 144 and 145 are recognized too; the event tap acts only on those key codes and never reads typed characters.
