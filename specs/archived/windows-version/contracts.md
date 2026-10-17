# Contracts: Windows Version with Laptop Parity and Low-Energy Face Detection

Source of truth for the core side: `api.md`, `include/telaviva_core.h`, `crates/telaviva-core/src/core.rs` and `settings.rs`. This file only defines what the Windows shell must send, how it derives values, and the new Windows-specific contracts. Field names below were confirmed against the core by reading its source summary; re-check against `core.rs` when implementing.

## Data Models

### Monitor discovery payload (shell to core)

```json
{
  "type": "monitor_discovered",
  "monitor": {
    "id": "win-GSM-23456-873459-1f3a9c2b",
    "name": "LG ULTRAFINE",
    "identity_status": "stable",
    "connection_path": "\\\\?\\DISPLAY#GSM5BC0#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}",
    "brightness": { "supported": true, "raw": 41, "min": 0, "max": 100 },
    "volume":     { "supported": true, "raw": 30, "min": 0, "max": 100 }
  }
}
```

Rules:
- `id` = `"win-" + manufacturer(3 letters) + "-" + product(decimal) + "-" + serial(decimal) + "-" + hex8(SHA-256 of the 128-byte EDID base block)`. Length stays under the 512-byte core limit. The connector path never enters the id.
- `identity_status` is `stable` when the id is unique among the attached monitors. A serial that is not usable (`0`, `1`, `0x01010101`, `0xFFFFFFFF`) does not make a lone monitor ambiguous, because many monitors report serial 0 and would otherwise never be controllable (decided during implementation, 2026-10-17; the earlier wording tied `stable` to a usable serial). If two or more attached monitors produce the same id (identical EDID content, typically two identical monitors without a usable serial), all of them are discovered with `ambiguous`; the core then enqueues no hardware action for them.
- `brightness`/`volume`: `supported` is true only if the first VCP read (`0x10` brightness, `0x62` volume) succeeded. `raw` is the current value, `max` the maximum reported by the read, `min` is 0. `value` is omitted (the core prefers `raw`). If the read fails, send `{ "supported": false }`.
- Re-discovery of the same id replaces the monitor and bumps its connection generation; the shell stores the generation from the snapshot and echoes it in later commands.
- Send `monitor_disconnected` with `monitor_id` when a catalog refresh no longer lists an id.

### Core command mapping (Windows source to `tv_submit`)

| Source | Command | Fields and notes |
|--------|---------|------------------|
| Catalog refresh (startup, `WM_DISPLAYCHANGE`, `GUID_DEVINTERFACE_MONITOR` arrival or removal, debounced 500 ms) | `monitor_discovered`, `monitor_disconnected` | Only new ids are rediscovered; removed ids are disconnected |
| `GUID_SESSION_DISPLAY_STATUS` off | `monitor_display_state` for every external monitor | `state: "asleep"`, `generation` = current connection generation |
| `GUID_SESSION_DISPLAY_STATUS` on, or resume (`PBT_APMRESUMEAUTOMATIC`) | `monitor_display_state` for every external monitor | `state: "awake"` after the 2 s DDC grace is armed; the shell does not write before the grace ends |
| Startup before first state | `monitor_display_state` | `state: "unknown"` is not sent; use the real state |
| `GUID_ACDC_POWER_SOURCE` | `power` | `{"on_battery": true}` for DC, `false` for AC, `null` when unknown or no battery |
| Screensaver poll (`SPI_GETSCREENSAVERRUNNING`, every 2 s while enabled; once at startup) | `screensaver` | `"active"`, `"inactive"`; `"unknown"` before the first poll or while the poll is not running |
| Camera pipeline | `presence_observation`, `camera_status` | See the camera status table |
| Laptop panel | `builtin_display` | `{"available": bool, "brightness": 0-100 or null, "calibrating": bool}`; brightness is a whole percent and `null` when unknown; never `0` for a failed read |
| User or hook action | `set_brightness`, `set_volume` | `{"monitor_id","value":0-100,"connection_generation"}` |
| Settings UI | `update_settings` | Full settings subset except `selected_camera_id` and `camera_history`; camera choice is its own `update_settings` with only `selected_camera_id` (or `null` to clear) |
| `pending_actions` executor | `hardware_result` | `{"action_id","generation","ok","value"?,"error"?}`; `generation` is the action generation from the snapshot, not the connection generation |

