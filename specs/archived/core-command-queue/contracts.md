# Contracts: Core-Owned Manual-Control Queue and DDC Dispatch

These contracts define the C ABI additions and the JSON protocol between the shells and `TvControls`. They follow the style of `tv_camera_lifecycle_*` (ADR 0014): one JSON event in, one JSON result out. Field names are snake_case. Unknown fields, duplicate keys, non-finite numbers and trailing data are rejected, and a rejected event leaves the state unchanged.

## C ABI (additive to version 1)

```c
typedef struct TvControls TvControls;

/* Creates an empty controls machine. */
int32_t tv_controls_create(TvControls **out);

/* Applies one UTF-8 JSON event (at most 64 KiB) and copies the JSON result into a
   Rust-owned buffer that the caller frees with tv_buffer_free. */
int32_t tv_controls_step(TvControls *controls, const uint8_t *json, size_t len, TvBuffer *out);

/* Frees the machine. Must not race other calls on the same handle. */
void tv_controls_destroy(TvControls *controls);
```

- Status codes are the existing ones: `TV_OK`, `TV_INVALID_ARGUMENT` (null handle or out, malformed or invalid event), `TV_SIZE_LIMIT` (input over 64 KiB), `TV_INTERNAL` (panic or poisoned lock). Failures write nothing to `out`.
- The handle holds one `Mutex`. Calls are serialized. The lock is never held across I/O and is a leaf lock: callers must not hold another lock that a step could wait on.
- `tests/header_contract.rs` gains the three exports in its function table and prototype strings, and `TvControls` in the opaque-handle list. `tools/windows/check-dll-exports.ps1` verifies the DLL.

## Common event envelope

Every event is a JSON object with a string `type` and a number `now`.

- `now`: monotonic seconds from the shell clock (`CMClockGetHostTimeClock` on macOS, `steady_clock` on Windows). Must be finite. A `now` smaller than the previous one is clamped to the previous one, so a mis-ordered event never moves a deadline backward.

## Events (shell to core)

| `type` | Fields | When the shell sends it |
| --- | --- | --- |
| `configure` | `brightness_step`, `volume_step` (numbers, each clamped to whole points 1 to 15; the macOS shell has two user settings, the Windows shell sends its volume step for both) and `keys_enabled` (bool) are required: a missing one is rejected, and a non-finite step is rejected as invalid input. The machine uses 5 as the step only before the first `configure`. `volume_owner` (optional string or null: the monitor that owns volume keys; absent leaves it unchanged, a change clears all remembered mute levels). `keys_enabled: false` also clears the volume owner, and a `volume_owner` in the same event is then ignored (the owner only applies while keys are enabled) | At start and when the key step setting or key routing changes |
| `observe` | `revision` (u64), `monitors` (array), `pending_actions` (array) | After every snapshot publish, in snapshot order |
| `monitor_profile` | `monitor_id`, `manufacturer` (EDID code, may be empty) | After discovery of a DDC-capable monitor. Optional, see below |
| `monitor_removed` | `monitor_id` | When the shell drops a monitor |
| `display_power` | `on` (bool) | When the session reports displays off or on |
| `slider` | `monitor_id`, `feature` (`brightness` or `volume`), `value` (number) | On every slider value change |
| `key` | `key`, `phase` (`down` or `up`), `repeat` (bool), `fine` (bool), `owner` (object or null) | On every decoded volume, mute or brightness key event |
| `submitted` | `token` (u64), `accepted` (bool) | After the shell passed a `command_json` or `result_json` to `tv_submit` |
| `attempt_done` | `token` (u64), `ok` (bool), `error` (`none`, `unsupported`, `io`, `disconnected`; present when `ok` is false), `value` (number or null; raw readback for reads) | After the shell ran one DDC primitive |
| `tick` | none | When the shell timer for `next_deadline` fires |
| `query_draft` | `monitor_id`, `feature` | When the UI needs the value to draw |
| `shutdown` | none | On application exit |

`monitor_profile` is optional. Without it, the default timing profile (no duplicate write) is used. The machine keeps at most 64 profiles: at the cap a new one evicts the profile of a monitor that is absent from the latest `observe`, and is ignored (default profile) when every stored profile belongs to a listed monitor. What a DDC action needs is that its monitor appears in an `observe`: an action for a monitor absent from the observation fails with `disconnected`.

### `observe`

```json
{
  "type": "observe", "now": 120.5, "revision": 42,
  "monitors": [
    {
      "id": "mon-1", "generation": 3,
      "display_state": "awake", "identity_status": "stable",
      "brightness": { "supported": true, "observed": 41.0, "min": 0, "max": 100 },
      "volume": { "supported": true, "observed": 20.0, "min": 0, "max": 100 }
    }
  ],
  "pending_actions": [
    { "id": 17, "generation": 8, "monitor_id": "mon-1", "kind": "set_brightness", "value": 37.0 }
  ]
}
```

