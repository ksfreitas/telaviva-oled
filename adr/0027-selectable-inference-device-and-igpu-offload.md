# ADR-0027: Selectable inference devices with validated offload and CPU fallback

**Status:** Accepted
**Date:** 2026-10-18
**Feature:** inference-device-selection
**Spec:** [Archived spec](../specs/archived/inference-device-selection/spec.md)

## Context

The owner wants control over the device used for local face detection and visibility when an accelerator fails. The earlier CPU-default energy-measurement gate in [ADR 0001](0001-native-shell-and-rust-core.md) and the Windows profile allowlist in [ADR 0019](0019-windows-inference-cpu-default-directml-allowlist.md) prevented offloading despite usable integrated graphics. The justification is freeing compute resources; energy savings have not been measured.

## Decision

- Persist `inference_device` as an additive schema-1 string, default `auto`. The Rust core validates shape; each shell owns device resolution, runtime status and its native Advanced page, localized in en-US and pt-BR. The core ABI stays at version 1.
- Windows Automatic tries the first integrated hardware GPU identified by DXCore `IsIntegrated` matched to the DXGI catalog by LUID, then CPU. Discrete and unknown-kind GPUs are manual choices only. Stable setting ids encode vendor, device and ordinal, while DirectML indices come from the live catalog. Classification never creates a GPU device.
- Windows ships one DirectML-capable ONNX Runtime 1.20.1 library serving every provider. `DirectML.dll` remains OS-supplied. Retire the acceleration-profile allowlist. Automatic failures use the existing device/driver/app failure markers; explicit choices ignore and never write markers.
- Apple Silicon macOS offers Automatic, Neural Engine preference, GPU preference and CPU. CoreML string `MLComputeUnits` values are `ALL`, `CPUAndNeuralEngine` and `CPUAndGPU`. CPU uses the CPU provider directly. Hardware validation gates enabling CoreML for Automatic.
- macOS pins official ONNX Runtime 1.23.0 with unchanged `ort`/`ort-sys` rc.9 bindings through API 20. The planned 1.22.0 spike exposed an upstream mutex abort during process exit even on CPU-only sessions. The owner authorized the necessary runtime adjustment; 1.23.0 passes session creation, inference and normal exit. The two platforms' runtime pins are independent.
- Every accelerated preference session must register its provider, find the self-test face as the CPU reference does, and detect within the self-test budget of 250 ms set by the amendment below (originally 125 ms). It need not beat CPU latency. Registration failure must report selected provider 0 rather than claim acceleration.
- A changed preference releases the detector and resets validation without requiring a restart. Idle release retains the validated decision. A runtime accelerator error retries the same frame on CPU and retains CPU fallback for the preference session. A missing/foreign-platform id remains stored, follows Automatic and reports `unavailable`.
- The page observes a shell-owned report: requested and active ids, active label, state (`ok`, `auto_cpu`, `unavailable`, `failed`), failure reason and whether lazy creation has occurred. It distinguishes a compute-unit preference from measured physical execution.

## Alternatives Considered

- Keep CPU-only defaults or the energy allowlist: conflicts with the owner's decision to offer validated offload without claiming energy savings.
- Prefer a discrete GPU automatically: can wake it and increase power; keep it manual.
- Classify GPUs by dedicated memory or by creating D3D12 devices: memory heuristics are unreliable, and probing devices may wake a discrete GPU.
- Keep two Windows runtimes and require restart: unnecessary when one delay-loaded DirectML runtime supports CPU as well.
- Keep macOS 1.20.1 or 1.22.0: the former cannot express the selected Neural Engine policy; the latter fails the shutdown spike. A Rust binding upgrade was unnecessary once 1.23.0 passed with API 20.
- Vendor NPU routes: excluded by the open-source redistribution and no-runtime-download constraints. Provider 3 stays reserved.

## Consequences

The shells implement equivalent resolution rules in Swift and C++, with their respective tests. CoreML entries are preferences: unsupported operators can still run on CPU, and the report cannot prove per-operator physical placement. An unavailable or pathological accelerator falls back at runtime even after hardware validation on a different machine. macOS and Windows validation records are preserved with the archived spec; tests and one-machine measurements do not establish camera recall, idle/energy improvements, universal accelerator support or release readiness.

## Non-Obvious Constraints

ONNX Runtime binds its library path once per process, so session changes must keep using the same platform library. The bundled self-test image derives from the shared CC0 portrait and retains its attribution. No app network request is added: the About release check remains the only one. Intel Macs and NPU providers remain out of scope.

## Amendment 2026-10-19: self-test budget

The self-test budget is 250 ms on macOS and Windows instead of 125 ms, by owner decision, to leave headroom for a cold CoreML compile (an isolated test of the bundled ONNX Runtime 1.23.0 measured a 242 ms first run for CPUAndGPU). The budget was briefly set to 500 ms and then settled at 250 ms. It was not the cause of the macOS fallback to the CPU, which came from a validator wiring bug fixed in the same release (a passing self-test was reported as `session_failed`). The budget is a latency check, not a placement check, so a CPU-placed run still passes it. It applies once per preference session, to the first detection rather than to every sample, so it no longer has to fit the 0.25 s minimum return interval. Windows was aligned to the same value for consistency, and no Windows measurement showed a rejection.

## Amendment 2026-10-19: macOS validator wiring

The macOS shell wired the accelerator self-test as `validate: { [weak self] in self?.validate($0) ?? .sessionFailed }`, but `validate` returns nil when the self-test passes. Optional chaining flattens that nil into the same result as a missing owner, so the `?? .sessionFailed` fallback turned every passing self-test into a failure. Since 0.1.1 (commit 666d331), Automatic, Neural Engine and GPU therefore always used the CPU on macOS, and Settings showed "In use: CPU" with reason `session_failed` even though CoreML had created the session. The Swift tests injected fake `validate` functions and never exercised this wiring, and the hardware evidence in the [archived macOS validation record](../specs/archived/inference-device-selection/validation/macos-coreml.md) came from a temporary harness that set `automaticValidated` to true, so the defect was not caught. The fix adds `InferenceDeviceSelection.validate(_:_:using:)`, which decides the missing-owner case before it calls the check, so a pass, a failure and a missing owner stay apart. The regression test `validationHelperKeepsPassFailAndMissingOwnerApart` and a DetectorHost test cover those three cases. The self-test still compares presence only and does not show where CoreML places the work, and the latency budget was not the cause. The corrected build still needs a hardware check on macOS; the fix was verified only by log analysis, an isolated Swift snippet and the regression tests.