`monitor_observed` and `shutdown` are not sent by the Windows shell (same as macOS); readbacks go through `hardware_result`, shutdown through `tv_shutdown`.

### Pending-action execution

`pending_actions[]` carries `{id, monitor_id, generation, kind, value, target}`; the list holds at most 8 entries and only the first action per monitor, so the executor re-reads the snapshot after every completion.

| Kind | Executor behavior | `hardware_result` |
|------|------------------|-------------------|
| `set_brightness`, `set_volume` | Round `value` to an integer raw, write, wait the profile delay (default 60 ms), read back; success only if the readback equals the written value | `ok: true` with `value` = readback, or `ok: false` with `error` in `disconnected`, `ambiguous`, `unsupported`, `io` |
| `read_brightness` | Read VCP `0x10` | `ok: true` with `value` = raw readback (required), or `ok: false` with `error` |
| Any action for an `asleep` monitor | Do not touch the hardware | The shell reports `asleep` through `monitor_display_state`; no result is invented |

Timing and retry owned by the shell: 60 ms between transactions per monitor, optional duplicate Set 10 ms apart (profile), read retry delays 0.2 s then 1.0 s, 2 s grace after wake, 10 s outstanding expiry and 1000 ms settle window for key intents. The core bounds restore retries at 3 (`MAX_RESTORE_FAILURES`).

### Camera status mapping

| Windows condition | `presence_observation.status` | `camera_status` |
|-------------------|-------------------------------|-----------------|
| Frame assessed, face found | `present` | none |
| Frame assessed, no face | `absent` | none |
| Mean luma below 18 (core gate) or lifecycle reports dark | `insufficient_light` | none |
| Light-only check, frame usable | `usable_light` | none |
| `MediaCapture.InitializeAsync` returns `E_ACCESSDENIED` (0x80070005) | `unavailable` | `permission` |
| No selectable camera, device removed, or format not delivered as expected | `unavailable` | `unavailable` |
| Acquisition, assessment or frame deadline exceeded, or start failure while the device is present | `unavailable` | `busy` |
| Pixel conversion failure or inference error | `unavailable` | `error` |
| Device add or remove while the last status was `busy` or `unavailable` | send `camera_status` with `"unknown"` | |

`captured_at_ms` is Unix epoch milliseconds from the system clock and must be monotonically increasing and at most 60 s old when submitted. `camera_generation` is copied from `snapshot.camera.generation`. Lifecycle `now` values are `steady_clock` seconds as doubles (the core reads no clock there).

### Frame contract for `tv_inference_detect_bgra`

Top-down BGRA8, `stride >= width*4`, `len >= stride*height`, at most 8192 pixels per side and 256 MiB. The shell requests the smallest adequate format (about 640x480). No rotation or mirroring is applied. Frames are not retained or logged.

### Laptop availability

```text
LaptopUnavailableReason = noBuiltInDisplay | brightnessNotAdjustable | apiUnavailable | displayOffline
```

These reasons exist only in the shell; the core sees `available: bool`. Mapping:

| Condition | `available` | Reason shown |
|-----------|-------------|--------------|
| No internal or embedded DisplayConfig target ever present, or desktop with no panel | false | `noBuiltInDisplay` |
| Internal target known but inactive (lid closed with external monitors), or session display off | false | `displayOffline` |
| Internal target active but no readable `WmiMonitorBrightness` or no valid levels | false | `brightnessNotAdjustable` |
| WMI namespace missing or call fails | false | `apiUnavailable` |
| Active internal panel with a successful brightness read | true | none |

### Key route table (published atomically to the hook thread)

```text
KeyRoute {
  enabled: bool                 // hardware_keys_enabled
  owner_monitor_id: string|null // default render endpoint container id == monitor container id
  connection_generation: u64
  volume_supported: bool
  display_awake: bool
  step: u8                      // volume_key_step, 1..15
  revision: u64
}
```

The hook swallows `VK_VOLUME_UP`, `VK_VOLUME_DOWN`, `VK_VOLUME_MUTE` only when `enabled && owner_monitor_id != null && volume_supported && display_awake`. Otherwise it passes the key to Windows untouched. The decision is latched for the whole key sequence (down, repeats, up). Mute is emulated as `set_volume` to 0 with the previous level remembered and restored on the next press. Brightness keys are never hooked.

