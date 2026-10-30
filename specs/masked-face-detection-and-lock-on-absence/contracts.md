# Contracts: Occlusion-robust face detection, 1% presence-check floor and lock screen on absence

The C ABI (`include/telaviva_core.h`, `include/telaviva_inference.h`) and its version do not change. Every contract below travels as JSON through `tv_submit` and `tv_snapshot`, or lives in the repository.

## Data Models

### Settings changes (`settings.json` and the snapshot `settings` object)

| Key | Type | Default | Validation | Notes |
|---|---|---|---|---|
| `presence_min_brightness_percent` | number | 20 | 1 to 90 (was 0 to 90) | A stored value in `[0, 1)` is raised to 1 on load |
| `presence_min_brightness_by_display` | map of id to number | `{}` | each entry 1 to 90 (was 0 to 90); same key and entry limits as today | Includes the `builtin` entry; stored values in `[0, 1)` are raised to 1 on load |
| `lock_on_absence` | boolean | `false` | none | New, `#[serde(default)]`, added to the `update_settings` whitelist. The settings struct denies unknown fields, so older binaries reset the file |
| `lock_delay_s` | number | 5 | 1 to 60 on update (out of range is rejected, nothing stored); on load a stored value is clamped into 1 to 60 | New, `#[serde(default)]`, whitelisted; seconds between arming and the confirming sample; the shells show whole seconds |
| `occlusion_assist` | boolean | `true` | none | Only if outcome O3 is built; absent otherwise |

Update-settings example:

```json
{"type":"update_settings","settings":{"lock_on_absence":true,"lock_delay_s":10,"presence_min_brightness_percent":1}}
```

An update with `presence_min_brightness_percent: 0` is rejected with `presence brightness threshold out of range`, and nothing is stored (existing behavior of a rejected update).

### Snapshot `lock` block (new)

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
| `enabled` | boolean | Mirrors `settings.lock_on_absence` |
| `stage` | string | `idle`, `armed`, `requested`, `locked`, `failed` (`idle` when disabled) |
| `requested` | boolean | `true` only in stage `requested`; the shell must lock now |
| `token` | number or null | Unique per request, a counter starting at 1 for each core instance; non-null only in the stages `requested`, `locked` and `failed` for the current episode |

A shell executes a request once per token and remembers the last token it handled. A snapshot with the same token and `requested: true` is not executed again.

### Command `lock_result` (new)

```json
{"type":"lock_result","token":3,"ok":true}
```

| Field | Type | Rule |
|---|---|---|
| `token` | number | Must equal the current request token; any other token is accepted and ignored |
| `ok` | boolean | `true` only when the shell confirmed that the session is locked; `false` when the request failed or was not confirmed in time |
| `error` | string, optional | Short code such as `unavailable`, `not_confirmed`; no identifiers; at most 64 characters |

The command is added to the command allow-list in `core.rs` (the list at the top of the command dispatcher). Unknown fields are rejected like in the other commands.

### Fixture manifest (`crates/telaviva-inference/tests/fixtures/occlusion/manifest.toml`)

```toml
[[image]]
file = "mask-surgical-01.jpg"
category = "mask"            # none | mask | hand_mouth | hand_eyes | glasses | tilt | no_face_hand | no_face_mask | no_face_object
has_face = true
occlusion = "partial"        # none | partial | heavy
source_url = "https://commons.wikimedia.org/wiki/File:..."
license = "CC0-1.0"          # CC0-1.0 or PDM-1.0 only
author = ""
subject_rights_checked = true
notes = ""
```

Rules: every image has a source URL, a license of CC0 or public domain, and a `subject_rights_checked` value; images with identifiable people require `true`. Images are downscaled to at most 640 px on the longest side. A `README.md` next to the manifest lists the coverage gaps.

### Known-model table (inference crate, Decision 2)

