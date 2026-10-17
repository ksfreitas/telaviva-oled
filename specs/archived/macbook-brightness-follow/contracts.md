# Contracts: MacBook Brightness Follow

> Superseded in part: the calibration direction was inverted (points are monitor levels, the slider sets the MacBook). See `specs/archived/invert-macbook-calibration-direction/spec.md`.

All contracts extend the existing JSON interface of `telaviva-core` (`tv_submit`, `tv_snapshot`) and `settings.json`. No C ABI function changes. `api.md` is updated to match when implemented.

## Data Models

### AmbientCurve — one monitor's calibration

Percent values are the monitor's normalized brightness (0 = its minimum, 100 = its maximum) judged equal to the MacBook at 25, 50, 75 and 100 percent.

| Field | Type | Rule |
|-------|------|------|
| `p25` | number or null | finite, 0–100 |
| `p50` | number or null | finite, 0–100 |
| `p75` | number or null | finite, 0–100 |
| `p100` | number or null | finite, 0–100 |

- Present values MUST be non-decreasing in point order (`p25 <= p50 <= p75 <= p100`, ignoring nulls).
- A curve is *complete* when all four are non-null. Only complete curves can follow.

```json
{ "p25": 18.0, "p50": 40.0, "p75": 66.0, "p100": 100.0 }
```

### Settings additions (schema stays 1)

| Field | Type | Default | Validation |
|-------|------|---------|------------|
| `ambient_follow_enabled` | bool | `false` | none |
| `ambient_curves` | map monitor ID → `AmbientCurve` | `{}` | key 1–512 bytes; at most 128 entries; curve rule above |
| `protection_disabled` | array of monitor IDs (a set, sorted, unique) | `[]` | each 1–512 bytes; at most 128 entries |

- `update_settings` replaces `ambient_curves` and `protection_disabled` whole when present (same rule as `screensaver_targets`); omitted keys are untouched. Unknown keys remain an error.
- Absence of an ID in `protection_disabled` means protection is on for that monitor.
- Entries for monitors that are not connected are kept.

```json
{
  "type": "update_settings",
  "settings": {
    "ambient_follow_enabled": true,
    "ambient_curves": { "<monitor-id>": { "p25": 18, "p50": 40, "p75": 66, "p100": 100 } },
    "protection_disabled": ["<monitor-id>"]
  }
}
```

Compatibility: a build without these fields reading a file that has them treats it as corrupt (`deny_unknown_fields`), renames it to `settings.json.corrupt` and starts from defaults. Newer files read by newer builds and older files read by newer builds are fine.

### Calibration session file (shell-owned, not read by the core)

`Application Support/TelaVivaOLED/calibration-session.json`, written atomically (temp file and rename) when a calibration session starts, deleted when it ends.

```json
{
  "version": 1,
  "started_at": "2026-10-16T12:00:00Z",
  "builtin_level_percent": 44.3,
  "auto_brightness_was_enabled": true,
  "monitor_id": "<monitor-id>",
  "monitor_brightness_percent": 62.0
}
```

At launch, if the file exists the shell restores built-in level, then automatic brightness (only when `auto_brightness_was_enabled` is true), then the monitor brightness if that monitor is connected, then deletes the file. An unreadable file is deleted and the failure reported in the tab.

## API Contracts

### Command `builtin_display`

Reports the shell's view of the built-in display. Runtime state only, not persisted.

Request:

```json
{ "type": "builtin_display", "available": true, "brightness": 44.0, "calibrating": false }
```

| Field | Type | Rule |
|-------|------|------|
| `available` | bool, required | built-in display present, online, awake or recoverable, and brightness readable and settable |
| `brightness` | number or null | percent, finite, 0–100; null when unknown (the core keeps no stale value as current: follow targets are computed only from a known level) |
| `calibrating` | bool, required | true during a calibration session |

Success: snapshot with an incremented revision. Errors (same style as other commands): unknown key, wrong type, or `brightness` outside 0–100 or not finite → command error, state unchanged.

Behavior:

1. Follow is *active* iff `ambient_follow_enabled && available`. It is *applying* iff active, `brightness` is not null and `calibrating` is false.
2. For each monitor that is eligible (brightness feature supported, stable identity, complete curve in `ambient_curves[monitor.id]`), when applying: `follow_raw = round(normalized_to_raw(curve(brightness)))`, clamped to the monitor's raw range.
3. `curve(x)`: piecewise linear through (0, 0), (25, p25), (50, p50), (75, p75), (100, p100); `x` below 0 or above 100 is clamped.
4. Not applying: `follow_raw` is cleared and the monitor keeps its current baseline (no writes are issued by follow).
5. `follow_raw` set: `baseline = follow_raw`; `manual_override` is not changed; a write is enqueued only if the monitor is awake, not dimmed by a protection reason, not already within one raw unit of `follow_raw` (observed or desired), and the queue accepts it.

