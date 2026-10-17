# Windows measurements

Measured on 2026-10-17. Machine: Windows 11 Pro build 26300, AMD Ryzen 7 9800X3D (desktop), AMD Radeon iGPU (driver 32.0.21041.1000), NVIDIA GeForce RTX 4080 SUPER (driver 32.0.16.1742). No battery, no NPU.

## S4: DirectML on this machine (T095, T097)

Method: `crates/telaviva-inference/tests/directml_bench_windows.rs`, release build, ONNX Runtime 1.20.1 DirectML package, YuNet model, the CC0 face fixture, 3 warm-up and 50 timed detections per device index. The CPU baseline runs in a second process on the CPU runtime because ONNX Runtime binds one runtime per process.

| Path | selected provider | mean | p95 | status | confidence |
|------|-------------------|------|-----|--------|------------|
| CPU (ORT 1.20.1 CPU package) | 0 | 5.27 ms | 5.69 ms | 1 (face) | 0.822190 |
| DirectML, device index 0 | 2 | 2.58 ms | 3.01 ms | 1 | 0.822190 |
| DirectML, device index 1 | 2 | 10.22 ms | 10.59 ms | 1 | 0.822190 |
| DirectML, device index 2 (does not exist) | 0 (CPU fallback, no error) | 5.26 ms | 5.72 ms | 1 | 0.822190 |

Findings:
- Session creation works on both adapters and the result is bit-identical to CPU on the fixture (same status, same confidence).
- An out-of-range device index falls back to a CPU session without an error, as the contract requires.
- Which index is which GPU is inferred from latency (index 0 looks like the RTX, index 1 the Radeon iGPU). It is not confirmed from the DXGI order yet; the shell's adapter list (`DxgiAdapterCatalog`) must confirm it before any profile maps a device to an index.
- The iGPU path is about twice as slow as CPU on this model and size; the RTX path is faster but wakes a discrete GPU, which is the opposite of the energy goal.
- Energy was **not measured**. This machine is a desktop with no battery or per-component power telemetry that is trustworthy, so no claim of lower energy can be made for any adapter.

In-box DirectML: `C:\Windows\System32\DirectML.dll` reports 1.15.5 (os-germanium) on build 26300 (T097). The minimum build that ships a compatible DirectML for ONNX Runtime 1.20.1 on older Windows 11 releases was not tested here (no 22H2 or 24H2 machine); the DirectML runtime stays an allowlist-only variant for that reason.

## Decision (T098)

`assets/inference/acceleration-profiles.toml` stays empty (`schema = 1`, no profiles). No device and driver has a measurement that shows lower energy with equal results, and the latency data above does not argue for an accelerator at 2 samples per second. The shipped default is CPU. A profile is added only with a measurement reference recorded here.

## Windows ML and NPU (T096)

The NPU route through Windows ML execution-provider packages is out of the first release: this machine has no NPU, the pinned `ort` 2.0.0-rc.9 has no plugin-provider API, and the OS-delivered provider flow cannot be validated here. NPU rows stay "Not run" in `hardware-checklist.md`.

## Not yet measured

- Idle CPU and memory of the full app (A5), EcoQoS on versus off (T104).
- Presence and return latency in forced Efficiency mode (A4, T103).
