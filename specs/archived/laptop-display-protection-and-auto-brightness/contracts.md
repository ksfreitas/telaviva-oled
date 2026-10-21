# Contracts: Laptop display in brightness controls and protection, with automatic brightness off during protection

All JSON, no C ABI change (revision stays 1, `include/telaviva_core.h` untouched). Time, ids and diagnostics follow the existing conventions in `api.md`.

## Data Models

### Reserved display id
`"builtin"` identifies the laptop's built-in screen in `protection_disabled` and `screensaver_targets`. It never appears in `monitors`.

### Setting `builtin_auto_brightness_off_on_protection`
- Type boolean, default `false`, `#[serde(default)]`, schema_version stays 1.
- Accepted by `update_settings` (`"setting must be boolean"` otherwise). Appears in the snapshot `settings` object automatically.
- Example: `{"type":"update_settings","settings":{"builtin_auto_brightness_off_on_protection":true}}`

### Snapshot object `builtin_protection`
```json
{
  "protectable": true,
  "protected": true,
  "available": true,
  "reasons": ["presence"],
  "target_percent": 1.0,
  "hold_auto_brightness": true,
  "holding": true
}
```
- `protectable`: shell opt-in from the last `builtin_display` (false for a shell that never sends it, e.g. Windows today).
- `protected`: `protectable` and `"builtin"` not in `protection_disabled`.
- `available`: the last reported availability (awake, adjustable).
- `reasons`: subset of `"presence"`, `"screensaver"`; empty when not protectable, not protected, unavailable, `calibrating`, or the core is closed.
- `target_percent`: `null` when `reasons` is empty; otherwise the protection level of `"builtin"`: `screensaver_targets["builtin"]` clamped to 1..=50, default 1. Presence and the screensaver give the same value, so reasons never stack.
- `hold_auto_brightness`: `builtin_auto_brightness_off_on_protection && !reasons.is_empty()`.
- `holding`: the last `holding` reported by the shell, forced to `false` unless `protectable` is `true`.

### Shell recovery record `builtin-protection-session.json`
Directory: the existing TelaVivaOLED Application Support folder. Written atomically before any dim write; deleted after a verified restore.
```json
{ "version": 1, "started_at": "2026-10-20T14:03:00Z", "baseline_percent": 62.5 }
```
`baseline_percent` is the unrounded level read just before the dim, in 0..100.

### Automatic-brightness owner record `auto-brightness-hold.json`
Written atomically before turning automatic brightness off; deleted after it is restored (or found already restored).
```json
{ "version": 1, "original_enabled": true }
```
The file exists only while the app has turned automatic brightness off. Holders are in memory only. `calibration-session.json` is unchanged.

### Settings `monitored_display_id` and `monitored_display_history`
- `monitored_display_id`: string or null (null = Automatic and clears the history); a string moves to the front of the history (dedupe, max 8). `monitored_display_history`: read-only, most recent first, front entry equals `monitored_display_id`; validation as for the camera history. Both `#[serde(default)]`; the id may be `"builtin"`.
- Snapshot `monitored_display`: `{"id": "<monitor id>" | "builtin" | null, "automatic": bool}` where `id` is the resolved display and `automatic` is true when the history is empty.
- `monitor_discovered` monitor object gains optional `primary` boolean (default false).

### Setting `presence_min_brightness_by_display`
- Map `{ "<display id>": <percent 0..=90> }`, default empty, `#[serde(default)]`, schema 1. Keys are monitor ids or `"builtin"` (1..512 bytes, at most 128 entries); entries for disconnected displays are kept. `update_settings` replaces the whole map. The effective threshold of a display is its entry, else `presence_min_brightness_percent`.

## API Contracts

