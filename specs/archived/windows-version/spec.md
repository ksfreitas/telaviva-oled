# Feature Specification: Windows Version with Laptop Parity and Low-Energy Face Detection

**Feature Branch**: `windows-version`
**Additional requirement (user, during discovery)**: "A aplicação deve executar em modo de eficiência também" (the application must also run in Windows Efficiency mode).

## Context and Understanding

TelaViva OLED controls external monitor brightness and volume over DDC and protects OLED panels by dimming them when a webcam sees no face, when a screensaver runs, or when the machine is on battery. Today only the macOS shell exists. The shared Rust core (`telaviva-core`), the inference crate (`telaviva-inference`), and both C headers are already platform-neutral by design (ADR 0001). `README.md`, `ARCHITECTURE.md` and the ADRs all record Windows as planned, deferred and unvalidated. This feature ends that deferral.

What is missing today:

- No Windows shell: no tray icon, popup, settings window, DDC transport, camera capture, power/screensaver observers, hardware-key handling, audio-output matching, packaging, or CI.
- No Windows inference runtime: the inference crate pins `ort 2.0.0-rc.9` with `load-dynamic`, imports `CoreMLExecutionProvider` without a platform gate (`lib.rs:3`), and only knows the providers CPU (0) and CoreML candidate (1). No Windows accelerator path exists. `ARCHITECTURE.md:31` names only "optional OpenVINO on supported Intel Windows hardware, subject to measured energy benefit".
- The macOS-only "MacBook brightness follow" family (ADRs 0007, 0008, 0010, 0012, 0013) has no Windows counterpart.

The product owner wants one Windows application with the same functionality as macOS, the laptop-only features included, using integrated-GPU or NPU acceleration for face detection when it saves energy and falling back to CPU otherwise. The overriding goal is minimum resource and energy use, so the app must also behave correctly under, and preferably opt into, Windows Efficiency mode (EcoQoS).

## Feature Description

A Windows 11 tray application that offers the same user-visible capabilities as the macOS app:

1. **External monitor control**: per-monitor brightness and speaker volume over DDC, fluid sliders, accurate unavailable/failed states, bounded retries.
2. **OLED presence protection**: local face detection (any face, no identification, no enrollment), two-miss dimming, 1 s return checks, low-light suspension, battery suspension, display-sleep suspension, brightness-threshold gate, optional "keep camera on", ownership-checked restore with bounded retries.
3. **Screensaver protection**: opt-in dimming while a screensaver runs.
4. **Hardware keys** (opt-in): volume and mute keys follow the Windows default sound output when it is the external monitor, with a configurable volume key step. Brightness keys are not handled on Windows (decision Q5).
5. **Laptop brightness follow** (the Windows equivalent of "MacBook brightness follow"): on a laptop, external monitors follow the laptop panel brightness through user-managed calibration points; follow pauses when the lid is closed or the laptop panel is off; calibration turns adaptive brightness off and restores it. Disabled with a stated reason on desktops and on laptops without a controllable panel. User-visible text says **"Laptop"** (en-US) and **"Notebook"** (pt-BR); it never says "MacBook" or "Mac" on Windows.
6. **Low-energy inference**: the YuNet detector runs on the CPU by default. The iGPU or an NPU is used only for a device profile where measurement shows lower energy than the CPU with the same detection result, with silent, reliable fallback to CPU (decision Q2). A measurement spike comes before any accelerator is enabled.
7. **Efficiency mode**: the app runs correctly when Windows Efficiency mode is on (user-forced or app-requested), and by default requests it while idle in the background so presence checks cost as little as possible.
8. Settings, language (en-US / pt-BR / auto), launch at login, status icon, and packaging that matches what macOS provides.
9. **A Windows 11-native look and feel**: the app feels like something that ships with Windows 11, the same way the macOS app feels like a Mac app. Parity is in functions and rules, not in layout; the Windows interface is not a port of the macOS popover and tabs.

The macOS app is not changed by this feature, except where a shared change (inference ABI provider values, core identifiers) is strictly needed and stays backward compatible with ABI 1.

## Requirements *(mandatory)*

### Proposed solution

Direction only; the design step decides the details.