## API Contracts

### Inference C ABI additions (`include/telaviva_inference.h`, ABI stays 1)

```c
#define TV_INFERENCE_PROVIDER_CPU 0
#define TV_INFERENCE_PROVIDER_COREML_CANDIDATE 1
#define TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE 2   /* new */
/* 3 reserved for a future NPU provider, not accepted yet */

int32_t tv_inference_directml_available(void);       /* new: 1 if the loaded runtime lists DmlExecutionProvider, else 0 */
```

Behavior:
- `tv_inference_create_with_provider(runtime, model, provider, out)` with provider `2` registers `DmlExecutionProvider` on adapter 0. `tv_inference_create_with_provider_device(runtime, model, provider, device_index, out)` is the same call with a `device_index` (`uint32_t`) after `provider`; `tv_inference_create_with_provider` behaves as device index 0. The shell passes the index of the DXGI adapter (in `IDXGIFactory1::EnumAdapters1` order, as ONNX Runtime sees it) that matched the allowlist profile, so the low-energy iGPU is chosen when its profile matches. `device_index` applies only to provider `2` and is ignored for providers `0` and `1`. If registration fails (including an index the runtime does not have) or the provider is unavailable, it creates a CPU session and `tv_inference_selected_provider(h)` returns `0`. It never returns an error just because the accelerator is missing. The index is the DXGI adapter enumeration index. Its mapping to physical adapters is inferred from latency in `validation/measurements.md` and must be confirmed against the shell's adapter list before any profile is recorded.
- Provider `1` on a non-macOS build behaves as unavailable and falls back to CPU. CoreML code is compiled only for macOS.
- `tv_inference_selected_provider(h)` always reports the provider actually used.
- The runtime path is still bound once per process; a different path returns `-2`.
- Every added constant and function is added to the constants table in `header_contract_tests.rs`, and `tv_inference_directml_available` to the exported-function list test.

### Self-test (shell side, no new ABI)

For an accelerated session, the shell runs one embedded fixture frame (the existing CC0 face fixture bytes compiled into the test and a small shipped copy for runtime) on the accelerated session and on a CPU session. It requires the same result (`present`) and a latency below a stated limit. Any mismatch or error counts as failed.

### Windows runtime manifest (`assets/model/manifest.toml`, additive)

```toml
[runtime.windows-x64.cpu]
version = "1.20.1"
archive_sha256 = "<computed locally from onnxruntime-win-x64-1.20.1.zip>"
dll_sha256     = "<computed locally from onnxruntime.dll>"
license = "MIT"

[runtime.windows-x64.directml]      # present only after a measured profile exists
version = "1.20.1"
package = "Microsoft.ML.OnnxRuntime.DirectML"
package_sha256 = "<computed locally>"
dll_sha256     = "<computed locally>"
directml_dll = "system"             # in-box System32\DirectML.dll, never redistributed
```

The GitHub release assets carry no published digests; the fetch script computes and compares against these pins and fails on mismatch.

## Integration Contracts

### `assets/inference/acceleration-profiles.toml`

```toml
schema = 1

# Empty in the first release. Each entry needs a recorded measurement.
# [[profile]]
# id            = "amd-radeon-igpu-example"
# provider      = "directml"          # directml only in schema 1
# vendor_id     = 0x1002              # PCI vendor id from DXGI_ADAPTER_DESC
# device_ids    = [0x164e]            # empty list means any device of the vendor
# min_driver    = "32.0.21041.1000"   # inclusive
# max_driver    = ""                  # empty means no upper bound
# measurement   = "specs/archived/windows-version/validation/measurements.md#amd-radeon-igpu-example"
# measured_on   = "2026-10-17"
```

Rules: unknown keys are rejected at load (a rejected file means CPU only and a diagnostic line); `provider` other than `directml` is rejected; an entry without a `measurement` reference is rejected. A missing file is reported once at Error level and the app uses CPU only; an invalid file likewise.

### `%LOCALAPPDATA%\TelaVivaOLED\inference-failures.json`

