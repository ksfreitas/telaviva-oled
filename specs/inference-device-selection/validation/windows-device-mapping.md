# Windows DirectML device mapping and latency (plan T003)

- Date: 2026-10-18
- Machine: desktop, AMD Ryzen 7 9800X3D (Radeon integrated graphics), NVIDIA GeForce RTX 4080 SUPER, no NPU, no battery
- OS build: Windows 11 10.0.26300.0; system `DirectML.dll` file version 1.15.5+250417-0100.1.os-germanium.7c460ce
- GPU driver versions: AMD Radeon(TM) Graphics 32.0.21041.1000, NVIDIA 32.0.16.1742
- App build / commit: shipped `telaviva_inference.dll` of the 0.1.0 distribution (built with the `directml` feature), ONNX Runtime 1.20.1 DirectML build `onnxruntime.dll` (14,682,160 bytes) from `tools/inference/runtime/windows-x64-directml`, YuNet model, `selftest-face.bgra` (320x240)
- Command or steps: throwaway probe `bench` mode (one process, one runtime for every session: CPU, then DirectML `device_id` 0 and 1; 3 warmup + 50 timed detects each), then `bench --index N --loop-seconds 14` while sampling `\GPU Engine(*)\Utilization Percentage` filtered to the probe's process id, grouped by LUID

## Result

| Provider | device_id | Physical device (by per-LUID utilisation) | create ms | detect median ms | p95 ms | max ms | result |
|---|---|---|---|---|---|---|---|
| CPU | n/a | none (no GPU engine activity) | 118 | 4.95 | 5.95 | 6.11 | present, confidence 0.8905 |
| DirectML | 0 | NVIDIA RTX 4080 SUPER (LUID 0x139AE, max 2.2 %) | 1753 | 2.27 | 3.18 | 4.54 | present, confidence 0.8905 |
| DirectML | 1 | AMD Radeon iGPU (LUID 0x1A6B8, max 13.6 %) | 918 | 9.36 | 10.06 | 11.05 | present, confidence 0.8905 |

- The DXGI `EnumAdapters1` index used as the DirectML `device_id` selects the same physical adapter as the DXGI/DXCore LUID classification on this machine: index 0 is the RTX, index 1 is the iGPU. During the iGPU loop only the Radeon's LUID showed activity; during the RTX loop only the RTX's.
- All three routes return the same presence result and the same confidence on the self-test frame.
- The iGPU takes about 1.9x the CPU's time (9.4 ms vs 4.9 ms median), consistent with the earlier Windows measurement notes. The slowest observed accelerated call was 11 ms.
- Session creation on the iGPU takes about 0.9 s (RTX 1.75 s, cold start), so the first detection after a decision pays that once; the app creates the session lazily and keeps it until idle release.
- **Latency budget decision**: the core accepts a return-sampling interval down to 0.25 s, so the validation budget is half of that, 0.125 s (named constant `kMaxAcceleratedLatencySeconds`). The measured iGPU worst case (11 ms) is about 11x under it. architecture.md and contracts.md said 500 ms based on a 1 s minimum interval; they were corrected to 125 ms.

## Not checked

Energy and power (no power telemetry on a desktop), the iGPU's effect on the main CPU load, other GPUs or drivers, and older Windows 11 builds whose in-box DirectML may be older than 1.15.5. The system DirectML version needed by ONNX Runtime 1.20.1 is not established beyond this machine.
