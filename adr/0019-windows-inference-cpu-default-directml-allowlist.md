# ADR-0019: Windows inference: CPU by default, DirectML only for measured device profiles with a failure marker

**Status:** Accepted; default/runtime/allowlist policy amended by [ADR 0027](0027-selectable-inference-device-and-igpu-offload.md)
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

[ADR 0001](0001-native-shell-and-rust-core.md) uses CPU as the default inference provider and enables an accelerator only after measurements. The Windows spec requires a measured energy benefit before an accelerator is enabled (Q2). The `ort` rc.9 crate reports registration failure only from `register()`, and its `build()` cannot fail.

## Decision

- The shipped ONNX Runtime is the CPU build, 1.20.1, pinned by SHA-256.
- `assets/inference/acceleration-profiles.toml` (schema 1) ships empty. A DirectML request is made only for a device that matches an entry: PCI vendor id, device id list, driver range, and a `measurement` reference. A rejected file means CPU only.
- The DirectML runtime variant (`onnxruntime.dll` from the DirectML package) is a reserved, separate variant and is not shipped by default. `DirectML.dll` is never redistributed; the OS copy in `System32` is used.
- The C ABI adds provider value `2` (`TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE`), `tv_inference_directml_available()`, and `tv_inference_create_with_provider_device(..., device_index, ...)`. `device_index` is the position in `IDXGIFactory1::EnumAdapters1` order. An index that fails registers as CPU, and `tv_inference_selected_provider` reports `0`. Provider value `3` is reserved for NPU.
- At startup the shell runs an embedded frame on the accelerated session and on CPU, with CPU fallback disabled in the accelerated session. The result must be `present` on both, with latency under a limit. Any error counts as failure.
- Failures are written to `inference-failures.json`, keyed by (profile, driver, app), with reasons `register_failed`, `session_failed`, `self_test_mismatch`, `self_test_slow` and `runtime_error`. A failed key is skipped until the driver or app version changes. A runtime error falls back to CPU for that and later frames, and never reports "absent".

## Alternatives Considered

- **Ship the DirectML runtime always.** A failure path with no proven benefit.
- **Runtime heuristics ("use the GPU if present").** Rejected by Q2.
- **Upgrade to `ort` rc.14.** Needs a newer Rust toolchain and has API churn. NPU plugin providers are deferred.
- **Windows ML NPU providers.** Not documented for non-WinRT Rust, and there is no NPU on the development machine.

## Consequences

- The first release runs CPU inference everywhere and loads no DirectML code.
- Each enabled profile needs a measurement record.
- Evidence, from [measurements](../specs/archived/windows-version/validation/measurements.md) (spike S4):
  - CPU mean 5.27 ms (p95 5.69 ms).
  - DirectML device index 0: 2.58 ms. Index 1: 10.22 ms. Index 2 (absent) fell back to CPU with no error.
  - Results were bit-identical to CPU on the fixture. Energy was not measured. The RTX path is faster but wakes a discrete GPU.
  - The profiles file stays empty.
  - In-box `DirectML.dll` reports 1.15.5 on build 26300. Its minimum compatible build on older Windows 11 releases was not tested, which is why the DirectML runtime stays allowlist-only.
  - [Hardware checklist](../specs/archived/windows-version/validation/hardware-checklist.md): A1 Pass; A2 Partial (energy not measured); A3 (NPU) not run.
- Known limitation: the DirectML index-to-adapter mapping is inferred, not confirmed. Index 0 looks like the RTX and index 1 like the Radeon iGPU, from latency only. The shell must confirm the mapping against the DXGI order before any profile maps a device to an index.

## Non-Obvious Constraints

- ONNX Runtime binds one runtime per process, so the runtime variant is chosen before the first detector is created (see [architecture](../specs/archived/windows-version/architecture.md), Technical Constraints).
- Do not add a profile entry without its `measurement` reference recorded in the validation notes.

## Amendment 2026-10-18: Validated device selection

[ADR 0027](0027-selectable-inference-device-and-igpu-offload.md) replaces the CPU-default profile allowlist and separate runtime variants with Automatic (integrated GPU, then CPU), manual GPU choices, one DirectML-capable runtime and a 125 ms validation budget. Failure markers apply to Automatic only. The live DXGI/DirectML index mapping was confirmed on the development desktop by per-LUID utilization; this does not establish universal hardware compatibility. NPU remains reserved and DirectML.dll remains OS-supplied.
