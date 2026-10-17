# Windows version: ADR candidates

**Feature**: Windows version (T141)
**Date**: 2026-10-17
**Use**: input for `/codeadvisor-dev:finalize`, which turns each entry into a permanent record in `adr/`. The next free number is 0016. Titles below are proposals. Each entry lists the evidence that supports it; an entry marked "not run" has no validation yet.

Status labels in the evidence are those recorded in [validation/hardware-checklist.md](validation/hardware-checklist.md). Only rows marked Pass are verified.

---

## 1. Windows shell and platform split

**Proposed title**: ADR 0016, Windows shell: C++/WinRT WinUI 3, unpackaged and self-contained, with a UI-free platform library

**Context**: [ADR 0001](../../adr/0001-native-shell-and-rust-core.md) chooses WinUI 3 for Windows. Platform logic must be testable without XAML and must run `--smoke-test` before any XAML initialization.

**Decision**: One process with two native projects.
- `native/windows/Platform` is a static library with no XAML dependency. It talks to the core only through `CoreBridge` and to the OS only through interfaces (`IDdcPort`, `IDisplayCatalog`, `IPowerEvents`, `IAudioOwner`, `ICameraBackend`, `ILaptopPanel`, `IKeyHook`, `IClock`, `IEfficiency`) that tests replace with fakes.
- `native/windows/TelaViva` is the C++/WinRT WinUI 3 app: tray host, flyout, Settings and calibration dialog.
- The app is unpackaged on Windows App SDK 2.5.1, self-contained, with the hybrid CRT (`native/windows/HybridCRT.props`), and declares per-monitor DPI v2.
- A named mutex keeps one instance.

**Consequences**: Tests run as plain executables (`tests.exe`) without XAML. The self-contained runtime makes the package large (the 2026-10-17 assembly was 303 files and 116.7 MB before the unused-component step). The Windows App SDK license question in section 10 applies to this choice.

**Alternatives**:
- Win32 or Direct2D-only shell: lower idle cost, much more UI code. Fallback only if S10 misses the budget.
- Rust-native shell: breaks ADR 0001.
- Framework-dependent Windows App SDK with the runtime installer chained: installer redistribution terms were not confirmed.

**Evidence**:
- [architecture.md](architecture.md) Decisions 1, 12, 14.
- [validation/foundation.md](validation/foundation.md): native build with 0 warnings and 0 errors, `tests.exe` 50 of 50, `--smoke-test` exit 0, `--exit-immediately` in about 260 ms.
- [validation/spike-s10-idle.md](validation/spike-s10-idle.md): provisional go; 0.078% of one core and 57.6 MB working set for the tray-only shell over 60 s. Longer window and full-app measurement are open.
- [validation/spike-s9-resources.md](validation/spike-s9-resources.md): unpackaged `.resw` loading works.

---

## 2. Two Rust cdylibs

**Proposed title**: ADR 0017, Windows Rust core and inference as two cdylibs with a C ABI

**Context**: Linking two Rust staticlibs into one MSVC binary commonly collides on duplicated standard library symbols. A Rust CRT inside the hybrid-CRT shell is also a risk.

**Decision**: Build `telaviva_core.dll` and `telaviva_inference.dll` with `cargo rustc --lib --crate-type cdylib` (`tools/windows/build-rust-dlls.ps1`). The shell links their import libraries. The core crate manifest is unchanged. Both DLLs keep ABI version 1.

**Consequences**: Two copies of the standard library and two DLLs to ship. Every export change must pass `tools/windows/check-dll-exports.ps1` against the headers. The DLL boundary keeps the Rust and shell CRTs separate, but S1 does not test behavior under the shell's hybrid CRT.

**Alternatives**:
- One combined crate re-exporting both ABIs: exports from dependency rlibs are unreliable on MSVC.
- Two staticlibs: duplicate symbols.

**Evidence**:
- [validation/spike-s1-exports.md](validation/spike-s1-exports.md): `telaviva_core.dll` 10 of 10 exports and `telaviva_inference.dll` 7 of 7 match the headers; 0 mismatches.
- [validation/foundation.md](validation/foundation.md): the DLL export check and the core snapshot round trip.