- `observed` is the normalized percentage 0 to 100 (the snapshot's `normalized` field, not its raw `observed` field), because drafts, targets and tolerances are percentages. `min` and `max` are the raw device range from the snapshot. The Windows shell reads the raw `observed` today and compares it with a percentage draft, which is only correct for 0 to 100 ranges; the adapter must now pass `normalized`.
- `observed` may be null. `kind` is one of `set_brightness`, `set_volume`, `read_brightness`; other kinds are not ignored: they fail with `unsupported` and a result is reported.
- A set action's `value` is the raw target; the dispatcher rounds it to the nearest integer.
- A snapshot older than the last applied revision is ignored by the whole machine (manual queue, dispatcher and key policy); an equal revision is applied.
- A channel is eligible when `supported` and `observed` is non-null and `display_state` is not `asleep` and `identity_status` is `stable` and `max > min`.
- Each array is capped (64 monitors, 128 pending actions); more is `TV_INVALID_ARGUMENT`.

### `key`

```json
{ "type": "key", "now": 120.6, "key": "volume_up", "phase": "down",
  "repeat": false, "fine": false,
  "owner": { "monitor_id": "mon-1", "generation": 3 } }
```

- `key` is `volume_up`, `volume_down`, `mute`, `brightness_up` or `brightness_down`. The matching feature is derived from the key.
- `owner` is the target the shell resolved (macOS pointer or default output, Windows route owner). `null` means no eligible target, so the machine answers `consume: false` for the whole sequence. A change of the `volume_owner` given in `configure` clears all remembered mute levels; a different `owner` on a key event does not.
- The shell owns the decision to swallow on Windows. macOS uses the returned `consume`.

## Result (core to shell)

```json
{
  "value": null,
  "effects": [ ],
  "next_deadline": null
}
```

- `next_deadline`: absolute monotonic seconds of the next time the machine needs a `tick`, or `null`. The shell keeps exactly one timer armed for it and re-arms from every result. A `tick` before the deadline does nothing harmful. The deadline covers the 20 ms coalesce drain, the DDC waits (settle, backoff, resubmission) and the wake grace. The 3 s in-flight safety release, the rebase settle window and the outstanding-write expiry are evaluated on the next `observe` or `key` event, not by a deadline.
- `value` depends on the event:

| Event | `value` |
| --- | --- |
| `slider` | `{ "draft": <number>, "status": "pending" }` or `null` when the target was ignored (unknown monitor, ineligible, asleep, draft cap, non-finite) |
| `key` | `{ "consume": <bool> }`; `consume` is `false` when the owner is null or unknown |
| `query_draft` | `{ "has_draft": <bool>, "draft": <number>, "status": "ready" or "pending" or "failed" }` |
| everything else | `null` |

- `draft` for an idle control is the latest observed value, so the UI can always draw `draft`.

## Effects

Effects are an ordered array. The shell executes them in order, outside any lock it holds for its own state.

| `type` | Fields | Shell action |
| --- | --- | --- |
| `submit_command` | `token`, `command_json` | Pass `command_json` verbatim to `tv_submit`; then send `submitted {token, accepted}` where `accepted` is `status == TV_OK` |
| `run_attempt` | `token`, `monitor_id`, `op` (`read` or `write`), `code` (VCP code: 16 brightness, 98 volume), `value` (raw, present for `write`) | Run exactly one DDC primitive on the DDC worker; then send `attempt_done {token, ...}`. The primitive's internal framing (macOS write-twice and checksum seed fallback, Windows single call) is a shell detail and counts as one attempt |
| `submit_result` | `token`, `result_json` | Pass `result_json` verbatim to `tv_submit`; then send `submitted {token, accepted}` |
| `draft_changed` | `monitor_id`, `feature` | Repaint the control (call `query_draft`) |

- Tokens are unique per effect and never reused during a run. An `attempt_done` or `submitted` for an unknown or already-finished token is ignored.
- `command_json` is exactly one of `{"type":"set_brightness",...}` or `{"type":"set_volume",...}` with the fields the core already accepts (including `connection_generation`); `result_json` is the `hardware_result` the core already accepts: `{"type":"hardware_result","action_id":N,"generation":G,"ok":true,"value":RAW}` or `...,"ok":false,"error":"io|unsupported|disconnected|ambiguous"`. `value` is a raw readback, never a percentage.

## Constants

| Name | Value | Origin (names in the Windows code that this change removed) |
| --- | --- | --- |
| Coalesce delay | 20 ms | Windows `kDrainDelay` |
| Re-drain after completion | 0 ms | Windows `kImmediateDrain` |
| In-flight safety release | 3.0 s | Windows `kManualInFlightSafetySeconds` |
| Readback tolerance | 0.51 percent | Windows `kManualReadbackTolerance`, `kKeyReadbackTolerance` |
| Key step | default 5, 1 to 15, fine 1; separate brightness and volume steps | both shells |
| Rebase settle | 1000 ms | both shells |
| Outstanding-write expiry | 10000 ms | both shells |
| Bounds | 128 drafts, intents, in-flight, outstanding, mute memory; 64 early completions; 256 remembered action ids | both shells |
| DDC primitive attempts | 3 per primitive | contracts rule, both shells |
| Backoff between attempts | 200 ms, then 1000 ms | both shells |
| Settle after a write | 60 ms | both shells |
| Duplicate-set gap | 10 ms (profile `GSM` only) | Windows `DdcTimingProfile` |
| Wake grace | 2000 ms | Windows `wakeGrace` |
| Result resubmission | at most 3, backoff 200 ms then 1000 ms | new (Decision 7) |

Retry ceiling pinned by test: one stuck monitor costs at most 4 failed read operations (the first plus three core restore retries), 12 primitive attempts, and about 4.8 s of backoff waiting.

## State transitions

### Slot (per monitor and feature)

```
Idle ──slider/key──▶ Pending(queued) ──tick(20ms)──▶ InFlight ──submitted(accepted)──▶ Acknowledged
  ▲                     │  ▲                              │                                │
  │                     │  └── newer target replaces ─────┘ submitted(rejected)            │ listed in pending_actions
  │                     │                                  ▼                                │ then done, or readback
  │                     │                               Failed (draft := observed)          │ within tolerance
  └────────── completion ok, readback match, or safety release ─────────────────────────────┘
```

- A slider or key target replaces the queued target. Same raw value as the in-flight command: draft is `pending`, nothing queued.
- A completion carries the monitor, feature and connection generation; it has no token. It is ignored when its connection generation differs from the in-flight command's.
- Sleep, a missing monitor, a new generation, or an ineligible channel resets the slot and drops its queue. Drafts and queued targets are dropped and not replayed on wake. A core action that is already listed in `pending_actions` is held through sleep and runs after the wake grace.
- `shutdown` clears all slots; later events are ignored.

### DDC action (per pending action)

```
Listed ──(first per monitor, not asleep, no grace)──▶ Writing(attempt n)
Writing ──ok──▶ [duplicate gap 10ms if GSM] ──▶ Settling(60ms) ──▶ Reading(attempt n)
Writing/Reading ──transient error, n<3──▶ Backoff(200ms | 1000ms) ──▶ same primitive, n+1
Writing/Reading ──permanent error or n=3──▶ Resulting(failure)
Reading ──value──▶ Resulting(ok iff value == raw target, else error "io" and no value in the reported result)
Resulting ──emit submit_result──▶ Submitting
Submitting ──submitted(accepted)──▶ Done
Submitting ──submitted(rejected), k<3──▶ Resubmit backoff ──▶ Submitting
Submitting ──rejected after the third resubmission (the 4th rejection) ──▶ Dropped (draft failed, action marked locally completed)
```

- `read_brightness` skips the write and settle and goes straight to `Reading`.
- Asleep, display off (`display_power` off) or wake grace holds `Listed` without marking it completed; it runs at the next `observe` or `tick` once the display is on and the grace has ended.
- Only one primitive executes at a time across monitors; a monitor in `Settling` or `Backoff` does not hold the execution slot.
- A readback mismatch is never retried.
- Permanent errors are `unsupported` and `disconnected`. When an action starts, an unknown monitor fails with `disconnected`, an ambiguous identity with `ambiguous`, and an action of another kind or a set without a value with `unsupported`; the failed action is marked completed. A monitor with no `monitor_profile` runs with the default timing.

### Key sequence (per key)

```
none ──down(first)──▶ Handled | Mute | PassThrough ──repeat──▶ same decision (Handled steps the pending target)
Handled/Mute ──up──▶ none (consume)        PassThrough ──up──▶ none (pass)
Handled ──owner leaves / generation changes──▶ cancelled (repeats consumed, no write)
```

Intents rebase to the readback only when the intent is not scheduled, no action of that feature is pending in the snapshot, no write is outstanding or inside the settle window, the readback moved since the last observed value, and the readback differs from the target by more than 0.51.

## Error handling summary

| Condition | Result |
| --- | --- |
| Malformed JSON, unknown `type`, unknown field, duplicate key, non-finite number, missing or mistyped field | `TV_INVALID_ARGUMENT`, state unchanged |
| Event over 64 KiB | `TV_SIZE_LIMIT`, state unchanged |
| Unknown `monitor_id` on `slider` or `monitor_removed` | Accepted, ignored (`value` null) |
| `key` whose `owner` is null or unknown | Accepted; `value` is `{"consume": false}` |
| `submitted` or `attempt_done` with an unknown token | Accepted, ignored |
| Event after `shutdown` | Accepted, ignored |
| Panic inside a step | `TV_INTERNAL`; the shell stops using the handle, as the camera wrapper does. The current Windows wrapper instead returns an empty reply on any non-OK status and ignores the event. The macOS wrapper (`ControlsBridge.swift`) follows the camera wrapper: a failed step stops the handle. |