- Reuse the shared Rust core unchanged where possible. The Windows shell supplies the platform facts (monitors, DDC results, camera status, power, screensaver, display and lid state, laptop panel level, hardware-key intents, clock) and executes `pending_actions`, exactly as the macOS shell does through the existing C ABI (ADR 0001, ADR 0014, ADR 0015).
- Windows shell in C++/WinRT with WinUI 3 on Windows 11 (`ARCHITECTURE.md:30`, ADR 0001), with its own en-US and pt-BR resources (ADR 0015).
- Face detection stays YuNet through ONNX Runtime with CPU as the default, reference and fallback path. Accelerated providers are candidates enabled per device profile only after measurement, as ADR 0001 and ADR 0002 already require. The bundled providers are CPU and DirectML only; NPU providers come only from the OS (Windows ML through Windows Update) and are never redistributed (decision Q1).
- Validation mirrors macOS: pure-logic tests with fake backends, a build and smoke gate in Windows CI, and a hardware checklist whose rows stay "Not run" until actually run.

User stories:

- **US-001**: As a Windows user with an external OLED monitor, I can adjust its brightness and volume from a tray popup, and I see accurately when a control is unavailable or failed.
- **US-002**: As an OLED owner, my monitor dims when no face is present and returns when I come back, without the app identifying me or storing images.
- **US-003**: As a laptop user on battery, presence checking pauses by default so the camera and detector do not drain the battery.
- **US-004**: As a laptop user, my external monitors follow my laptop screen brightness through calibration points I manage, and follow pauses while the lid is closed.
- **US-005**: As a desktop user, the Laptop brightness features are disabled with a clear reason instead of failing.
- **US-006**: As a user who enabled hardware keys, volume and mute keys control the external monitor only while it is my default sound output. Brightness keys keep their normal Windows behavior.
- **US-007**: As a user on a PC with an iGPU or NPU, face detection uses it only where it is proven to use less energy than the CPU, and falls back to CPU without my noticing when it is missing or fails.
- **US-008**: As a user who put the app in Efficiency mode from Task Manager (or let the app do it), presence protection still works within its documented latencies.
- **US-009**: As a Brazilian user, every Windows screen is in pt-BR, with "Notebook" wherever the English UI says "Laptop".
- **US-010**: As a user, I can set the app to launch at login, off by default, and failed registration is never shown as success.
- **US-011**: As a Windows 11 user, the tray flyout and the Settings window look and behave like the built-in Windows 11 ones (Quick Settings-style flyout, Settings-app-style window), follow my light or dark theme, accent color and high contrast choice, and work with keyboard, Narrator and text scaling.

### Functional Requirements

**Parity**

- **FR-001**: System MUST provide on Windows every user-visible capability listed under Feature Description items 1 to 4 and 8, with the same defaults, ranges and semantics as the macOS app (for example: presence interval 5 to 300 s default 10 s, return interval 0.25 to 5 s default 1 s, brightness-threshold gate 10 to 90 default 20 strict "above", volume key step 1 to 15 default 5, camera history of up to 8 cameras, up to 32 calibration points per monitor at 1 to 100%).
- **FR-002**: System MUST reuse the shared core policy (presence, restore ownership, retries, camera lifecycle deadlines, ambient curve math, settings schema) without re-implementing it in the Windows shell.
- **FR-003**: System MUST read and write the same settings schema (schema 1, additive only) so a settings file is valid on both platforms.
- **FR-004**: System MUST show unavailable or failed controls as such and never present a failed DDC operation as success; DDC writes MUST use bounded, spaced retries with visible status (parity with FR-002 and FR-016 of the initial spec).
- **FR-005**: System MUST target Windows 11 on x64 in this feature. Windows 10 is out of scope. ARM64 is a later feature; the code, build scripts and provider selection MUST NOT assume x64 where avoidable, so ARM64 can be added without redesign (decision Q3). The NPU path requires Windows 11 24H2 or newer; older Windows 11 builds use iGPU or CPU.

**Presence and camera on Windows**