---

## 3. Monitor identity rule

**Proposed title**: ADR 0018, Windows monitor identity from EDID without the connector path

**Context**: The macOS id embeds the connector location, so per-monitor curves are orphaned on a port change. Guessing between identical monitors could write to the wrong panel.

**Decision**:
- `id = "win-" + manufacturer(3 letters) + "-" + product + "-" + serial + "-" + hex8`, where `hex8` is the first eight hex digits of the SHA-256 of the 128-byte EDID base block.
- The connector path goes to `connection_path` only and never enters the id.
- `identity_status` is `stable` when the id is unique among attached monitors. A serial that is not usable (`0`, `1`, `0x01010101`, `0xFFFFFFFF`) does not make a lone monitor ambiguous.
- If two or more attached monitors produce the same id, all of them are discovered as `ambiguous`. The core enqueues no hardware action for them, and the UI shows them read-only with a reason.

**Note for finalize**: [architecture.md](architecture.md) Decision 5 gives an earlier rule (stable only with a usable serial). [contracts.md](contracts.md) records the rule implemented on 2026-10-17. The ADR must use the contracts rule.

**Consequences**: Curves, `protection_disabled` and `screensaver_targets` survive docks and port changes. Two identical monitors without a usable serial cannot be controlled in the first release (known limitation). A monitor whose EDID is rewritten gets a new id.

**Alternatives**:
- Path-based ids: orphaned on every port change.
- Ordinal disambiguation: can swap monitors silently.

**Evidence**:
- [validation/spike-s2-identity.md](validation/spike-s2-identity.md): D1 Pass for one DELL S3225QC over HDMI, `identity_status` stable.
- Duplicate-id behavior is covered by unit tests only. Hardware row D9 (two identical, dock, DisplayPort, USB-C) is Not run.

---

## 4. Provider allowlist and CPU default

**Proposed title**: ADR 0019, Windows inference: CPU by default, DirectML only for measured device profiles with a failure marker

**Context**: Q2 requires a measured energy benefit before an accelerator is enabled. `ort` rc.9 reports registration failure only from `register()`, and its `build()` cannot fail.

**Decision**:
- The shipped ONNX Runtime is the CPU build, 1.20.1, pinned by SHA-256.
- `assets/inference/acceleration-profiles.toml` (schema 1) ships empty. A DirectML request is made only for a device that matches an entry: PCI vendor id, device id list, driver range, and a `measurement` reference. A rejected file means CPU only.
- The DirectML runtime variant (`onnxruntime.dll` from the DirectML package) is a reserved, separate variant, not shipped by default. `DirectML.dll` is never redistributed; the OS copy in `System32` is used.
- The C ABI adds provider value `2` (`TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE`), `tv_inference_directml_available()`, and `tv_inference_create_with_provider_device(..., device_index, ...)`. `device_index` is the position in `IDXGIFactory1::EnumAdapters1` order. An index that fails registers as CPU, and `tv_inference_selected_provider` reports `0`. Provider value `3` is reserved for NPU.
- At startup the shell runs an embedded frame on the accelerated session and on CPU, with CPU fallback disabled in the accelerated session. The result must be `present` on both, with latency under a limit. Any error counts as failure.
- Failures are written to `inference-failures.json`, keyed by (profile, driver, app), with reasons `register_failed`, `session_failed`, `self_test_mismatch`, `self_test_slow` and `runtime_error`. A failed key is skipped until the driver or app version changes. A runtime error falls back to CPU for that and later frames and never reports "absent".

**Consequences**: The first release runs CPU inference everywhere and loads no DirectML code. Each enabled profile needs a measurement record. The device-index mapping is unconfirmed (limitation 5).

**Alternatives**:
- Ship the DirectML runtime always: a failure path with no proven benefit.
- Runtime heuristics ("use the GPU if present"): rejected by Q2.
- Upgrade to `ort` rc.14: needs a newer Rust toolchain and has API churn; NPU plugin providers are deferred.
- Windows ML NPU providers: not documented for non-WinRT Rust; no NPU on the development machine.