| Field | Meaning |
|---|---|
| `sha256` | Digest of the ONNX file; the only way a model becomes known |
| `family` | `yunet` or `retinaface` |
| `input` | Static input side (640) |
| `mean_bgr` | Per-channel subtraction before inference (`[0,0,0]` for YuNet, `[104,117,123]` for RetinaFace) |
| `confidence` | Presence threshold (0.8 for YuNet today; the benchmark's operating point for the adopted model) |
| `nms_iou` | 0.3 for YuNet, 0.4 for RetinaFace |

An unknown digest returns `MODEL_ERROR` before a session is created, as today.

## API Contracts

### Benchmark report (`occlusion_bench`, output in markdown)

Command: `cargo run -p telaviva-inference --features bench --example occlusion_bench -- --model <path> --family <yunet|retinaface> [--runtime <libonnxruntime path>]`

Output sections: (1) per-category recall at each threshold of the grid 0.30 to 0.90 step 0.05, with Wilson 95% intervals; (2) face-free false positives per threshold; (3) latency p50 and p95 over 100 runs on CPU and the process memory after loading; (4) the operating threshold computed by rule 5 of Decision 3 and a pass or fail line for each rule 1 to 4; (5) the coverage gaps copied from the fixture README.

Validation rules: refuses a fixture whose manifest entry lacks a source URL or a CC0 or public-domain license; refuses images larger than 640 px.

## Integration Contracts

### macOS locker (`SessionLocker.swift`)

- Input: none. Output: `locked`, `unavailable` (symbol missing) or `not_confirmed` (called, state did not show locked within 3 s).
- The only file that names `SACLockScreenImmediate` or `login.framework`. A missing library or symbol returns `unavailable` without a crash.
- Confirmation reads the existing session state (`CGSSessionScreenIsLocked`) and listens to `com.apple.screenIsLocked`, subscribed before the call.
- A diagnostic line is written on every result, with no identifiers (ADR 0034).

### Windows locker (`Platform/SessionLocker`)

- `ISessionLocker::lockNow()` returns a started flag. The Win32 implementation calls `LockWorkStation()`; a zero return is reported as failure at once.
- `LockCoordinator` waits at most 3 s for the `WTS_SESSION_LOCK` callback already delivered by `SessionEvents`, then submits `lock_result`.
- Runs on the interactive-session process only; the Win32 header is included only in the `.cpp`.

### Shell reporting of the session state

When `lock_on_absence` or `keep_awake_with_presence` is on, the shell reports `{"type":"keep_awake_session","active":<bool>}` on every session lock, unlock, fast user switch and screensaver change, as the keep-awake path does today. `active` is false while the session is locked.

## State Transitions

Lock stage (core), recomputed at the end of `presence()` and of every accepted command. The condition `C` is `lock_on_absence && any_presence_dimmed() && session_active && camera_mode == "face"`.

```
idle      --[C true, absent sample that produced the dim]------------------> armed (arming time = captured_at_ms)
armed     --[C false]-------------------------------------------------------> idle
armed     --[C true, absent sample, captured_at_ms - arming >= lock_delay_s*1000]--> requested (token n)
requested --[lock_result ok:true, token n]---------------------------------> locked
requested --[lock_result ok:false, token n]--------------------------------> failed
requested --[C false]-------------------------------------------------------> idle
locked    --[session_active becomes true]------------------------------------> idle (camera generation bumped, resume starts)
locked    --[present sample or dims cleared]---------------------------------> idle
failed    --[C false (present, insufficient light, unavailable, option off)]-> idle
```

Notes: the stage `locked` is the one exception to condition `C`: it ignores the session-inactive part, because the session is expected to become inactive once the lock takes effect. It stays `locked` while the session is inactive and leaves only when the session becomes active again or when no display is presence-dimmed any more (a present sample, or the keep-awake rule clearing the dims); an `insufficient_light` or `unavailable` sample also clears the dims and so ends it. A failed or locked stage never issues a second request in the same episode, because the stage returns to `idle` only when `C` becomes false or the session becomes active. While `locked` and the session is inactive, `camera.mode` is `suspended`. With the option off, the stage is always `idle` and the snapshot differs from the previous release only by the extra `lock` block (`enabled: false`).

Threshold normalization (settings load):

```
stored value v in [0, 1)  --[load]--> v = 1, file kept, validation passes
stored value v < 0, v > 90, NaN, wrong type --[load]--> validation fails, existing reset-with-backup behavior
```