### Snapshot additions

Top-level `ambient`:

```json
"ambient": { "enabled": true, "available": true, "active": true, "calibrating": false, "builtin_percent": 44 }
```

`active` = `enabled && available`. `builtin_percent` is null when unknown.

Per connected monitor:

```json
{ "id": "…", "follow": "eligible", "protected": true }
```

| `follow` value | Meaning |
|----------------|---------|
| `eligible` | supported, stable identity, complete curve |
| `uncalibrated` | supported and stable, curve missing or incomplete |
| `unsupported` | no DDC brightness |
| `unstable` | identity is ambiguous, writes are blocked |

`follow` describes eligibility regardless of the feature being on. The popup hides the brightness slider iff `ambient.active && follow == "eligible"`. `protected` is false when the ID is in `protection_disabled`.

### Existing commands, changed semantics

- `set_brightness`: unchanged. Used by the calibration slider; it sets `manual_override` and baseline as today. Follow is paused (`calibrating: true`) while it is in use.
- `monitor_discovered`: also recomputes `follow_raw` for the new monitor.
- `update_settings`: after a change to `ambient_follow_enabled`, `ambient_curves` or `protection_disabled`, follow targets and protection reasons are recomputed. A monitor newly protection-disabled gets its `presence_dimmed`, miss counter and screensaver dim cleared and restored through the existing restore path.

## Integration Contracts

### DisplayServices (private, loaded at runtime)

| Function | Signature (from open-source usage, SDK export verified) | Use |
|----------|--------------------------------------------------------|-----|
| `DisplayServicesCanChangeBrightness` | `(CGDirectDisplayID) -> Bool` | availability gate |
| `DisplayServicesGetBrightness` | `(CGDirectDisplayID, UnsafeMutablePointer<Float>) -> Int32` | read; 0 on success, level 0.0–1.0 |
| `DisplayServicesSetBrightness` | `(CGDirectDisplayID, Float) -> Int32` | write; unverified on hardware until Phase 0 |

Any missing symbol, a nonzero status, or a display other than the one `CGDisplayIsBuiltin` selected means "unavailable". On the development Mac an external display returned status 1000 for a read.

### CoreBrightness automatic brightness (private, Objective-C runtime)

`CBClient` (`activateWithError:`) → `newDisplayClientForID:withError:` → `brightnessClient` (`CBDisplayBrightnessClient`): `isAutobrightnessEnabledWithError:` (BOOL), `setAutoBrightnessEnabled:error:` (BOOL, `NSError**`). The getter was verified on a MacBook Pro; the setter is unverified. A missing class or selector, an error or `NO` means "cannot toggle", which shows the calibration notice and never blocks calibration.

### Hardware key router

`setFollowedBrightnessDisplays(_ ids: Set<String>)`: IDs of monitors with `follow == "eligible"` while `ambient.active`. A brightness key whose target monitor is in the set is passed through unchanged. Empty set restores current routing.

## State Transitions

Follow (per monitor):

```
Idle        --[enabled && available && eligible && level known && !calibrating]--> Following
Following   --[level change >= 1 raw unit]--> Following (baseline = new target; write if awake and not dimmed)
Following   --[presence or screensaver dim]--> Following (baseline only, dim level kept)
Following   --[protection ends]--> Following (existing restore writes baseline, which is the mapped value)
Following   --[calibrating:true]--> Idle (no follow writes)
Following   --[available:false or enabled:false]--> Idle (monitor keeps last value, slider returns)
Idle        --[calibrating:false and conditions hold]--> Following
```

Calibration session (shell):

```
Closed      --[select first point]--> Active (state saved, session file written, calibrating:true, auto brightness off if possible)
Active      --[select another point]--> Active (built-in set to point, monitor slider shown)
Active      --[confirm]--> Active (curve point saved via update_settings)
Active      --[done, cancel, tab left, window closed, quit, display sleep, built-in unavailable]--> Closed (restore built-in level, auto brightness if it was on, monitor brightness; calibrating:false; file deleted)
Closed      --[launch with leftover session file]--> Closed (recovered by the same restore)
```

Protection participation (per monitor):

```
Protected   --[ID added to protection_disabled]--> Unprotected (presence/saver dims cleared, restored via existing path)
Unprotected --[ID removed]--> Protected (no immediate dim; next evaluation applies normally)
```