**Evidence**:
- [validation/measurements.md](validation/measurements.md), S4: CPU 5.27 ms mean; DirectML index 0 2.58 ms; index 1 10.22 ms; index 2 (absent) falls back to CPU with no error. Results are bit-identical to CPU on the fixture. Energy not measured. Decision T098: profiles stay empty.
- [validation/hardware-checklist.md](validation/hardware-checklist.md): A1 Pass; A2 Partial (energy not measured); A3 NPU Not run.
- [contracts.md](contracts.md): inference provider selection, failure reasons, self-test.

---

## 5. Efficiency policy

**Proposed title**: ADR 0020, Windows efficiency: EcoQoS set once, UI exemption only while a window is visible, user-forced Efficiency mode respected

**Context**: FR-025 to FR-028 and Q7 require minimal background cost and respect for a user-forced Efficiency mode, without raising the timer resolution.

**Decision**:
- At startup the process requests EcoQoS once (`ProcessPowerThrottling`, `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`). The app never calls `timeBeginPeriod`.
- While the popup or Settings window is visible, only the UI thread requests normal quality (thread level). It returns to EcoQoS when the window is hidden.
- The process-level state is never reasserted after startup.
- If the priority class is idle, or the process-level state differs from what the app set (the user forced Efficiency mode), the controller enters UserManaged: no thread exemptions, no process-level writes, and a diagnostic line.
- Timers use thread-pool timers with a tolerable delay of 10% of the delay, at most 1 s.
- **Amended hook-thread rule**: [architecture.md](architecture.md) Decision 11 says the hook thread always requests normal quality. [contracts.md](contracts.md) amends this: the hook thread requests normal quality while the hook is installed, except in UserManaged, where no thread is exempted and a delayed hook is logged. Record the amended rule only.

**Consequences**: Behavior under a forced Efficiency mode is unmeasured. Under UserManaged the keyboard hook may be delayed, and the watchdog and logging are the only mitigation.

**Alternatives**:
- No EcoQoS: higher idle cost.
- Raising timer resolution: violates FR-027.
- Always normal quality: fights the user's choice.

**Evidence**:
- `native/windows/Tests/efficiency_policy.cpp`: policy tests with a fake `IEfficiency` (T099 done). These are unit evidence only.
- T103 (S6 and S8) and T104 (EcoQoS on versus off): Not run. [validation/measurements.md](validation/measurements.md) lists both as not yet measured.

---

## 6. Key hook design (volume and mute only)

**Proposed title**: ADR 0021, Windows volume and mute keys through a low-level hook, a lock-free route and container-id ownership

**Context**: `WH_KEYBOARD_LL` is the only documented way to consume a volume key. A slow callback silently removes the hook. Brightness keys keep Windows behavior (Q5).

**Decision**:
- A dedicated hook thread with its own message loop installs `WH_KEYBOARD_LL` for `VK_VOLUME_UP`, `VK_VOLUME_DOWN` and `VK_VOLUME_MUTE` only.
- The callback reads an atomically published `KeyRoute` (`enabled`, `owner_monitor_id`, `connection_generation`, `volume_supported`, `display_awake`, `step`, `revision`). It swallows a key only when `enabled && owner != null && volume_supported && display_awake`. It posts the intent to the bridge queue and never calls the core.
- The decision is latched for the whole key sequence (down, repeats, up).
- Ownership: the default render endpoint (`eConsole`) container id must equal a monitor's PnP container id. No match or no id means the key passes through. There is no name match.
- Mute is emulated as `set_volume` to 0, with the previous level remembered.
- A watchdog runs only while a key is held. If no hook callback arrives within `kKeyHookWatchdogWindowMs` (1500 ms, `Platform/KeyHook.h`), the hook is reinstalled and the event is logged.
- The step, rebase guard and per-key latch are a C++ port of `HardwareKeyRouter.swift` (`KeyIntentRouter`).

**Consequences**: Windows does not show its own volume OSD for a swallowed key. Router logic is duplicated in Swift and C++, kept aligned by ported test vectors. The container-id match is inferred from Windows device information, not documented.

**Alternatives**:
- Raw Input: observe only, cannot swallow.
- `RegisterHotKey`: volume-key behavior not documented.

