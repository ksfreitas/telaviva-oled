# Contracts: Inference device selection

## Data Models

### Setting `inference_device`

- Type: string. Default: `"auto"`. Stored in the settings file; `schema_version` stays 1.
- Accepted shapes (core validation, both platforms):

| Value | Meaning | Platform use |
|---|---|---|
| `auto` | Platform chain, validated | both |
| `cpu` | CPU provider only | both |
| `ane` | CoreML, `CPUAndNeuralEngine` | macOS |
| `gpu` | CoreML, `CPUAndGPU` | macOS |
| `gpu:<vendor>:<device>:<ordinal>` | A specific GPU; `<vendor>` and `<device>` are 4 lowercase hex digits, `<ordinal>` is a decimal number | Windows |

- The core accepts any of the shapes above on any platform (it does not know the platform). A shell treats a value it cannot map to a device as `unavailable` and resolves as `auto`.
- Anything else, a non-string, or a string longer than 32 characters is rejected with an error like the other settings.
- Example: `{"type":"update_settings","settings":{"inference_device":"gpu:1002:164e:0"}}`.
- Older snapshots without the key decode to `auto`; shells must default a missing key.

### Windows adapter record (`AdapterInfo`, additive)

`vendorId`, `deviceId`, `description`, `driverVersion`, `software`, `index` (unchanged) plus `luid` (64-bit) and `kind` (`integrated`, `discrete`, `unknown`). `kind` is `unknown` unless DXCore reports `IsIntegrated` for the adapter matched by LUID.

### Device option (what the page lists)

`id` (the setting value), `label`, `kind` (`auto`, `cpu`, `igpu`, `dgpu`, `gpu`, `ane`). Windows labels GPUs as `<description>` with an integrated or discrete note. The list order is Automatic, then the GPUs in catalog order, then CPU on Windows; Automatic, Neural Engine, GPU, CPU on macOS.

### Inference report (shell-owned, not in the core snapshot)

| Field | Meaning |
|---|---|
| `requestedId` | The stored setting value |
| `activeId` | The id of the device actually attached, or `cpu` |
| `activeLabel` | Human-readable name of that device |
| `state` | `ok`, `auto_cpu`, `unavailable`, `failed` (see transitions) |
| `reason` | For `failed`: `register_failed`, `session_failed`, `self_test_mismatch`, `self_test_slow`, `runtime_error` (same strings as the failure store) |

## API Contracts

### Inference C ABI (additive semantics, signature unchanged)

`int32_t tv_inference_create_with_provider_device(runtime, model, int32_t provider, uint32_t device_index, out)`

| `provider` | `device_index` meaning |
|---|---|
| 0 (CPU) | ignored |
| 1 (CoreML, macOS) | compute units: 0 `ALL`, 1 `CPUAndNeuralEngine`, 2 `CPUAndGPU`; any other value returns `-1` |
| 2 (DirectML, Windows) | adapter index (DXGI order) |
| 3 | reserved, rejected with `-1` |

Success returns 0. `tv_inference_selected_provider(handle)` returns the provider actually attached (0 when the accelerator failed to register and the CPU took over). The creation call registers the accelerator with error-on-failure semantics, so a failed registration is reported as `selected_provider == 0`, never as success. Hard failures keep the existing codes (`-1`, `-2`, `-3`, `-4`, and `3` for a missing file).

### Core `update_settings`

Key `inference_device`: string per the table above. Invalid value: the command fails with `inference_device ...` and the file is untouched. Unknown keys still return `unknown setting`.

## Integration Contracts

### Resolution rules (identical on both platforms; shared test vectors)

Inputs: requested id, live device list, markers (Windows), self-test result.

| Requested | Live state | Result | `state` |
|---|---|---|---|
| `auto` | Windows: an integrated, non-software GPU without a marker, validation passes | that GPU | `ok` |
| `auto` | Windows: no such GPU, only a discrete GPU, `kind` unknown, or marker present | CPU | `auto_cpu` |
| `auto` | Windows: GPU candidate fails to create or validate | CPU, marker written | `auto_cpu` |
| `auto` | macOS, CoreML not yet validated on real hardware | CPU | `auto_cpu` |
| `auto` | macOS, validated | CoreML `ALL` | `ok` |
| `cpu` | any | CPU | `ok` |
| explicit id | device present, validation passes | that device | `ok` |
| explicit id | device present, creation or validation fails | CPU, no marker | `failed` |
| explicit id | device not in the live list | resolve as `auto`, stored value kept | `unavailable` |
| `ane` / `gpu` | macOS, session created and validated | CoreML with those compute units | `ok` |
| any, runtime error during detection | | CPU for the rest of the session; the frame is retried on CPU | `failed` (explicit) or `auto_cpu` with a marker (auto) |

### Validation rules (accelerated candidate)

1. Session created and `selected_provider` equals the requested provider.
2. Self-test frame returns `present` on the candidate and on the CPU reference.
3. Candidate detect latency for that frame is at most 125 ms (named constant, half of the 0.25 s minimum return sampling interval).
4. Speed relative to the CPU is not part of validation.

### Failure markers (Windows)

File `inference-failures.json`, schema 1, unchanged. `profile` holds the device id, key `(device id, driver, app version)`. Written only for Automatic. Cleared when the driver or app version changes. Never consulted for an explicit choice.

### Windows DXCore classification

Create the DXCore adapter factory at runtime (`dxcore.dll`), list hardware adapters, read `IsIntegrated` and `InstanceLuid`, match to the DXGI adapter by LUID. Any failure leaves `kind = unknown`. The classification never creates a D3D12 or DirectML device.

## State Transitions

```
(app start) --stored value, live list--> Resolving
Resolving --candidate ok--> ok
Resolving --auto, no candidate or marker--> auto_cpu
Resolving --explicit device missing--> unavailable (acts as auto)
Resolving --explicit candidate fails--> failed (CPU)
ok --runtime error--> failed (explicit) | auto_cpu + marker (auto)
any --setting changed--> Resolving (session released, no restart)
any --device list changes (hot plug)--> Resolving on the next session creation
```
