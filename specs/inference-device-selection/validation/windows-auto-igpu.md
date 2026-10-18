# Windows Automatic selects the integrated GPU (plan T023)

- Date: 2026-10-18
- Machine: desktop, AMD Ryzen 7 9800X3D (Radeon integrated graphics), NVIDIA GeForce RTX 4080 SUPER, no NPU, no camera, no battery
- OS build: Windows 11 10.0.26300.0; system `DirectML.dll` 1.15.5
- GPU driver versions: AMD Radeon(TM) Graphics 32.0.21041.1000, NVIDIA 32.0.16.1742
- App build / commit: branch `inference-device-selection` working tree built by `scripts\build-windows.ps1 -NoZip` (685 tests passed); the packaged `onnxruntime.dll` is the DirectML build of ONNX Runtime 1.20.1 (14,682,160 bytes)
- Command or steps: a throwaway program outside the repo links the production `InferenceHost`, `DxgiAdapterCatalog` (DXGI plus DXCore), `DeviceSelection`, `InferenceFailureStore` and the real `telaviva_inference.dll`, loads the packaged `selftest-face.bgra`, and for each preference calls `setPreference`, runs 30 detections and prints the report. Then `--loop <pref> 14` while sampling `\GPU Engine(*)\Utilization Percentage` for that process, grouped by LUID.

## Result

Catalog as production sees it: RTX `gpu:10de:2702:0` kind `discrete`; Radeon `gpu:1002:13c0:0` kind `integrated`; Microsoft Basic Render Driver `software`, no id.

| Preference | Active device | State | Provider | First call ms (session, self-test) | Detect median ms | Result |
|---|---|---|---|---|---|---|
| `auto` | AMD Radeon(TM) Graphics (`gpu:1002:13c0:0`) | ok | 2 | 1009 | 9.4 | present |
| `cpu` | CPU | ok | 0 | 16 | 5.0 | present |
| `gpu:10de:2702:0` (explicit RTX) | NVIDIA GeForce RTX 4080 SUPER | ok | 2 | 659 | 2.2 | present |
| `gpu:1002:13c0:0` (explicit iGPU) | AMD Radeon(TM) Graphics | ok | 2 | 115 | 9.8 | present |
| `gpu:ffff:ffff:0` (not present) | AMD Radeon(TM) Graphics (as Automatic) | **unavailable** | 2 | 117 | 9.5 | present |

- Under `auto`, per-LUID GPU engine utilisation of the probe process showed activity only on the Radeon's LUID (0x1A6B8, peak 13.6 %, 222 detections in 14 s). The RTX showed none. Automatic never picked the discrete GPU.
- Under `cpu`, no GPU engine activity was recorded.
- A stored id that is not present resolves like Automatic and reports `unavailable`, as specified; the stored value is not changed by the host.
- Every route returned the same presence result on the self-test frame, with the self-test (CPU reference, 125 ms budget) passing for the iGPU.
- Detection latency is well inside the 125 ms budget (iGPU worst case seen 11 ms).

## Not checked

- Detection from a real camera through the app: this machine has no camera. The probe feeds the same bundled self-test frame the host uses for validation.
- The Advanced page in the running app: an installed 0.1.0 instance was already running, so a second launch only forwarded to it; the page has not been viewed. Its strings and key parity are covered by the test suite only.
- Failure paths on hardware (a driver that fails registration or mismatches): covered by unit tests with fakes, not provoked on this machine.
- Idle CPU and memory of the app against the previous baseline (SC-005) and the main CPU load while detection runs on the iGPU (SC-007): not measured.
- Energy and power (no telemetry on a desktop). No energy claim is made.
- Other GPUs, hybrid laptops, other drivers and Windows builds.