**Evidence**:
- [validation/spike-s2b-audio.md](validation/spike-s2b-audio.md): NVIDIA HDMI endpoint has the same container id as the DELL monitor; on this machine the default output is the headset, so keys pass through (K3 expected). AMD and DisplayPort/USB-C paths not exercised.
- `native/windows/Tests/key_route.cpp`: watchdog and route tests.
- K1 to K4 in the hardware checklist: Not run.

---

## 7. Camera backend

**Proposed title**: ADR 0022, Windows presence camera: shared read-only MediaCapture, infrared sources excluded

**Context**: Presence must not interfere with video calls, and infrared cameras must not be used for presence without validation (Q8).

**Decision**:
- A shared, read-only `MediaCapture` with `MediaFrameReader` in CPU memory, using the smallest adequate format (about 640x480, about 2 fps, single-frame checks in sampled mode), converted to top-down BGRA8 for `tv_inference_detect_bgra`.
- Enumeration uses `MediaFrameSourceGroup` and `DeviceWatcher`. Sources with `MediaFrameSourceKind.Infrared` are excluded from selection and listing. There is no unhide control in the first release.
- The camera id is the symbolic link. Device identifiers are not logged.
- Errors map to core statuses as in [contracts.md](contracts.md): `E_ACCESSDENIED` to `permission`, device absent to `unavailable`, start failure with the device present to `busy`, other errors to `error`.
- The lifecycle runs through `tv_camera_lifecycle_step` with `steady_clock` seconds.
- Fallback: a Media Foundation `IMFSourceReader` behind the same `ICameraBackend`, if the unpackaged `MediaCapture` path fails.

**Consequences**: Windows Studio Effects may alter frames on supported hardware; this is a validation row, not a design input. "In use by another app" cannot be separated from other start failures, so `busy` is shown as "could not start".

**Alternatives**: Media Foundation `IMFSourceReader` as the primary path (harder IR classification and enumeration).

**Evidence**:
- No S3 validation note exists yet. The planned spike (unpackaged `MediaCapture` on RGB and IR cameras) is open.
- `native/windows/Tests/camera_lifecycle_driver.cpp`: lifecycle driver tests with a fake clock.
- P2 (shared with a video call), P3 (RGB and IR, IR hidden) and P1 (permission denied): Not run.

---

## 8. Laptop naming rule

**Proposed title**: ADR 0023, Windows UI says "Laptop" and "Notebook"; core identifiers unchanged

**Context**: macOS copy says "MacBook". A Windows panel is not a MacBook, and Windows copy must not name a Mac.

**Decision**:
- In Windows resources only, English text says "Laptop" and Portuguese (pt-BR) text says "Notebook".
- No Windows resource contains "MacBook" or "Mac". `native/windows/Tests/strings_keys.cpp` checks this, along with key parity between `en-US` and `pt-BR`.
- Core identifiers and ABI names are unchanged (for example `builtin_display`, `noBuiltInDisplay`).
- The rule covers UI text only. The core and the macOS shell keep "MacBook".

**Consequences**: The same concept has different text on each platform. Shared keys keep the same names across locales.

**Alternatives**:
- "MacBook" everywhere: wrong on Windows.
- Renaming core identifiers: ABI churn with no user benefit.

**Evidence**:
- Spec Q10 and FR-017, SC-009 in [spec.md](spec.md).
- [validation/spike-s9-resources.md](validation/spike-s9-resources.md): `strings_keys` passes; the check covers both locales and the "Mac" rule.
- L1 to L3 (laptop rows): Not run.

---

## 9. Installer and packaging

**Proposed title**: ADR 0024, Windows distribution: unpackaged zip with a per-user PowerShell installer and HKCU Run for launch at login

**Context**: Q6 rejects MSIX. Inno Setup is not OSI-licensed and requests a commercial license; WiX v6 charges a maintenance fee for revenue use. Both conflict with the open-source rule.

