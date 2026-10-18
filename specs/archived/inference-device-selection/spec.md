# Feature Specification: Inference device selection

**Feature Branch**: `inference-device-selection`

## Context and Understanding

Face-presence detection (YuNet through ONNX Runtime) decides when the monitor is dimmed. Today it runs on the **CPU on both platforms**; there is no setting that lets the user choose where it runs.

- **macOS**: the shell always requests the CPU. A CoreML path exists in the inference C ABI but the shell never asks for it, and it has no hardware measurement (`feasibility.md:17`).
- **Windows**: CPU by default. DirectML is used only for device profiles that carry a recorded energy measurement, and the profile file ships empty (ADR-0019), so in practice it is CPU too. The decision is made once per process and cannot be changed from the UI.
- **No NPU route exists** (provider value 3 is reserved and rejected), and nothing in the app lists the available devices or shows which one is active (the active provider is only logged on Windows).

The user wants a setting to choose the inference device, with a Windows preference order of NPU, then iGPU, then dGPU, then CPU, and wants to know what the Mac offers.

Why it matters: the app's priority is minimal CPU, memory, camera and battery use. A tiny, always-on detector is the kind of workload where an NPU or low-power GPU can save energy, but the benefit is hardware-specific and currently unmeasured, so the user needs control and visibility instead of a hard-coded choice.

### What the Mac offers (answer to the user's question)

On Apple Silicon the accelerators are reached only through the ONNX Runtime CoreML execution provider, which exposes a **compute-unit preference**, not a device list:

| Choice | CoreML setting | Notes |
|---|---|---|
| Automatic | `ALL` | CoreML picks among CPU, GPU and Neural Engine. |
| Neural Engine (ANE) | `CPUAndNeuralEngine` | A hint: unsupported operators still run on the CPU, and there is no guarantee the whole model runs on the ANE. |
| GPU | `CPUAndGPU` | Same hint semantics. |
| CPU | `CPUOnly` | Reference behavior, and today's behavior. |

There is no enumeration of individual devices (an Apple Silicon Mac has one GPU and one Neural Engine). CoreML recompiles the model on every session unless a model cache directory is configured. A per-operator dispatch log is available for diagnostics, but the app cannot reliably report which unit actually ran. Intel Macs are deferred by project policy.

### What Windows offers

| Target | Candidate route | Status in this repo |
|---|---|---|
| NPU (Intel / AMD) | OpenVINO (Intel), VitisAI (AMD), via bundled plugin EP or the Windows ML EP catalog | **Out of scope** (owner decision): these routes need vendor runtimes that are not fully open source or need runtime downloads. The reserved NPU provider value stays reserved. |
| iGPU / dGPU | DirectML, selected per adapter | Implemented behind the profile allowlist. Measured on this project's desktop: iGPU about 2x slower than CPU, and the RTX wakes a discrete GPU. Energy not measured. |
| CPU | Default ONNX Runtime CPU provider | Shipped. Always the fallback. |

DirectML is in sustained engineering at Microsoft, with new work moving to Windows ML. Several facts above are not confirmed from documentation (see Notes).

## Feature Description

