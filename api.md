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
| `tv_diagnostics_drain(core, out_buffer)` | Returns and clears the pending diagnostic lines as a UTF-8 JSON array of strings, copied into a Rust-owned buffer capped at 8 MiB; an empty ring returns `[]`. Each line names a monitor only by its hashed `id-xxxxxxxx`. Free the buffer with `tv_buffer_free`. Additive to ABI version 1. |
| `tv_shutdown(core)` | Stops new callbacks/commands and emits best-effort ownership-checked restore actions. Parked dims are discarded, not restored. |
| `tv_destroy(core)` | Destroys a handle after the host handles shutdown actions; callers must not race it with other calls. |
| `tv_buffer_free(buffer)` | Frees and clears a buffer returned by `tv_snapshot`, `tv_camera_lifecycle_step`, `tv_controls_step`, `tv_core_version`, `tv_update_evaluate` or `tv_diagnostics_drain`. |
| `tv_camera_lifecycle_create(out)` | Creates an inactive `TvCameraLifecycle` handle (mode `disabled`, no deadlines). Additive to ABI version 1. |
| `tv_camera_lifecycle_step(handle, json, len, out_buffer)` | Applies one UTF-8 JSON lifecycle event (maximum 64 KiB) and copies its result into a Rust-owned buffer. Additive to ABI version 1. |
| `tv_camera_lifecycle_destroy(handle)` | Destroys a lifecycle handle; callers must not race it with other calls on the same handle. Additive to ABI version 1. |
| `tv_controls_create(out)` | Creates an empty `TvControls` handle. Additive to ABI version 1. |
| `tv_controls_step(handle, json, len, out_buffer)` | Applies one UTF-8 JSON controls event (maximum 64 KiB) and copies its result into a Rust-owned buffer. Additive to ABI version 1. |
| `tv_controls_destroy(handle)` | Destroys a controls handle; callers must not race it with other calls on the same handle. Additive to ABI version 1. |
| `tv_core_version(out)` | Writes the core version as UTF-8 without a terminator to a Rust-owned buffer; free it with `tv_buffer_free`. Additive to ABI version 1. |
| `tv_update_evaluate(installed, installed_len, http_status, body, body_len, out)` | Compares the installed version with a GitHub `releases/latest` answer and writes an UpdateOutcome JSON to a Rust-owned buffer. Pure: no network and no state. Additive to ABI version 1. |

Return codes are `TV_OK=0`, `TV_INVALID_ARGUMENT=1`, `TV_ABI_MISMATCH=2`, `TV_CLOSED=3`, `TV_INTERNAL=4`, and `TV_SIZE_LIMIT=5`. The callback receives a revision and caller-owned `userdata`; it does not transfer a snapshot. Core buffers and pointers must follow the lifetimes stated in the header. Do not destroy a core concurrently with another call.

Implemented command envelope types are `monitor_discovered`, `monitor_disconnected`, `monitor_observed`, `monitor_display_state`, `presence_observation`, `keep_awake_session`, `camera_status`, `builtin_display`, `power`, `screensaver`, `set_brightness`, `set_volume`, `update_settings`, `hardware_result`, `lock_result`, and `shutdown`. Unknown fields and duplicate JSON keys are rejected. The snapshot currently includes `revision`, `settings`, `monitors`, `pending_actions`, `camera`, `keep_awake`, `power`, `screensaver`, `ambient`, `lock`, top-level `status`, `builtin_protection`, and `monitored_display`. The snapshot no longer carries a `messages` field: the core returns status codes and no translated text, and each native shell owns its UI strings (see [ADR 0015](adr/0015-native-shells-own-ui-strings.md)). Pending hardware actions are dispatched by the native host; automatic ownership checks use `read_brightness` and require the raw readback to be returned as a `hardware_result`.

Each monitor snapshot reports `id`, `name`, `identity_status`, `connection_path`, `generation`, `display_state`, `presence_dimmed`, `brightness`, `volume`, `follow`, `protected`, and `primary`. Each brightness/volume feature reports `supported`, `normalized`, `raw`, `min`, `max`, `observed`, `desired`, `status`, `owned`, and `reasons`. Each pending action reports `id`, `monitor_id`, `generation`, `kind`, `value`, and `target`. Discovery identities are `stable` or `ambiguous`; camera status inputs are `unknown`, `ready`, `unavailable`, `busy`, `permission`, or `error`. Presence observations currently accept `present`, `absent`, `insufficient_light`, `unavailable`, and `usable_light`; observation freshness and camera generation gate whether an event is applied.