**Decision**:
- `packaging/windows/build-app.ps1` produces a self-contained, hybrid-CRT app folder with the two Rust DLLs, the CPU `onnxruntime.dll`, the model and manifest, the self-test frame, `acceleration-profiles.toml`, licenses and `VERSION`, and the zip `TelaVivaOLED-<version>-win-x64.zip`.
- The zip also contains `Install-TelaVivaOLED.ps1` and `Uninstall-TelaVivaOLED.ps1`. The installer copies to `%LOCALAPPDATA%\Programs\TelaVivaOLED`, adds a Start menu shortcut, and the uninstaller removes the files and the Run value.
- Launch at login writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `TelaVivaOLED` = `"<exe>" --background`. The value is read back before the setting is saved.
- Unused Windows App SDK AI/ML files are removed by the `$unusedComponents` list in `build-app.ps1`, including `DirectML.dll`. The list is explicit, so a new runtime file stays until reviewed.
- Builds are unsigned. SmartScreen behavior is documented in the README.

**Consequences**: The download is larger than a framework-dependent build. Users run the installer or extract the zip manually. The size figure must be re-measured after the unused-component step.

**Alternatives**: Inno Setup; WiX v6; MSIX (rejected by Q6); framework-dependent build with the runtime installer chained (redistribution terms not confirmed).

**Evidence**:
- [validation/foundation.md](validation/foundation.md): assembly run, 303 files and 116.7 MB, before the unused-component step; ONNX Runtime not fetched yet.
- T133 and T134 done in the plan. Clean-profile install (I2), launch at login on a real sign-in (I1) and SmartScreen (I3): Not run.

---

## 10. Windows App SDK license versus the open-source rule (OPEN DECISION)

**Proposed title**: To be decided by the owner before the ADR is written. Title if option (a) is chosen: ADR 0025, Microsoft Software License Terms for the redistributed Windows App SDK runtime.

**Context**: [AGENTS.md](../../AGENTS.md) and ADR 0001 require redistributed dependencies to be open source. The Windows App SDK source is MIT on GitHub, but the NuGet runtime that the app redistributes (WinUI 3 and the Windows App SDK packages) ships under Microsoft Software License Terms. See [licenses/DEPENDENCIES.md](../../licenses/DEPENDENCIES.md). The redistribution terms were not confirmed when Decision 12 was written.

**Options for the owner**:
- (a) Accept the Microsoft Software License Terms for the redistributed runtime as a stated exception to the open-source-only rule. The ADR and README then name the exception and the license file that ships in `licenses/windows/`.
- (b) Replace the shell framework so no Microsoft-licensed runtime is redistributed. The architecture's fallback is a Win32 or Direct2D shell (Decision 1). Cost: much more UI code and a loss of native Fluent controls and materials. The S10 and S11 results do not carry over.
- (c) Build the Windows App SDK runtime from its MIT-licensed source and redistribute that build. Feasibility is not evaluated in this repository.

**Consequences**: The choice decides whether section 1 stands as written. Until it is made, the ADR cannot be finalized.

**Evidence**:
- [licenses/DEPENDENCIES.md](../../licenses/DEPENDENCIES.md), Windows native dependencies table.
- [architecture.md](architecture.md) Decision 12 and Technical Constraints.

---

## 11. Known limitations (for the consequences of the ADRs above)

1. **Router outstanding-action inputs are not wired.** The C ABI `tv_submit` returns only a status, so the key router does not receive outstanding-action inputs from the core (section 6).
2. **Calibration crash recovery restores only adaptive brightness.** Recovery at the next launch restores the saved scheme values and deletes `calibration-session.json`. It does not restore the monitor's brightness. See [contracts.md](contracts.md), `calibration-session.json`.
3. **Hook removal while idle is noticed only on the next key press.** The watchdog runs only while a key is held (section 6).
4. **`ApiMissing` cannot be told apart from `NotReadable` in the WMI probe.** The shell may therefore report a different laptop unavailable reason than the cause (see [contracts.md](contracts.md), laptop availability). Not verified on a laptop.
5. **The DirectML index-to-adapter mapping is inferred, not confirmed.** Index 0 looks like the RTX and index 1 like the Radeon iGPU from latency only. It must be confirmed against the DXGI order by the shell before any profile maps a device to an index ([validation/measurements.md](validation/measurements.md)).

Related unverified items, not on the owner's list: identical serial-less monitors (section 3), EcoQoS effects (section 5), camera behavior (section 7), and all laptop rows (section 8).