Add an **Inference device** setting, on both platforms, under **Settings > Advanced**, that lets the user choose where face detection runs. The choice is *Automatic* (the default; the platform's preference order, validated at runtime), a specific detected device, or the CPU. The reason to prefer an accelerator in *Automatic* is **offloading**: running detection on the iGPU (Windows) or the Neural Engine/GPU (Mac) keeps the main CPU and GPU free for the user's other work. Raw speed is irrelevant as long as detection meets the detection cadence. The app shows which devices were detected, which one is currently in use, and tells the user when the chosen device could not be used and the CPU took over.

The user gets control over the energy/latency trade-off on their own hardware without the project having to claim a universal benefit it has not measured.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As a Windows user with an integrated GPU, I want the app to run detection there automatically so that my main CPU and discrete GPU stay free for other work, without my configuring anything.
- **US-002**: As a user, I want to open Settings and see which inference devices were detected on my machine so that I can pick one.
- **US-003**: As a user, I want to force a specific device (or the CPU) when Automatic does not behave well on my hardware, for example a discrete GPU that wakes up.
- **US-004**: As a Mac user, I want to choose between Automatic, Neural Engine, GPU and CPU so that I can use the accelerator I prefer.
- **US-005**: As a user, I want to see which device is actually in use, and be told when my choice failed and the CPU took over, so that I can trust the setting.
- **US-006**: As a user, I want presence detection to keep working if my chosen device disappears or fails (driver update, unplugged eGPU), without a crash or a stuck screen.

### Functional Requirements

- **FR-001**: System MUST provide an "Inference device" setting on macOS and Windows, with equivalent behavior, en-US and pt-BR strings, and tests on each (per AGENTS.md).
- **FR-002**: System MUST offer *Automatic* as the default value and *CPU* as an always-available explicit choice.
- **FR-003**: On Windows, *Automatic* MUST try the iGPU, then the CPU, using the first candidate that is present and passes validation. This becomes the shipped default and amends ADR-0019 (CPU default, DirectML allowlist). The NPU and the discrete GPU are not part of the chain; the dGPU is available only as an explicit manual choice (FR-004).
- **FR-003a**: On macOS, *Automatic* MUST use CoreML with all compute units (CPU, GPU, Neural Engine) when validated on real hardware, and MUST resolve to the CPU until that validation exists.
- **FR-004**: System MUST enumerate the devices available on the machine and list them by human-readable name and type (NPU, integrated GPU, discrete GPU, CPU), never by raw index. A stable identifier, not an index, MUST be what is persisted.
- **FR-005**: On macOS, System MUST offer Automatic, Neural Engine, GPU and CPU, and label the accelerator entries as preferences, since CoreML may run unsupported operators on the CPU.
- **FR-006**: System MUST validate an accelerated route before relying on it: it initializes, its output matches the CPU reference, and its latency fits within the detection cadence. It does NOT need to be faster than the CPU (this replaces the "accelerated must be faster" self-test rule of ADR-0019). On any initialization or runtime failure, System MUST fall back to the CPU for the session and record the failure.
- **FR-007**: System MUST show the device actually in use, and show a clear notice when an explicit choice could not be honored.
- **FR-008**: If the persisted device is no longer present, System MUST behave as Automatic for that run, keep the stored choice, and show the notice from FR-007. It MUST NOT discard the choice silently.
- **FR-009**: A change to the setting MUST take effect immediately by recreating the inference session, without quitting the app. If the switch needs a different ONNX Runtime library (the library path is bound once per process), the UI MUST say that a restart is needed.
- **FR-010**: System MUST NOT open any new network connection for this feature. The About page's update check remains the only network request.
- **FR-010a**: System MUST add no redistributed dependency that is not open source. The only accelerator routes are the ones the operating system already supplies (DirectML on Windows, CoreML on macOS), which the app does not redistribute.
- **FR-010b**: The setting MUST live in the Advanced section of Settings on both platforms, with the list of detected devices, the device in use, and the fallback notice.
- **FR-011**: The setting MUST persist in the settings file with a backward-compatible default so existing installs and older snapshots keep working, and the core must reject invalid values like any other setting.
- **FR-012**: System MUST keep camera and idle behavior unchanged: the setting only selects the execution device and must not raise idle CPU, memory or camera-on time.
- **FR-013**: Any accelerator becoming part of the *Automatic* chain MUST be backed by a recorded hardware validation (correctness and latency within the cadence budget) on a real machine. The docs MUST NOT claim an energy saving, since the justification is offloading, not measured energy.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can change the inference device in at most 3 interactions from the menu bar or window, and the change is reflected in the "in use" readout within one detection cycle (or after a stated restart).
- **SC-002**: With every explicit choice, including a deliberately broken or missing device, presence detection still produces results; the app never ends up with no detector.
- **SC-003**: On a machine with no accelerator, the list contains only CPU, Automatic resolves to CPU, and behavior and resource use are identical to today's.
- **SC-004**: The detection result for the same test frames is identical in presence outcome across all devices that pass validation.
- **SC-005**: Idle CPU and memory with the setting available (and on CPU) do not regress against the current baseline.
- **SC-006**: Behavior is validated and documented on at least one real machine per accelerator class that is enabled in Automatic. Classes without hardware validation are not enabled by default.
- **SC-007**: With Automatic on a machine with an iGPU, detection keeps meeting the detection cadence while the main CPU and discrete GPU are not used by it.

## Decisions

Recorded from the owner on 2026-10-18.

1. **Default policy**: Automatic chain with runtime validation and CPU fallback, plus manual device selection. On Windows the chain is iGPU then CPU; the discrete GPU is manual-only (owner decision, because it can wake from idle and cost power). Raw speed is irrelevant: an accelerator that still meets the cadence is preferred because it frees the main CPU/GPU. This amends ADR-0019 and its "accelerated must be faster than CPU" rule. Same logic on macOS, gated on hardware validation.
2. **NPU**: dropped. The owner wants nothing that is not open source, which rules out the vendor NPU runtimes. Only OS-supplied routes remain (DirectML, CoreML).
3. **macOS options**: Automatic, Neural Engine, GPU, CPU, with the accelerator entries labeled as preferences. Automatic resolves to CPU until validated on a real Mac.
4. **Apply timing**: immediately, by recreating the session; restart notice only when a different runtime library is needed.
5. **Placement**: Settings > Advanced.

## Clarification Needed

None blocking. Open points for `/codeadvisor-dev:design` are listed in Notes.

## Notes

- **Conflicts with current ADRs**: ADR-0019 (CPU default, DirectML only with a recorded measurement, accelerated must be faster than CPU) and ADR-0001 (accelerate only after energy and correctness measurements) conflict with an accelerator-first Automatic. A new ADR amending ADR-0019 and ADR-0001 is needed, stating that the justification is offloading, not energy.
- **dGPU is manual-only**: this project's own measurements show the discrete RTX waking up from idle, so Automatic never selects it. Design should decide whether the rule that the DXGI index is inferred from latency (`api.md:202`) can be replaced by a reliable adapter identity, since the iGPU/dGPU distinction now decides what Automatic may pick. On machines with only a discrete GPU, or where the iGPU cannot be identified reliably, Automatic resolves to the CPU.
- **"Open source" reading**: DirectML and CoreML are operating-system components, not redistributed by the app, which is how ADR-0019 already treats DirectML. The Neural Engine is reached only through CoreML on macOS and is kept. If the owner reads "open source" as also excluding OS-supplied closed components, DirectML and CoreML would have to go too; confirm if in doubt.
- **Hardware limits**: the development machine is a desktop with no battery, so energy cannot be measured here; per FR-013 no energy claim is made. Never infer CoreML or DirectML support on other hardware from code presence.
- **Runtime constraints found in the code (inputs for design)**: ONNX Runtime is pinned at 1.20.1 through `ort =2.0.0-rc.9` with `load-dynamic`; the runtime path is bound once per process; DirectML's DLL is not redistributed; the Windows decision is made once per process; the core `Settings` has no device field yet. The `OrtEpDevice` / EP-selection APIs in newer ONNX Runtime versions are likely needed for NPU routes and may require a runtime upgrade. All of that belongs in `/codeadvisor-dev:design`.
- **Unconfirmed from research (verify before design)**: whether the Windows ML EP catalog works for an unpackaged per-user zip app; the minimum ORT version for the EP-device API; whether the Rust `ort` crate exposes the EP-selection policy; YuNet operator coverage on NPUs; fallback behavior when an EP fails to initialize; NPU vs CPU power for a tiny CNN; OpenVINO redistribution size and licence; whether `MLComputeUnits` is a hard or soft constraint.
- Windows UI naming follows AGENTS.md: the MacBook features are called Laptop/Notebook on Windows only; this feature has no such naming impact.
- Sources used: ONNX Runtime DirectML, OpenVINO, CoreML and plugin-EP pages; Microsoft Learn Windows ML pages; the repo's ADR-0001, ADR-0019, `api.md`, `ARCHITECTURE.md` and archived validation reports.