Settings schema version `1` uses locale `auto`, `en-US`, or `pt-BR` (the core stores this setting and each native shell reads it to choose its own UI strings; the core translates nothing); presence sampling interval 5–300 seconds (default 10); return sampling interval 0.25–5 seconds (default 0.5; it was 1 before 2026-10-29, and stored values are kept); Boolean presence, screensaver, battery-suspension, launch-at-login, `always_on_camera`, and `hardware_keys_enabled` options; integer `brightness_key_step` and `volume_key_step` options (1–15 percentage points, default 5; additive in schema 1 with no ABI change, and out-of-range values are rejected by `update_settings`); the numeric `presence_min_brightness_percent` threshold (1–90 percentage points, default 20; a stored value in [0, 1) loads as 1; additive in schema 1 with no ABI change, a missing key loads as 20, and out-of-range values are rejected by `update_settings`; see the camera lifecycle section); optional selected camera ID and read-only `camera_history` (see below); per-monitor normalized screensaver targets 0–100, applied as the 1–50 percent protection level (see [Setting `screensaver_targets` as the protection level](#setting-screensaver_targets-as-the-protection-level)); and the brightness follow and protection settings described in the brightness follow section below. Defaults disable presence and screensaver protection, enable battery suspension, disable launch at login and hardware-key routing, and enable always-on capture. Missing `always_on_camera` defaults to true and missing `hardware_keys_enabled` defaults to false when loading existing settings (a saved value is kept); older binaries that reject unknown fields may not accept files saved by this version. Brightness and volume control commands use normalized values 0–100; device actions/readbacks use adapter-reported raw values and ranges. `set_brightness` and `set_volume` map the value to the device raw range, round it to a whole raw unit and clamp it, so desired and baseline are whole units.

The implemented camera snapshot has mode `disabled`, `suspended`, `light_only`, or `face`; capture mode is `stopped`, `single_frame`, `continuous_return`, or `continuous_monitoring`. It reports status, display status, generation, `next_sample_in_ms` (null while disabled or suspended), and `retry_in_ms`. `retry_in_ms` is a number of milliseconds: 30000 after the first consecutive `busy` or `unavailable` report, 60000 after the second, and 300000 from the third on. It is null unless the status is `busy` or `unavailable`, presence protection is on and the core is open. While it is not null, the native shell arms one timer for the current camera generation; when the timer elapses, the shell sends `camera_status` with status `unknown` for that generation, which starts a new probe (see [Camera selection history and resume sampling](#camera-selection-history-and-resume-sampling)). The counter resets on an accepted `ready` sample and on each resume trigger. The field is additive to ABI version 1. This is policy output; the native adapter performs capture. The core ABI carries no frame data. `display_status` is presentation-only: `disabled`, `checking`, `ready`, `insufficient_light`, `unavailable`, `busy`, `permission`, `error`, `suspended_battery`, `suspended_power_unknown`, `suspended_no_monitor`, `suspended_display_sleep`, `suspended_low_brightness`, or fallback `unknown`. Each native shell maps these codes to its own localized text. It must not be used to override acquisition policy; consumers use `mode` and `capture_mode` for that.

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

### Controls ABI

The controls machine is a separate opaque `TvControls` handle, not a `TvCore`. It owns the manual-control queue for slider and hardware-key changes: it decides which command the shell submits to the core and which DDC primitive the shell runs next, and it reports each outcome back through events. The shell runs the DDC primitives and every `tv_submit` call. This surface is additive to core ABI version 1 and changes no existing function.

Each event is one UTF-8 JSON object with a string `type` and a number `now`. Unknown fields, duplicate keys, non-finite numbers and trailing data are rejected, and a rejected event leaves the state unchanged.

- `now`: monotonic seconds from the shell clock (`CMClockGetHostTimeClock` on macOS, `steady_clock` on Windows). It must be finite. A `now` smaller than the previous one is clamped to the previous one, so a mis-ordered event never moves a deadline backward.

| `type` | Fields | When the shell sends it |
|---|---|---|
| `configure` | `brightness_step` and `volume_step` (required finite numbers, each clamped to whole points 1 to 15; a non-finite value is rejected), `keys_enabled` (required Boolean; `false` also clears the volume owner, and a `volume_owner` in the same event is then ignored), `volume_owner` (optional string or null; absent leaves it unchanged, null clears it) | At start and when the key step setting or key routing changes |
| `observe` | `revision` (u64), `monitors` (array), `pending_actions` (array) | After every snapshot publish, in snapshot order |
| `monitor_profile` | `monitor_id`, `manufacturer` (EDID code, may be empty) | After discovery of a DDC-capable monitor |
| `monitor_removed` | `monitor_id` | When the shell drops a monitor |
| `display_power` | `on` (Boolean) | When the session reports displays off or on |
| `slider` | `monitor_id`, `feature` (`brightness` or `volume`), `value` (number) | On every slider value change |
| `key` | `key`, `phase` (`down` or `up`), `repeat` (Boolean), `fine` (Boolean), `owner` (object or null) | On every decoded volume, mute or brightness key event |
| `submitted` | `token` (u64), `accepted` (Boolean) | After the shell passed a `command_json` or `result_json` to `tv_submit` |
| `attempt_done` | `token` (u64), `ok` (Boolean), `error` (`none`, `unsupported`, `io`, `disconnected`; present when `ok` is false), `value` (number or null; raw readback for reads) | After the shell ran one DDC primitive |
| `tick` | none | When the shell timer for `next_deadline` fires |
| `query_draft` | `monitor_id`, `feature` | When the UI needs the value to draw |
| `shutdown` | none | On application exit |

### `observe` and `key` details

- `observe` carries the snapshot's `monitors` and `pending_actions`. In each channel, `observed` is the normalized 0 to 100 percentage (the snapshot's `normalized` field), and `min` and `max` are the raw device range. A snapshot older than the last applied revision is ignored; an equal revision is accepted. Each array is capped at 64 monitors and 128 pending actions; more is rejected. `kind` is one of `set_brightness`, `set_volume` or `read_brightness`; an action of another kind fails with `unsupported` and the failure is reported. A pending action's `value` is the raw device target, which the machine rounds.
- A channel is eligible when it is supported, its observed value is non-null, the display is not `asleep`, the identity is `stable`, and `max` is greater than `min`.
- `key` is `volume_up`, `volume_down`, `mute`, `brightness_up` or `brightness_down`; the matching feature is derived from the key. `owner` is the target the shell resolved. A `null` owner, or one that does not resolve to a supported, awake target with a known level from 0 to 100, means there is no eligible target, so the machine answers `{"consume": false}` for the whole sequence. A change of the `volume_owner` set by `configure` clears all remembered mute levels; a different `owner` on a key event does not.
- The shell owns the swallow decision on Windows; macOS uses the returned `consume`.

### Controls result

Every successful event returns one object:

```json
{
  "value": null,
  "effects": [ ],
  "next_deadline": null
}
```

- `next_deadline` is the absolute monotonic time, in seconds, of the next `tick` the machine needs, or `null`. The shell keeps exactly one timer armed for it and re-arms it from every result. A `tick` before the deadline does nothing harmful.
- `value` depends on the event:

| Event | `value` |
|---|---|
| `slider` | `{ "draft": <number>, "status": "pending" }`, or `null` when the target was ignored (unknown monitor, ineligible, asleep, draft cap, non-finite) |
| `key` | `{ "consume": <Boolean> }` |
| `query_draft` | `{ "has_draft": <Boolean>, "draft": <number>, "status": "ready", "pending" or "failed" }` |
| any other event | `null` |

- For an idle control, `draft` is the latest observed value, so the UI can always draw `draft`.

Effects are an ordered array. The shell executes them in order, outside any lock it holds for its own state.

| `type` | Fields | Shell action |
|---|---|---|
| `submit_command` | `token`, `command_json` | Pass `command_json` verbatim to `tv_submit`; then send `submitted {token, accepted}`, where `accepted` is true when `tv_submit` returned `TV_OK`. |
| `run_attempt` | `token`, `monitor_id`, `op` (`read` or `write`), `code` (VCP code: 16 brightness, 98 volume), `value` (raw; present for `write`) | Run exactly one DDC primitive on the DDC worker; then send `attempt_done {token, ...}`. The primitive's internal framing (macOS write-twice and checksum seed fallback, Windows single call) is a shell detail and counts as one attempt. |
| `submit_result` | `token`, `result_json` | Pass `result_json` verbatim to `tv_submit`; then send `submitted {token, accepted}`. |
| `draft_changed` | `monitor_id`, `feature` | Repaint the control by calling `query_draft`. |

- Tokens are unique per effect and are never reused during a run. An `attempt_done` or `submitted` with an unknown or already-finished token is accepted and ignored.
- `command_json` is exactly one of `{"type":"set_brightness",...}` or `{"type":"set_volume",...}`, with the fields the core already accepts, including `connection_generation`. `result_json` is a `hardware_result` the core already accepts: `{"type":"hardware_result","action_id":N,"generation":G,"ok":true,"value":RAW}`, or the same with `"ok":false` and `"error"` set to `io`, `unsupported`, `disconnected` or `ambiguous`. `value` is a raw readback, never a percentage.
- `command_json` and `result_json` are passed to `tv_submit` exactly as the machine produced them. The outcome is reported back to the machine with `submitted`, so the machine can resubmit or drop a result.

### Controls errors and threading

| Condition | Result |
|---|---|
| Malformed JSON, unknown `type`, unknown field, duplicate key, non-finite number, missing or mistyped field | `TV_INVALID_ARGUMENT`, state unchanged |
| Event over 64 KiB | `TV_SIZE_LIMIT`, state unchanged |
| Null handle or null `out` | `TV_INVALID_ARGUMENT` |
| Panic inside a step, or a poisoned lock | `TV_INTERNAL`; the shell should stop using the handle. The Windows wrapper instead returns an empty reply on any non-OK status and ignores that event |
| Unknown `monitor_id` on `slider` or `monitor_removed` | Accepted, ignored (`value` null) |
| `key` whose `owner` is null or unknown | Accepted; `value` is `{"consume": false}` |
| `submitted` or `attempt_done` with an unknown token | Accepted, ignored |
| Event after `shutdown` | Accepted, ignored |

- Failures write nothing to `out`. On `TV_OK` the caller owns the returned buffer and frees it with `tv_buffer_free`.
- The handle holds one `Mutex`, so calls are serialized. The lock is a leaf lock: it is never held across I/O, and callers must not hold another lock that a step could wait on. The shell must never take its core lock for a controls call.
- `tv_controls_destroy` must not race other calls on the same handle, and the handle must not be used after it.
- For the rationale behind this protocol, see [ADR 0029](adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md).

### Update check ABI

The core evaluates an update check and nothing else. The native shell performs the HTTPS request to the GitHub releases API, off the UI thread, and passes the installed version, the HTTP status and the response body to `tv_update_evaluate`. Both functions are pure: they perform no network access and keep no state, and nothing is written to disk. `tv_core_version` returns the installed core version. These functions are additive to ABI version 1: `tv_abi_version()` still returns `1` and `TV_CORE_ABI_VERSION` stays `1`.

`tv_core_version(out)` writes the core version as UTF-8 without a terminator. `out` is written only on `TV_OK`.

`tv_update_evaluate(installed, installed_len, http_status, body, body_len, out)` takes these parameters:

- `installed` and `installed_len`: the installed version as UTF-8, in `MAJOR.MINOR.PATCH` form (optionally `MAJOR.MINOR.PATCH.REVISION`) with an optional `-prerelease`. Empty input, invalid UTF-8 or a malformed version is rejected. The length may not exceed 64 bytes.
- `http_status`: the HTTP status code of the GitHub response.
- `body` and `body_len`: the response body. The pointer may be null only when `body_len` is `0`. The body may not exceed 1 MiB.
- `out`: receives the UpdateOutcome JSON.

Return codes:

- `TV_OK`: `out` is written, including for non-success outcomes such as `rate_limited`.
- `TV_INVALID_ARGUMENT`: null `out`, a null pointer with a non-zero length, or an empty, malformed or non-UTF-8 `installed`.
- `TV_SIZE_LIMIT`: `body_len` greater than 1 MiB, or `installed_len` greater than 64 bytes.
- `TV_INTERNAL`: a panic was caught.

`out` is written only on `TV_OK`. The caller owns the written buffer and frees it with `tv_buffer_free`.

UpdateOutcome is UTF-8 JSON:

```json
{"status":"update_available","remote_version":"0.2.0","release_url":"https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0"}
```

| `status` | Meaning | Fields |
|---|---|---|
| `update_available` | The remote version is newer than the installed version. | `remote_version`, `release_url` |
| `up_to_date` | The remote version is the same as or older than the installed version. | `remote_version` |
| `no_release` | HTTP 404, or the release is a draft or pre-release. | none |
| `rate_limited` | HTTP 403 or 429. | none |
| `http_error` | Any other non-200 status. | none |
| `invalid_response` | HTTP 200 with an unusable body. | none |

On a 200 response the core reads `tag_name`, `draft` and `prerelease`; all other fields are ignored. `tag_name` must match `^[vV]?[0-9A-Za-z.+-]+$` after trimming, and `remote_version` is the tag without its leading `v` or `V`.

`release_url` is always built by the core as `https://github.com/ksfreitas/telaviva-oled/releases/tag/<tag_name>`. The `html_url` sent by the server is never used.

Version comparison rules:

- A version is `MAJOR.MINOR.PATCH`, optionally followed by a fourth numeric revision for compatibility with existing release tags such as `v0.1.3.1`. Each component is an unsigned decimal number without leading zeros. An optional `-prerelease` uses dot-separated `[0-9A-Za-z-]+` identifiers; optional `+build` metadata is ignored. A single leading `v` or `V` is accepted for tags only.
- Two or more than four numeric parts, empty parts, signs or spaces are malformed.
- Versions compare numerically in major/minor/patch/revision order; a missing revision is zero. Thus `0.1.3` equals `0.1.3.0`, `0.1.3.1` is newer than `0.1.3`, and `0.1.4` is newer than both. With equal numbers, a pre-release is lower than the release.
- A remote pre-release is never offered as an update.

### Camera lifecycle and display sleep

`monitor_display_state` accepts `monitor_id`, the current monitor connection `generation`, and `state` (`unknown`, `awake`, or `asleep`). Invalid states and unknown monitor IDs are rejected; stale connection observations are ignored. Each monitor snapshot reports `display_state`. Unknown state is not classified as asleep. When all eligible external monitors (stable identity, supported brightness, known baseline) are asleep, camera mode is `suspended`, capture is `stopped`, and the next-sample delay is null. Any awake eligible monitor above the brightness threshold permits monitoring. Brightness zero does not indicate display sleep.

The brightness threshold `presence_min_brightness_percent` (default 20, range 1 to 90; the comparison is strict, so a monitor exactly at the threshold does not count) applies in the same way. Presence checking needs at least one eligible monitor that is awake and above the threshold. Otherwise the camera is `suspended` with capture `stopped` and a null next-sample delay. Display status is `suspended_low_brightness` when an eligible monitor exists, and `suspended_no_monitor` when none does. When several suspension reasons apply, battery states take precedence, then `suspended_display_sleep`, then `suspended_low_brightness`, then `suspended_no_monitor`. The existing no-eligible-monitor suspension is unchanged. The level compared is the monitor's `baseline` while the app owns its brightness or a protection reason is active (presence or screensaver dimming, or a pending restore write), and the observed level otherwise. A monitor with `presence_dimmed` set always counts as above, so its return check keeps running; an unknown observed level also counts as above. A monitor that falls to or below the threshold restarts its absence count. A connected monitor that rises above the threshold, or a threshold lowered below it, starts a resume as described below. Newly discovered monitors do not trigger a resume.

`always_on_camera` retains an eligible capture session in `continuous_monitoring` mode during face and light-only checks. It does not bypass presence enablement, battery suspension, display sleep, camera errors, no-monitor suspension, or shutdown. Inference follows the configured intervals, and frames between checks are discarded. Existing return checks continue to hold capture regardless of this setting. Presence observations received while disabled or suspended, or carrying an obsolete camera generation, are ignored.

Light-only retries keep `next_sample_in_ms` at 10000. The native adapter then holds capture for five seconds before assessing a fresh frame; completed assessments are approximately 15 seconds apart plus acquisition latency. Warmup uses a monotonic clock, performs no face inference, and does not count as absence. Initial acquisition and acquisition after settling each have a separate bounded timeout. Two usable-light results resume face checks.

Known-asleep targets receive no hardware writes. Sleep invalidates outstanding target actions; wake uses fresh brightness readback and existing ownership rules before protection or restoration writes. Screensaver protection remains an independent reason. Native detection follows OS-reported sleep and cannot guarantee detection of manual power-off when a monitor retains its logical connection.

### Camera selection history and resume sampling

`camera_history` is a read-only settings list of up to 8 camera IDs that the user selected, most recent first. Its first entry always equals `selected_camera_id`. A payload containing `camera_history` is rejected by `update_settings` with the error `camera_history is read-only`. Selecting a camera moves it to the front without duplicates, and selecting the camera already at the front changes nothing. `update_settings` bumps the camera generation only when the settings actually changed. Setting `selected_camera_id` to `null` (the system default camera) clears both the selection and the history. Files without the key load with an empty history, and a file that contains only `selected_camera_id` loads with that ID as a one-entry history. The key is additive to schema 1 and does not change ABI 1, but it carries the same compatibility caveat as the settings paragraph above: an older binary that rejects unknown fields may back up and reset a settings file that contains `camera_history`.

The native macOS shell resolves the effective camera when capture starts and when a camera connects or disconnects. It uses the first history entry that is currently connected and otherwise the system default camera. This fallback is a runtime decision and is never written back to settings, so a temporarily disconnected camera keeps its place at the front of the history and is used again after it reconnects. The settings picker shows the same effective camera.

A resume is triggered by an event rather than by a camera-mode transition: an eligible display going from asleep to awake, a monitor rising above the brightness threshold (or the threshold being lowered below it), power returning from battery to AC while battery suspension is on, or presence protection being re-enabled. On a trigger the core clears stale low-light state and resets `busy` or `unavailable` camera status to `unknown`; `permission` and `error` are not reset. The `retry_in_ms` timer for `busy` and `unavailable` (see the camera snapshot above) also ends here: when it elapses, the shell sends `unknown`, and the next sample follows after 1 second. In face mode, the snapshot reports `next_sample_in_ms` as 1000 until the first accepted camera report, instead of the configured interval. Later samples keep the configured interval, and `presence_sampling_interval_s` is unchanged. `light_only` keeps its existing cadence. In face mode, while a protected, awake monitor has exactly one no-face check on record and is not yet dimmed, `next_sample_in_ms` is `return_sampling_interval_s * 1000`; the second consecutive no-face check dims it.

In single-frame face mode, when a sample's frame is assessed as `insufficient_light`, the native adapter keeps the camera session open for five seconds and discards frames while exposure settles. It assesses one frame at the end of that window and reports only that final result to the core. A usable frame is assessed normally. A dark final frame is reported as `insufficient_light`, so `low_light` and the `light_only` cycle start only when the settled sample is also dark. This applies to single-frame face-mode samples only; continuous capture modes keep reporting an insufficient-light frame immediately. In a dark room each single-frame face sample keeps the camera on about five seconds longer before it reports.

Physical camera timing, wake latency and energy effects of these changes are not verified; the hardware checks for them are pending.

### Built-in display brightness follow

The native shell reports the built-in display with `builtin_display`, a runtime command that is not persisted. It takes `available` (Boolean, required), `brightness` (number or `null`), and `calibrating` (Boolean, required). `brightness` is a finite percentage from 0 to 100, or `null` when the level is unknown; the core keeps no stale level, so follow targets are computed only from a known level. An unknown key, a wrong type, or a `brightness` outside 0–100 or not finite returns a command error and leaves the state unchanged. A successful command increments the revision.

Follow is *active* when `ambient_follow_enabled` and `available` are both true. It is *applying* when it is active, `brightness` is known and `calibrating` is false. While applying, each eligible monitor gets `follow_raw = round(normalized_to_raw(curve(brightness)))`, clamped to the monitor's raw range. A monitor is eligible when its brightness feature is supported, its identity is stable, and `ambient_curves` holds a complete curve for its ID, meaning at least two points. `curve` is piecewise linear through the knots (MacBook level, monitor percent): the origin (0, 0), then each saved point as (`builtin`, `monitor`) in ascending order, where each `builtin` is the MacBook level at which the monitor looks equal to the monitor at `monitor`% of its range. Below the first point it interpolates from the origin. Above the last point the monitor stays at the last point's `monitor` percent, so a last point below 100 caps the monitor there; add a 100% point to let it reach full brightness. Inputs below 0 or above 100 are clamped.

When follow is not applying, `follow_raw` is cleared and the monitor keeps its current baseline; follow issues no writes. When `follow_raw` is set, the baseline becomes `follow_raw` and `manual_override` is unchanged. A write is queued only when the monitor is awake, not dimmed by a protection reason, not already within one raw unit of `follow_raw` (observed or desired), and the bounded hardware queue accepts it. Presence or screensaver dimming keeps the dimmed level and updates only the baseline; when protection ends, the existing restore writes the baseline, which is the mapped value.

`set_brightness` is unchanged and still sets `manual_override` and the baseline, both as the whole raw unit. The calibration uses it to set the monitor to each point; the calibration slider for the MacBook screen is written by the native shell, not through this command. While calibration is in use the shell reports `calibrating: true`, which pauses follow as described above. `monitor_discovered` also recomputes `follow_raw` for the new monitor.

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

Hardware camera behavior, DDC writes/restoration, and any accelerator energy benefit are not established by these interfaces or their source-level tests. NPU providers are not implemented.

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
| `3` | reserved | Reserved. NPU providers are out of scope. The create functions reject it with `-1`. The value `3` is also the create-function unavailable status and is not a provider value. |

```c
int32_t tv_inference_create_with_provider_device(const char *runtime_abs_path,
                                                 const char *model_abs_path,
                                                 int32_t provider,
                                                 uint32_t device_index,
                                                 TvInferenceHandle **out_handle);

int32_t tv_inference_directml_available(void);
```

`tv_inference_create_with_provider_device` is `tv_inference_create_with_provider` with a DirectML adapter index added. It has the same paths, return codes and handle rules as `tv_inference_create`. `device_index` is an index into the DXGI adapter enumeration order (`IDXGIFactory1::EnumAdapters1`) as seen by ONNX Runtime for provider `2`. For provider `1` (CoreML), it selects compute units: `0` = `ALL`, `1` = `CPUAndNeuralEngine`, `2` = `CPUAndGPU`; other values return `-1`. Provider `0` ignores the index. Registration failure falls back to CPU and `tv_inference_selected_provider` reports `0`. Index `0` gives the same session as `tv_inference_create_with_provider`. An index that ONNX Runtime cannot register falls back to CPU: the return value is still `0` and `tv_inference_selected_provider` reports `0`. On the Windows development desktop the `EnumAdapters1` index selected the same physical adapter as the DXGI and DXCore LUID classification (index 0 the discrete RTX, index 1 the integrated Radeon, confirmed by per-LUID GPU utilisation); this is one machine, so the Windows shell still takes the index from the live DXGI catalog and never assumes it.

`tv_inference_directml_available` returns `1` only when all of these hold: the build was compiled with the `directml` cargo feature (off by default; the shipped Windows `telaviva_inference.dll` enables it), the platform supports DirectML, a successful create call has initialized the runtime, and the loaded runtime lists the DirectML provider. In every other case it returns `0`, including builds without the `directml` feature and calls made before the runtime is initialized. It never returns an error and does not need a created detector.

`tv_inference_selected_provider` returns `2` when the session was built with DirectML.

The shipped Windows runtime is the DirectML build of ONNX Runtime 1.20.1, installed as `onnxruntime.dll`. Its DirectX and DirectML dependencies are delay-loaded, so the CPU provider works where DirectML is missing, and a single runtime serves every device (the runtime path is bound once per process). `DirectML.dll` is the operating system's copy and is not redistributed. Which device runs detection is chosen by the setting below. On the Windows test machine a DirectML session returned the same status and confidence as CPU on the test fixture; energy was not measured, so no energy benefit is claimed (see [the Windows measurements](specs/archived/windows-version/validation/measurements.md)).

## inference-device-selection

### Setting `inference_device`

Settings schema 1 adds `inference_device`, a string with default `"auto"`. Older files load with `auto`; as with the earlier additive schema-1 fields, a build without this field treats a `settings.json` that contains it as corrupt, because it rejects unknown fields. This is additive to core ABI version 1 and changes no function signature.

| Value | Meaning | Used on |
|---|---|---|
| `auto` | The platform chain, validated, with CPU fallback | both |
| `cpu` | CPU provider only | both |
| `ane` | CoreML with the Neural Engine preference | macOS |
| `gpu` | CoreML with the GPU preference | macOS |
| `gpu:<vendor>:<device>:<ordinal>` | A specific GPU: 4 lowercase hex digits for the PCI vendor and device ids, then the decimal ordinal among adapters with the same ids (1 to 3 digits) | Windows |

`update_settings` accepts `inference_device` as a string of at most 32 characters in exactly one of the shapes above, and rejects anything else (empty, uppercase, a missing segment, a non-string) with an error that names `inference_device`, leaving the settings unchanged. The core accepts every shape on every platform and does not know the platform; a shell that cannot map a stored value to a present device behaves as `auto` for that run, reports it as unavailable, and keeps the stored value.

### Windows behavior

- `auto` uses the first non-software adapter that DXCore reports as integrated (`IsIntegrated`, matched to the DXGI adapter by LUID) and has no failure marker, and the CPU otherwise. A discrete GPU, or an adapter whose kind could not be classified, is never chosen by `auto`, but can be chosen by its id.
- An accelerated device is used only after it creates a session with provider `2`, returns present for the bundled self-test frame as a CPU reference session does, and answers within 250 ms. It does not need to be faster than the CPU. On any failure, and on a runtime error later, detection moves to the CPU and the failed frame is retried there.
- Under `auto` a failure writes a marker `(device id, driver, app version)` to `inference-failures.json`, which later launches respect until the driver or app version changes. An explicit choice neither reads nor writes markers.
- The shell exposes a report (requested id, active id and label, a state of `ok`, `auto_cpu`, `unavailable` or `failed`, and the failure reason) to the Advanced page. It is not part of the core snapshot.

The Windows Advanced page lists Automatic, each hardware GPU by name and kind, and CPU.


### macOS behavior

The Advanced page lists Automatic, Neural Engine preference, GPU preference and CPU. The shell forwards changes through `PresenceCamera.setInferenceDevice`, releases the detector, and creates the new session lazily at the next face-detection request. A shell-owned observable report carries requested/active ids, active label, state (`ok`, `auto_cpu`, `unavailable`, `failed`), failure reason and whether the session has been decided; none of this enters the shared core snapshot.

CoreML uses the provider's string `MLComputeUnits` option through the versioned runtime API. macOS bundles ONNX Runtime 1.23.0 with unchanged `ort` rc.9/API 20 bindings; Windows retains 1.20.1. The initial macOS 1.22.0 target was replaced after reproducing a runtime process-exit mutex abort, including CPU-only sessions.

The shell checks the bundled self-test frame against a CPU reference and a 250 ms budget for the first detection of each preference session on both platforms, because the first CoreML inference compiles lazily. A candidate failure selects CPU; a runtime accelerator error retries the same frame on CPU and retains CPU for the preference session, including after idle detector release. Successful validation is also retained across idle recreation. A changed preference resets the decision. A foreign-platform id is retained, behaves as Automatic and reports `unavailable`. CoreML preferences do not prove which physical compute unit executed individual operators. No network use is added.

## Laptop display protection

The laptop's built-in screen is a protectable display in the shared core, under the reserved id `"builtin"`. The core decides and the macOS and Windows shells apply. The change is JSON only: ABI version `1` (`TV_CORE_ABI_VERSION`) and settings schema `1` are unchanged, no function signature changes, and the built-in screen never appears in the snapshot `monitors` object. The behavior is in [ARCHITECTURE.md](ARCHITECTURE.md) and the decision is in [ADR 0035](adr/0035-laptop-display-protection-and-auto-brightness.md).

This section supersedes the earlier ranges for `presence_min_brightness_percent` (now 1 to 90, not 10 to 90 or 0 to 90) and the protection level (1 to 50 percent, not 0 to 100).

### Command `builtin_display`

The command in [Built-in display brightness follow](#built-in-display-brightness-follow) gains two optional keys. It remains a runtime command and is not persisted.

```json
{ "type": "builtin_display", "available": true, "brightness": 62, "calibrating": false, "protectable": true, "holding": false }
```

- `protectable` (Boolean, optional, default `false`): the shell has a built-in screen whose brightness it can adjust. It stays `true` while the lid is closed, so the camera and display-sleep logic still see the screen.
- `holding` (Boolean, optional, default `false`): the shell holds the screen dimmed and has not finished restoring it. It is ignored unless `protectable` is `true`.
- A non-boolean `protectable` or `holding` returns a command error (`protectable must be boolean` or `holding must be boolean`) and changes no state. The validation of `available`, `brightness` and `calibrating` is unchanged.
- Freeze: while `holding` is `true` or any reason is active, `brightness` does not replace `builtin_percent`, which keeps the last undimmed level. Follow pauses until the freeze ends and then uses the next fresh report.
- `available: false` clears the built-in absence count and presence dim.

### Snapshot `builtin_protection`

A top-level object next to `ambient`:

```json
"builtin_protection": { "protectable": true, "protected": true, "available": true, "reasons": ["presence"], "target_percent": 1.0, "hold_auto_brightness": false, "holding": true }
```

- `protectable`: the last `protectable` value from `builtin_display`; `false` for a shell that never sends it.
- `protected`: `protectable` is `true` and `"builtin"` is not in `protection_disabled`.
- `available`: the last reported availability.
- `reasons`: `"presence"` and/or `"screensaver"`, in that order. Empty unless the screen is protected, available and not calibrating, and the core is open.
- `target_percent`: `null` without reasons; otherwise the protection level of `"builtin"` (see below).
- `hold_auto_brightness`: `true` when `builtin_auto_brightness_off_on_protection` is `true` and `reasons` is not empty.
- `holding`: the last `holding` value reported by the shell. It is `false` unless `protectable` is `true`.

### Snapshot `monitored_display`

A top-level object, next to `builtin_protection`:

```json
"monitored_display": { "id": "builtin", "automatic": true }
```

`id` is the selected display: the first connected entry of `monitored_display_history`, otherwise Automatic. Automatic picks the external monitor flagged `primary`, then the first usable external monitor by id, then the built-in screen when it is protectable and available. An external monitor is usable when its identity is stable and its brightness is supported. `id` is `null` when nothing resolves. `automatic` is `true` when `monitored_display_history` is empty.

The camera gate uses the first candidate in that order that is awake (the gate display), so a sleeping display falls through to the next one. The camera runs while the gate display is above its threshold or its level is unknown, and also while any display is presence-dimmed so that the return check continues. It is suspended for display sleep only when every candidate sleeps and no display is dimmed. Absence counting and dimming still cover every protected screen.

### Command `monitor_discovered` `primary`

The `monitor` object of `monitor_discovered` accepts an optional `primary` Boolean (default `false`; `primary must be boolean` otherwise). The macOS shell sets it from `CGDisplayIsMain`. It is a hint for Automatic only. A repeated `monitor_discovered` for the same id replaces the flag, so a change of the main display reaches the core at the next discovery of that monitor. Snapshot monitor objects report `primary`.

### Setting `builtin_auto_brightness_off_on_protection`

Boolean, default `false` (`setting must be boolean` otherwise). When `true`, the shell turns the system's automatic brightness off while protection dims the built-in screen, and turns it back on afterwards unless a fresh read says it is already on (a nil or off read still turns it on). When `false`, protection never reads or writes automatic brightness. The core publishes the decision as `hold_auto_brightness`. The notices for an unreadable or unchangeable automatic-brightness state are shell text in en-US and pt-BR, not core fields.

### Setting `monitored_display_id` and `monitored_display_history`

- `monitored_display_id`: a monitor id or `"builtin"`, or `null`. `null` selects Automatic and clears the history. A string moves to the front of the history, which holds at most 8 unique ids. Any other non-null value returns `monitored_display_id must be string or null`; an empty or over-long id returns `invalid monitored display ID`.
- `monitored_display_history`: read-only, most recent first. Its first entry always equals `monitored_display_id`. Setting it returns `monitored_display_history is read-only`.

Files without these keys load as Automatic with an empty history.

### Setting `presence_min_brightness_by_display`

A map from display id (a monitor id or `"builtin"`) to a threshold from 1 to 90 percent. Default `{}`. A stored entry in [0, 1) loads as 1. Keys are 1 to 512 bytes, and the map holds at most 128 entries. `update_settings` replaces the whole map, as it does `screensaver_targets`. A value out of range returns `invalid presence threshold by display`. Entries for disconnected displays are kept. The camera gate compares the level of its gate display (the first awake candidate, which is the monitored display unless it sleeps) with that display's entry, or with `presence_min_brightness_percent` when it has none. The Settings slider edits the monitored display's entry, or the global value when no display resolves.

### Setting `presence_min_brightness_percent`

The valid range is now 1 to 90 (the sections above give 10 to 90 and 0 to 90, which this section supersedes). The default stays 20. Out-of-range values, including 0, return `presence brightness threshold out of range` and nothing is stored. A stored value in [0, 1) is raised to 1 on load and the other settings are kept; a stored value below 0 or above 90 still fails validation as before.

### Setting `screensaver_targets` as the protection level

Keys are display ids, including `"builtin"` for the built-in screen. Stored values from 0 to 100 are accepted; a non-finite or out-of-range value returns `invalid screensaver target`. The applied level is clamped to 1 to 50 percent, and a display without an entry uses 1. The same level applies to presence absence and to the screensaver, for each protected monitor and for the built-in screen. Presence absence now uses this level too, which earlier builds did not.

### Setting `protection_disabled` with `"builtin"`

`protection_disabled` accepts `"builtin"` as a display id, validated like a monitor id. Adding it turns protection off for the built-in screen: the core clears the built-in presence dim and absence count, and the shell restores the screen. Removing it turns protection on again with a fresh absence count and no immediate dim.

### Diagnostics

- `builtin_protection reasons=<csv or none> target=<percent or none>` is written when the set of reasons changes.
- `presence_dim` for the built-in screen uses the reserved id, hashed like a monitor id (`monitor=id-xxxxxxxx`, see [ADR 0034](adr/0034-local-diagnostic-logs.md)).
- Shell lines (`builtin_dim`, `builtin_restore`, `builtin_restore_failed`, `builtin_recovery`, `auto_brightness_unknown`, `auto_brightness_not_changed`, `auto_brightness_restore_failed` and `auto_brightness_recovered`) are written by the shell to the unified log, not returned by `tv_diagnostics_drain`.
- Reconnect lines from the core: `dim_resumed monitor=id-xxxxxxxx baseline=N` (a parked dim restored after the monitor returned at the dimmed level), and `ownership_dropped` with `reason` `read_mismatch`, `read_failed`, `restore_failed` or `write_failed`. See the [reconnect section](#external-monitor-reconnect-restore--2026-10-21).

### Compatibility

- ABI version stays `1`. All changes are JSON fields and settings keys.
- Settings schema stays `1`. The new keys are optional with defaults, and older files load with those defaults. An older build that rejects unknown fields treats a file containing these keys as corrupt, renames it to `settings.json.corrupt` and starts from defaults, as it does for the earlier additive keys.
- Windows sends `protectable` and `holding` in `builtin_display` since 2026-10-29 ([ADR 0043](adr/0043-windows-laptop-display-parity.md)), so the built-in policy applies there too.
- The DisplayServices writes and the automatic-brightness setter are private macOS APIs and are unverified on hardware.

## external-monitor-reconnect-restore — 2026-10-21

The command contract is unchanged, and the behavior below is runtime only: nothing is persisted. The rule and its rationale are in [ARCHITECTURE.md](ARCHITECTURE.md#external-monitor-reconnect-restore-and-whole-unit-manual-values--2026-10-21) and [ADR 0006](adr/0006-keep-brightness-restore-pending-after-failed-wake-readback.md#amendment-2026-10-21-restore-a-dim-after-reconnect).

### `monitor_discovered` and `monitor_disconnected`

- `monitor_disconnected` bumps the monitor's connection generation, drops its pending actions and removes it from the snapshot. Before removal, a dim the core holds on it is parked: the app owns the brightness and the device value differs from the baseline by more than 0.5 raw units. The parked entry keeps the baseline (rounded) and the dimmed raw value. Parking writes no log line.
- `monitor_discovered` for a known id bumps the connection generation and the camera generation and drops that id's pending actions, as before. A dim held by the monitor being replaced is parked first, then the rule below applies.
- Resume rule: the parked entry for the id is consumed only when the discovered identity is `stable` and brightness is supported. If the discovered raw value is within 0.5 of the dimmed raw value, the parked baseline is restored through the normal ownership path, which reads first and then writes. Otherwise the discovered raw value becomes the baseline and nothing is restored. An ambiguous or unsupported rediscovery leaves the entry parked. A parked entry is never used for another id.
- Log lines: `monitor_discovered` (with `raw` and `baseline`, both the device reading before any resume), `dim_resumed monitor=id-xxxxxxxx baseline=N` (only when a parked dim is restored), and `monitor_disconnected monitor=id-xxxxxxxx`.

### Lifetime and limits

- At most 16 parked entries. When a new entry would exceed the cap, the first id in key order is dropped, not the oldest.
- An entry lasts until a stable, supported discovery of its id, shutdown, or eviction. `tv_shutdown` discards every entry, so a parked dim is never restored on quit. A quit before the monitor returns loses the dim, and the next launch adopts the dimmed reading.
- A resumed monitor starts with no presence reason. If the user is still away, it dims again after two absent checks.
- On macOS the monitor id includes the port's IORegistry location, so a monitor on another port or dock has another id and is not restored.

### Whole-unit manual values

`set_brightness` and `set_volume` store the mapped raw value as a whole unit (round, then clamp). The `desired` and `baseline` fields are therefore whole numbers that a DDC readback can match.

### Diagnostics

- `dim_resumed monitor=id-xxxxxxxx baseline=N`
- `ownership_dropped` with `reason` `read_mismatch`, `read_failed`, `restore_failed` or `write_failed`.

Hardware checks are pending on both platforms, and this section does not claim a physical reconnect restore. The path applies to Windows DDC monitors too.

## Keep awake with presence

Settings schema 1 adds `keep_awake_with_presence`, a Boolean defaulting to false when absent. Enabling it adds an independent camera consumer: monitoring bypasses battery, display brightness and eligible-monitor gates, and uses retained capture while keeping the configured inference cadence. It does not rewrite `always_on_camera`, `suspend_presence_on_battery` or protection settings. Brightness dimming retains its independent eligibility and two-miss rule.

The strict command `{"type":"keep_awake_session","active":true}` reports native camera-monitoring eligibility. The field is required and Boolean, and unknown fields are rejected. Eligibility starts false until the shell reports it. Repeated identical values do not invalidate evidence. Changes while enabled invalidate camera generation and positive evidence; when disabled, the value is remembered without disturbing brightness-only camera behavior. Native shells veto activity separately for screensaver/display state without unnecessarily stopping local camera monitoring. A known locked/inactive/unknown-lock session or actual system sleep disables monitoring. Supported external-camera clamshell operation stays eligible while the OS session is awake. Native effects always require fresh evidence after a veto ends.

The additive `keep_awake` snapshot contains `enabled`, `requested`, `observation_id`, `generation`, `valid_for_ms` and `reason`. `requested` means current-generation positive evidence with an unexpired monotonic lease, not proof of OS acceptance. `observation_id` is the accepted positive capture timestamp (zero without evidence); pair it with camera `generation`. A positive sample grants the normal sampling interval plus 15000 ms, capped at 315000 ms. `valid_for_ms` reports only the remaining duration. Snapshot reads never renew it. First accepted absence, low light, unavailable detection, generation invalidation, disabled setting or inactive session revokes the request. `usable_light` alone cannot grant it.

Native drivers mirror the remaining lease with independent monotonic expiry and cannot extend it from a repeated token. They own temporary energy requests and guarded activity pulses, stop on explicit session/saver/power transitions, and expose local permission/policy status. These do not change global OS settings or the ABI version. The Windows shell consumes the same settings key, `keep_awake` snapshot and `keep_awake_session` command; it uses `SetThreadExecutionState` and zero-displacement `SendInput` mouse moves, reports a policy-blocked status when synthetic input resets are blocked, and is covered by native fake-backend tests; physical acceptance is tracked in the archived feature validation report. See [ADR 0040](adr/0040-presence-driven-idle-inhibition.md), the [detailed contracts](specs/archived/keep-awake-with-presence/contracts.md) and [validation evidence](specs/archived/keep-awake-with-presence/validation.md).

## lock-on-absence — 2026-10-29

The opt-in lock after confirmed absence adds two settings, a snapshot `lock` block and one command. The core decides when a lock is due; the shell performs the lock, confirms it and reports the result. The rule and its reasons are in [ADR 0041](adr/0041-lock-session-on-confirmed-absence.md) and the [feature contracts](specs/archived/masked-face-detection-and-lock-on-absence/contracts.md). The macOS and Windows implementations each have automated tests, and the owner marked the hardware checks on both platforms as passed on 2026-10-31, with no values recorded.

### Settings `lock_on_absence` and `lock_delay_s`

| Key | Type | Default | Validation |
|---|---|---|---|
| `lock_on_absence` | Boolean | `false` | None. Enables the lock stage after a confirmed absence. |
| `lock_delay_s` | number | `5` | `update_settings` accepts 1 to 60 and rejects any other value, with nothing stored. On load a stored value is clamped into 1 to 60. The shells show whole seconds. |

Both keys are whitelisted in `update_settings` and default when absent from a file. Example:

```json
{"type":"update_settings","settings":{"lock_on_absence":true,"lock_delay_s":10}}
```

The lock stage runs only while `lock_on_absence` is true, presence protection is on, a display is presence-dimmed, the session is active and the camera is in face mode. `lock_delay_s` is the time between arming (the absent sample that dimmed the display) and the confirming absent sample. Insufficient light, a present sample, an unavailable camera or a disabled option return the stage to `idle`, so no lock is requested in unfavorable light.

### Snapshot `lock`

```json
"lock": {
  "enabled": true,
  "stage": "armed",
  "requested": false,
  "token": null
}
```

| Field | Type | Meaning |
|---|---|---|
| `enabled` | boolean | Mirrors `settings.lock_on_absence`. |
| `stage` | string | `idle`, `armed`, `requested`, `locked` or `failed`. `idle` when the option is off. |
| `requested` | boolean | `true` only in stage `requested`; the shell must lock the session now. |
| `token` | number or null | Unique per request, a counter starting at 1 for each core instance. Non-null only in the stages `requested`, `locked` and `failed` for the current episode. |

- `idle`: no absence episode is being timed.
- `armed`: a display was dimmed by an absent sample and the delay is running.
- `requested`: a lock is due. A shell executes a request once per token and remembers the last token it handled; a snapshot with the same token and `requested: true` is not executed again.
- `locked`: the shell confirmed the lock. While the session is inactive the camera is `suspended`. The stage stays `locked` until the session becomes active again or no display is presence-dimmed any more.
- `failed`: the shell reported `ok: false`. The display stays dimmed, and no second request is made in the same absence episode.

When the session becomes active again, the stage returns to `idle`, the camera generation changes and the camera resumes; the first sample follows about 1 second later.

### Command `lock_result`

```json
{"type":"lock_result","token":3,"ok":true}
```

| Field | Type | Rule |
|---|---|---|
| `token` | number | Must equal the current request token. Any other token is accepted and ignored. |
| `ok` | boolean | `true` only when the shell confirmed that the session is locked. `false` when the request failed or was not confirmed in time. |
| `error` | string, optional | Short code such as `unavailable` or `not_confirmed`. No identifiers; at most 64 characters. |

Unknown fields are rejected, as in the other commands.

### Session state reporting

When `lock_on_absence` or `keep_awake_with_presence` is on, each shell reports `{"type":"keep_awake_session","active":<bool>}` on every session lock, unlock, fast user switch and screensaver change. `active` is `false` while the session is locked. The core relies on this report to suspend the camera while locked and to resume after unlock. The Windows shell reports it on every session lock and unlock, screensaver change and display change, regardless of these settings, and its screensaver monitor also runs when only `lock_on_absence` is on.

### Compatibility

- ABI version stays `1`. The C headers do not change, and no function signature changes. The settings, the `lock` block and `lock_result` are JSON.
- Settings schema stays `1`. Both keys are optional with defaults. An older build that rejects unknown fields treats a `settings.json` that contains them as corrupt: it renames the file to `settings.json.corrupt` and starts from defaults, as it does for the earlier additive keys.
- With the option off (the default) the stage is always `idle`, and the snapshot differs from the previous release only by the `lock` block with `enabled: false`.
