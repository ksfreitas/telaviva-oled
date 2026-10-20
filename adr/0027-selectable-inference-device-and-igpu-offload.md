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
- Every accelerated preference session must register its provider, find the self-test face as the CPU reference does, and detect within the self-test budget of 500 ms set by the amendment below (originally 125 ms, then 250 ms). It need not beat CPU latency. Registration failure must report selected provider 0 rather than claim acceleration.
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

The self-test budget is 500 ms on macOS and Windows. It was first raised from 125 ms to 250 ms and then to 500 ms by owner decision, which amends the validation rule in the Decision. On macOS the reason is that CoreML compiles lazily on the first run: a measurement of the bundled ONNX Runtime 1.23.0 showed a first inference of 242 ms for CPUAndGPU, which the 125 ms budget rejected, so the app fell back to the CPU. The 500 ms budget leaves headroom for a cold compile. Windows was aligned to the same value by owner decision for consistency, and no Windows measurement showed a rejection. The budget is a latency check, not a placement check, so a CPU-placed run still passes it. The budget applies once per preference session, to the first detection, not per sample, so it no longer has to fit the 0.25 s minimum return interval. With this budget the first detection of a preference session can take up to 0.5 s before the app falls back to CPU.