### Command `builtin_display` (extended)
Request:
```json
{ "type": "builtin_display", "available": true, "brightness": 62, "calibrating": false,
  "protectable": true, "holding": false }
```
Validation:
- `available` boolean, required; `brightness` number in 0..=100 or null, required; `calibrating` boolean, required (unchanged).
- `protectable` boolean, optional, default false; `holding` boolean, optional, default false; a non-boolean value is `TV_INVALID_ARGUMENT` ("protectable must be boolean" / "holding must be boolean"). Both keys are added to the command allow-list; all fields are validated before any state changes.
- Behavior: `builtin_percent` is updated only when the new `holding` is false and the core has no active reason (freeze rule). `available == false` clears `builtin.misses` and `presence_dimmed`.
Success: the usual new snapshot revision. Error: unchanged envelope.

### Snapshot
`builtin_protection` is added at the top level next to `ambient`. Existing readers ignore it (the Windows codec reads named keys only).

### Diagnostics (drained by `tv_diagnostics_drain`)
- `presence_dim` with id `builtin` (existing line, new id).
- `builtin_protection reasons=<csv> target=<percent|none>` on each change of the reasons set.
- Shell-side lines go to the unified log (`DiagnosticLog.core`): `builtin_dim` (also `unverified` and `dim_failed` forms), `builtin_restore` (also `unverified`), `builtin_restore_failed`, `builtin_recovery`, `auto_brightness_unknown`, `auto_brightness_not_changed`, `auto_brightness_restore_failed`, `auto_brightness_recovered`.

## Integration Contracts

### macOS shell obligations toward the core
1. Report `protectable = true` when a built-in screen exists and its brightness is adjustable (also while the lid is closed); report `available` as today.
2. Report `holding = true` after the recovery record is saved and before the first dim write; report `holding = false` only after a verified restore (or after a recovery restore).
3. Resubmit the last `builtin_display` report whenever `holding` changes.
4. Reconcile on every snapshot, on availability changes, on wake and on screen parameter changes; all backend calls on the main queue.

### Notices (shell UI, not core)
`builtin.notice.autoBrightnessUnknown`, `builtin.notice.autoBrightnessNotChanged`, `builtin.notice.restoreFailed`, `calibration.notice.protectionActive`; each in en-US and pt-BR.

## State Transitions

Shell `BuiltInProtectionController` (one state variable `holding`, plus `baseline`):

```
idle  --[snapshot: reasons non-empty, available, baseline read ok]--> dimming
        (save record, report holding=true, [acquire auto owner if hold_auto], write min(baseline,target))
dimming --[snapshot: target/hold_auto changed]--> dimming   (rewrite level; acquire/release owner)
dimming --[write unverified, third failed attempt of the episode]--> parked
        (no writes or logs until a changed target, option or availability starts a new episode)
dimming --[snapshot: same decision, level verified < 5 s ago]--> dimming   (no readback; a changed decision, availability change or retry verifies at once)
dimming --[snapshot: reasons empty]--> restoring
restoring --[write baseline + readback ok]--> idle
        (release owner, delete record, report holding=false, then republish the report with a freshly read level)
restoring --[failure, attempts < 3]--> restoring   (retry after 1 s)
restoring --[failure, attempts >= 3]--> restoring   (not capped: retried on the next snapshot, availability change or launch; restore is never throttled)
any    --[display unavailable]--> same state, no writes; resumes when available
launch --[record exists]--> restoring   (holding=true with the stored baseline)
quit   --[holding]--> restoreNow() (one synchronous attempt), then idle
```

`BuiltInAutoBrightnessOwner`:

```
free   --[acquire(h), state on]--> held (record saved, auto set off)
free   --[acquire(h), state off or unknown]--> held-noop (no record, nothing to restore; unknown raises a notice)
held   --[acquire(h2)]--> held (holders add)
held   --[release(h), holders remain]--> held
held   --[release(last), read says off or unknown]--> free (auto set on, record deleted)
held   --[release(last), read says on]--> free (record deleted, no write)
held   --[release(last), set on fails]--> owed (record kept; the next acquire joins the owed restore and does not read "off" as the user's choice)
launch --[record exists, read says off or unknown]--> free (set on, record deleted)
launch --[record exists, read says on]--> free (record deleted, no write)
launch --[set on fails]--> owed (record kept for the next launch)
calibration crash recovery (not the owner): session says automatic brightness was on and this run holds nothing --> set on directly (legacy path)
```