- **FR-006**: System MUST capture webcam frames locally, share the camera with other applications where validated, never write or transmit frames, and never log device identifiers (parity with ADR 0002 and `api.md:115`).
- **FR-007**: System MUST handle the Windows camera privacy setting: when desktop apps are denied camera access, it MUST report a clear camera status and MUST NOT treat the failure as absence of a face.
- **FR-008**: System MUST suspend presence checking while on battery by default (opt-out setting), when every eligible external display is asleep, and when no eligible monitor is above the brightness threshold.
- **FR-009**: System MUST select among multiple cameras (this machine has an RGB and an IR camera) using the same selection history and fallback rules as macOS, and MUST hide infrared cameras from automatic selection and from the camera list by default until they are validated with the detector (decision Q8).

**Screensaver, power, display state**

- **FR-010**: System MUST observe screensaver start/stop and feed the core's screensaver reason; startup reconciliation (screensaver already running at launch) MUST be handled.
- **FR-011**: System MUST observe AC/battery and display sleep/wake and feed them to the core; no DDC write goes to a sleeping display, and wake uses a fresh readback before restore (ADR 0003, ADR 0006).

**Laptop brightness follow ("Laptop" / "Notebook")**

- **FR-012**: System MUST detect whether the computer is a laptop with a controllable internal panel, natively and not over DDC, and MUST disable Laptop follow with a stated reason otherwise (desktop, no internal panel, brightness not adjustable, API unavailable), failing closed at runtime.
- **FR-013**: System MUST let external monitors follow the laptop panel brightness through per-monitor, user-managed calibration points with the same rules as macOS (monitor brightness 1 to 100%, at least 2 points to follow, ascending, up to 32, interpolation, no ping-pong, minimal polling, 250 ms debounce).
- **FR-014**: System MUST pause follow while the lid is closed or the laptop panel is off, resume with the current level, return manual control during the pause, and refuse calibration in that state.
- **FR-015**: System MUST, during calibration, turn Windows adaptive brightness off if it is on and restore the previous value afterwards, including crash recovery at next launch. The setting is per power scheme and per AC/DC state, so both values MUST be saved and restored (decision Q4).
- **FR-016**: System MUST NOT intercept brightness keys on Windows (decision Q5), so they always keep their normal Windows behavior, including for monitors that follow the laptop panel.
- **FR-017**: Windows UI strings MUST say "Laptop" (en-US) and "Notebook" (pt-BR) for the internal panel, and MUST NOT say "MacBook" or "Mac". Core identifiers and ABI names (for example `builtin_display`, `ambient`) MUST NOT change.

**Hardware keys and audio**

- **FR-018**: System MUST offer opt-in (default off) hardware keys limited to volume and mute for the external monitor, active only while it is the default sound output, with mute emulated as volume 0 plus remembered restore and a configurable volume step (ADR 0004, ADR 0005). Brightness keys are out of scope on Windows. The Keyboard tab MUST NOT show brightness options on Windows; the shared `brightness_key_step` setting stays in the schema unused.
- **FR-019**: System MUST match the default sound output to a monitor by a stable identity, with no name-based fallback; if no match is possible, volume keys MUST pass through.

**Acceleration**

- **FR-020**: System MUST run face detection on CPU by default, and MUST select the iGPU or an NPU only for device profiles where recorded measurement shows lower energy than CPU with equal detection results (decision Q2). CPU remains the reference path for correctness tests. Until a profile has a recorded benefit, no accelerator is used on that device.
- **FR-021**: System MUST fall back to CPU, without user action and without a false "absent" result, when an accelerator is missing, a driver or provider fails to load, session creation fails, or inference fails at runtime; the active engine MUST be observable for diagnostics.
- **FR-022**: System MUST NOT redistribute a dependency whose license is not open source (`AGENTS.md`, ADR 0001). Only the CPU provider and DirectML are bundled. NPU providers (Intel, Qualcomm, AMD) MAY be used only when the operating system delivers them (Windows ML through Windows Update), and the app MUST work fully without them (decision Q1).
- **FR-023**: System MUST keep the inference ABI backward compatible (additive provider values only) and update the header contract tests for any new value.
- **FR-024**: System MUST keep the accelerated session cost bounded: session creation or compilation MUST NOT stall presence checks or the UI, MUST be cached per device and driver where the engine allows it, and the detector MUST be released after the idle period used on macOS (30 s) unless measurement justifies otherwise.

**Efficiency mode**