```json
{
  "schema": 1,
  "failures": [
    { "profile": "amd-radeon-igpu-example", "driver": "32.0.21041.1000", "app": "0.1.0", "reason": "self_test_mismatch", "at": "2026-10-17T14:30:00Z" }
  ]
}
```

A profile is skipped while a failure exists for the same (profile, driver, app). Reasons: `register_failed`, `session_failed`, `self_test_mismatch`, `self_test_slow`, `runtime_error`. A corrupt file is renamed to `.corrupt` and treated as empty. The file contains no camera data or device identifiers beyond adapter ids and driver versions.

### `%LOCALAPPDATA%\TelaVivaOLED\calibration-session.json` (shell-owned)

```json
{
  "schema": 1,
  "monitor_id": "win-GSM-23456-873459-1f3a9c2b",
  "adaptive_brightness": {
    "scheme": "381b4222-f694-41f0-9685-ff5bb260df2e",
    "ac": 1,
    "dc": 1,
    "turned_off": true
  },
  "started_at": "2026-10-17T14:30:00Z"
}
```

Written before adaptive brightness is turned off and removed after it is restored. On launch, a present file means the previous run ended during calibration: restore the saved scheme values, then delete the file. If the active scheme changed, restore the saved scheme's values by its GUID. If a restore fails, keep the file and show the same notice used for failed toggles.

### Settings and launch at login

`config_path` = `%LOCALAPPDATA%\TelaVivaOLED\settings.json`. `launch_at_login` is applied first through `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `TelaVivaOLED` = `"<exe path>" --background`, read back to confirm, and only then submitted to the core. A failed write or read-back leaves the setting unchanged and shows an error.

### Windows UI strings

Key names are shared by both `.resw` files and are not sent through the core. Required keys include the Laptop wording: `ambient.unavailable.noBuiltInDisplay`, `ambient.unavailable.displayOffline`, `ambient.unavailable.brightnessNotAdjustable`, `ambient.unavailable.apiUnavailable`, `calibration.slider`, `calibration.help`, `calibration.notice.builtInUnavailable`. English text uses "Laptop" and Portuguese text uses "Notebook". A build test fails if a key is missing in either locale or if any text contains "MacBook" or "Mac".

## State Transitions

### Inference provider selection

```text
Start --[allowlist empty or no match, or failure marker exists]--> CPU
Start --[matching profile, no marker]--> DirectML requested
DirectML requested --[register fails]--> CPU (marker: register_failed)
DirectML requested --[session creation fails]--> CPU (marker: session_failed)
DirectML requested --[self-test mismatch or slow]--> CPU (marker: self_test_mismatch | self_test_slow)
DirectML requested --[self-test ok]--> DirectML active
DirectML active --[inference error]--> CPU for this and later frames (marker: runtime_error), never "absent"
Driver or app version changes --[marker key no longer matches]--> eligible again
```

### Laptop follow

```text
Disabled --[ambient_follow_enabled && available]--> Following
Following --[lid closed | panel off | internal target inactive]--> Paused (displayOffline)
Paused --[lid open and panel on]--> Following (current level applied)
Following --[calibration starts]--> Calibrating (builtin_display.calibrating = true, adaptive brightness off)
Calibrating --[done | cancel | quit | display off]--> Following or Paused (adaptive brightness restored)
Any --[desktop or no controllable panel]--> Disabled with reason
```

### Efficiency mode

```text
Start --[set process EcoQoS once]--> Background (EcoQoS)
Background --[popup or settings visible, not user-managed]--> UI thread normal quality, rest stays EcoQoS
UI thread normal --[window hidden]--> Background
Any --[priority class idle OR process-level state differs from what the app set]--> UserManaged (no thread exemptions, no process-level writes)
Hook thread: normal quality while the hook is installed, except in UserManaged, where no thread is exempted (a user-forced Efficiency mode is never fought; a delayed hook is logged, see the risk table)
```

### Key hook

```text
Not installed --[hardware_keys_enabled]--> Installed
Installed --[route has owner and volume supported and display awake]--> Swallow volume keys
Installed --[otherwise]--> Pass through
Installed --[no hook callback for the watchdog window while keys were pressed]--> Reinstall and log
Installed --[hardware_keys_enabled false or quit]--> Not installed
```