- **FR-025**: System MUST run correctly with Windows Efficiency mode forced on by the user. It MUST NOT override that choice.
- **FR-026**: System MUST, by default, request EcoQoS for the process while it is idle in the background, and MUST return to normal quality of service while the popup or settings window is in use. If the user forces Efficiency mode, the app MUST keep that state and MUST NOT request normal quality of service against it (decision Q7).
- **FR-027**: System MUST NOT raise the system timer resolution, and MUST meet its presence, return and restore latencies with the default timer resolution and with EcoQoS on.
- **FR-028**: System MUST keep the low-level keyboard hook (if used) responsive regardless of efficiency mode, with minimal work inside the hook callback.

**Shell, settings and packaging**

- **FR-029**: System MUST provide a tray icon with single-click popup (monitor cards, presence status, settings, quit), a Settings window with the same tabs (General, Keyboard, Automatic brightness), and the three-state presence status icon (face, no face or off, insufficient light).
- **FR-030**: System MUST own its UI strings in en-US and pt-BR resources, including a language setting with auto detection (ADR 0015), and MUST NOT add display text to core responses.
- **FR-031**: System MUST offer launch at login (default off) through the per-user `Run` registry value, and MUST NOT save a failed registration as success.
- **FR-032**: System MUST build, link the Rust core and inference crates, load the ONNX Runtime library and the pinned model with SHA-256 verification, and run a packaged smoke test (ABI handshake) in Windows CI. The distribution is unpackaged (installer or zip, no MSIX), so no signing certificate is required to build it (decision Q6).
- **FR-033**: System MUST retain licenses and provenance for every redistributed Windows dependency (ONNX Runtime build, any provider library, any DDC or UI library) under `licenses/`.
- **FR-034**: System MUST use native Windows 11 conventions for its interface, not the macOS layout: the tray flyout follows the Quick Settings and system flyout idiom (anchored to the notification-area icon, rounded corners, system material, dismisses when focus is lost, not in Alt+Tab or the taskbar); the Settings window follows the Windows Settings app idiom (system title bar integration, left navigation with General, Keyboard and Automatic brightness, setting rows and cards, system backdrop). It MUST use the system type ramp and icon font, the user's light, dark, accent and high contrast settings, and standard controls, with no custom chrome or custom palette beyond the presence status icon.
- **FR-036**: System MUST carry over the user-facing refinements recorded in the macOS history, as listed in [mac-ux-parity.md](mac-ux-parity.md): slider value labels and the failed-apply line, the refresh control, the camera status line, the protection brightness cap, the per-monitor protection list in General, the Keyboard status line, calibration notices, end triggers, add-point rules and the two-point hint, each with the Windows wording (Laptop and Notebook) and the documented Windows deviations.
- **FR-035**: System MUST be accessible: every control has a UI Automation name, all functions work from the keyboard, text scaling up to 225% and per-monitor DPI do not clip or overlap, high contrast and the system transparency and animation settings are honored, and Narrator announces slider values and availability reasons.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On a Windows 11 x64 machine with at least one DDC-capable external monitor, a supported brightness or volume change reaches the requested value within the device's resolution, verified by readback (parity with SC-001 of the initial spec).
- **SC-002**: With presence protection on and no face in front of the camera, the monitor dims after two valid no-face checks, and returns to the pre-dim brightness within the return interval plus one DDC transaction after a face appears, with camera errors, privacy denial and low light never counted as absence.
- **SC-003**: With all protections off and the app idle, steady-state CPU and working set stay at or below the measured macOS idle figures (about 0.02% of one core, about 85 MiB RSS) or a Windows budget set in design. Numbers are measured and recorded, never claimed without a run.
- **SC-004**: For every accelerator the app enables, a recorded measurement on that device class shows energy per inference lower than the CPU path (method set in design, for example Windows Performance Recorder or battery drain) with the same present/absent result on the shared fixture and a camera sample set. Accelerators that cannot be measured on the available hardware are recorded as "Not run" and stay disabled. If none shows a benefit, the shipped default is CPU only and the acceleration code paths stay disabled.
- **SC-005**: Forcing each accelerator to fail (provider missing, driver absent, session creation error, runtime error) produces CPU results with no user-visible failure and no false "absent", verified by automated tests with fake engines.
- **SC-006**: With Efficiency mode forced on in Task Manager, SC-002 holds with the same logic and the measured latencies are recorded; with the app's own EcoQoS request active in the background, idle CPU use is not higher than without it.
- **SC-007**: On a laptop with a controllable panel, an external monitor with at least two calibration points follows the laptop level within 2 s of a change, pauses within 2 s of the lid closing or the panel turning off, and resumes at the current level on reopen, with no sustained idle CPU.
- **SC-008**: On a desktop or a laptop without a controllable panel, Laptop follow is disabled with the correct reason, and no write is ever sent to a panel the app does not control.
- **SC-009**: Every Windows UI string exists in en-US and pt-BR, and no Windows UI string or resource contains "MacBook" or "Mac".
- **SC-010**: Windows CI builds the shell and the Rust crates, runs the cross-platform core and header contract tests, runs the new Windows pure-logic tests, runs the packaged smoke test, and publishes a build artifact.
- **SC-011**: A hardware checklist for Windows exists with every row recorded as "Not run", "Pass" or "Fail" together with the device and driver used; the final release notes and README claim only what has a recorded run.
- **SC-012**: A recorded visual and accessibility review on Windows 11 confirms, in light, dark and high contrast themes at 100%, 150% and 225% scaling and with a Narrator and keyboard-only pass, that the flyout and Settings window use system materials, type, icons and controls and show no clipped or overlapping content; and the window idles with no timers or animation while hidden, with idle CPU and memory recorded.

## Decisions

Recorded from the product owner's answers after the first pass (2026-10-17). Each decision names the requirements it shaped.

| # | Question | Decision | Shapes |
|---|----------|----------|--------|
| Q1 | Open-source policy versus vendor NPU providers | Bundle only CPU and DirectML. NPU providers only when the OS delivers them (Windows ML through Windows Update), never redistributed. The app must work fully without them. | FR-020, FR-022 |
| Q2 | Energy-benefit gate | **Gated.** Measure first; CPU is the default until a device profile proves lower energy with equal results. Consistent with ADR 0001 and ADR 0002. (First answered B, changed to A by the owner the same day.) | FR-020, FR-021, SC-004 |
| Q3 | Target platforms | Windows 11 x64 in this feature, ARM64-ready code, ARM64 later, no Windows 10. NPU path needs Windows 11 24H2 or newer. | FR-005 |
| Q4 | Adaptive brightness during calibration | Same off-and-restore as macOS, saving and restoring the per-scheme, per-AC/DC values, with crash recovery. | FR-015 |
| Q5 | Hardware keys scope | **Volume and mute only.** No brightness keys on Windows; they keep their normal behavior. | FR-016, FR-018, US-006 |
| Q6 | Packaging and launch at login | Unpackaged (installer or zip), per-user `Run` registry value, no MSIX. | FR-031, FR-032 |
| Q7 | Efficiency mode | App requests EcoQoS while idle in the background and returns to normal while a window is in use. A user-forced Efficiency mode is respected, never fought. | FR-025 to FR-028 |
| Q8 | IR camera | Hidden from automatic selection and the camera list by default until validated with the detector. | FR-009 |
| Q9 | Validation hardware | Record what exists, leave the rest "Not run", claim no support without a run. | SC-011 |
| Q10 | Naming scope | "Laptop" / "Notebook" in Windows UI text only. macOS keeps "MacBook". Core identifiers and ABI names unchanged. | FR-017, SC-009 |

## Clarification Needed

Resolved by [architecture.md](architecture.md) on 2026-10-17: item 1 by Decisions 3 and 4 (allowlist file shipped empty, CPU runtime in the first release, failure markers); item 2 by the Keyboard tab rule in Decision 9 and the Integration Points; item 3 by Decision 3 (NPU deferred, provider value 3 reserved, spike S5); item 4 by the model inspection recorded in Decision 3 (per-operator DirectML support still verified in spike S4); item 5 stays as a recorded limitation in the risks table.

Original open items left for `/codeadvisor-dev:design`:

1. **Q2 measurement spike.** The energy benefit of an NPU or iGPU for this tiny model is unproven. The design must define the measurement method, the device-profile key (hardware, driver, provider version), where the per-profile result is stored, and what happens when a driver or provider update invalidates a recorded benefit. The design also decides whether a first release ships CPU only, with the provider selection kept ready.
2. **Q5 consequence.** Without brightness keys, the shared `brightness_key_step` setting is unused on Windows. The design confirms the Keyboard tab layout and that no schema change is needed.
3. **Q1 consequence.** Windows ML delivery of NPU providers is not usable from the Rust `ort` crate as documented (research could not confirm OpenVINO or QNN support in `ort`, or a plugin-provider loading path). The design must prove a loading route or drop NPU from the first release while keeping the provider order extensible.
4. **YuNet on DirectML and NPU.** The operator list, input shape (the repo uses `[1,3,640,640]`) and int8 variant behavior on DirectML and NPU providers were not verified. The design must verify them with the actual model file.
5. **Validation limits (Q9).** This machine is a desktop with no NPU, battery or internal panel, so laptop follow, battery suspension and NPU rows will be "Not run" unless other hardware appears.

## Notes

- **Findings verified in the repository**: the inference crate pins `ort =2.0.0-rc.9` with `load-dynamic`, imports `CoreMLExecutionProvider` unconditionally at `crates/telaviva-inference/src/lib.rs:3`, and builds as `rlib`, `staticlib` and `cdylib`. `settings.rs:383` already has a Windows `replace_file`. `.github/workflows/` only contains `macos.yml`. The CoreML-specific code probably needs platform gating before a Windows build succeeds. This was not yet built on Windows.
- **Reported, not verified by me**: `telaviva_core.h:40-41` says the core reads no clock, while `core.rs:651-659` calls `SystemTime::now()` in `presence()` to drop observations older than 60 s. Resolve in design because it affects test determinism.
- **Doc inconsistency to resolve**: ADR 0014 (line 27) refers to a community-maintained C# generator for a Windows shell, while ADR 0001 and `ARCHITECTURE.md:30` choose C++/WinRT.
- **Efficiency mode facts (Microsoft Learn, per the research)**: opt-in through `SetProcessInformation` with `ProcessPowerThrottling` (EcoQoS), per-thread variant exists, EcoQoS needs Windows 11, and it schedules on efficient cores at the most efficient frequency. Not raising timer resolution lets 1 s and 10 s timers work at the default resolution. Task Manager's own toggle also lowers base priority. Effects on camera frame delivery, ONNX Runtime threads and low-level keyboard hooks are not documented by Microsoft and must be measured with EcoQoS on.
- **Windows API candidates (research, partly unverified, for design)**: DDC through the Win32 physical-monitor VCP functions (about 40 ms per read; docks and USB-C hubs often break DDC); laptop panel brightness through WMI `WmiMonitorBrightness` and `WmiMonitorBrightnessMethods`; lid, display, AC/DC through power-setting notifications; screensaver through `SPI_GETSCREENSAVERRUNNING`; camera through `MediaCapture` with `MediaFrameReader` in shared read-only mode; tray through `Shell_NotifyIcon` because WinUI 3 has no tray API; default-output tracking through `IMMNotificationClient`, with monitor matching by EDID or container ID unverified. The ambient light sensor API exists but macOS never reads raw lux either, so the follow feature should track the panel level, not lux.
- **Accelerator facts (research)**: Windows ML downloads vendor providers on demand and needs Windows 11 24H2 or newer for NPU paths. The Qualcomm path needs a fixed-shape quantized (QDQ) model. The Rust `ort` documentation lists DirectML but not OpenVINO or QNN feature flags (unverified). The YuNet 2023mar operator list and the best-supported accuracy of its int8 variant were not verified. `ort` silently falls back to CPU if provider registration fails unless errors are made fatal, which design must handle.
- **Presence sensors**: Windows has `HumanPresenceSensor` for dedicated sensors, but it reports presence and not face detection, so it does not replace the webcam requirement. Out of scope here.
- **Process notes**: `AGENTS.md` asks to delegate long implementation to `gpt-6-luna` agents, which are not available in this session. The user's global rule routes reading and editing to Haiku agents with verification by the orchestrator. Archive and documentation conventions come from `ARCHITECTURE.md` (en-US docs, full ADR policy, commit types feat/bug/hotfix/chore, branch template `<type>/<short-description>`). The branch name `windows-version` follows the discover skill's slug rule; rename it if the template must apply.
- **Not measured today**: macOS acceleration energy benefit, camera-on cost of dark-frame settle, wake-up counts. The Windows budget in SC-003 should be set from measurement during design, not copied.
