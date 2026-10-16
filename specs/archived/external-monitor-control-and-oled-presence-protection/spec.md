# Feature Specification: External Monitor Control and OLED Presence Protection

**Feature Branch**: `feat/oled-monitor-protection`
**Status**: Discovery decisions recorded; technical design documented in architecture.md; implementation and hardware validation remain pending.

Confirmed decisions: detect any face without identity recognition; presence sampling defaults to 10 seconds and is configurable; return sampling while presence-dimmed defaults to 1 second and is configurable; settings and UI initially support pt-BR and en-US; camera processing is local with no image storage; use shared-camera capability where supported; battery suspension is configurable and ON by default; use low-energy hardware acceleration when feasible with software fallback. Low-light behavior must not treat insufficient illumination as confirmed absence. Architecture and process choices are recorded in ../../ARCHITECTURE.md; library selection belongs to design.

## Context and Understanding

The user wants convenient control of external monitor brightness and audio volume, plus automatic brightness reductions when a display is unattended or a screensaver is active. The intended benefit is reduced unnecessary display brightness and OLED exposure without losing the user's preferred brightness when they return. This is a new application; there is no existing implementation to repair.

The primary actor is the computer user. External monitors receive brightness and volume requests. The webcam supplies observations of faces; operating-system screensaver and power state supply additional protection conditions. Relevant data includes monitor identity and capabilities, preferred brightness, active protection reasons, detection intervals, camera selection, and power preferences.

Success means supported monitors respond predictably, previous brightness is restored according to clear ownership rules, face detection remains useful in low light, and presence checks have a measured and bounded resource cost. Hardware compatibility and physical darkness at brightness zero require validation; neither universal compatibility nor prevention of OLED degradation is assumed.

## Feature Description

Provide manual brightness and volume controls for external monitors and optional protections that temporarily lower brightness. Presence checking periodically detects any face while the user is present and checks more frequently during absence to restore brightness promptly. Screensaver protection is configured separately. A battery option suspends presence monitoring to save energy. The interface should make active protections, unsupported capabilities, and failures understandable.

## Requirements

### Proposed solution

- **US-001**: As a user, I want to adjust an external monitor's brightness and volume from my computer.
- **US-002**: As an OLED monitor user, I want brightness reduced to zero when no face is present and my previous brightness restored when a face returns.
- **US-003**: As a user, I want configurable presence timing and any-face detection without enrolling my identity.
- **US-004**: As a user, I want a separate brightness reduction when the screensaver is active.
- **US-005**: As a laptop user, I want presence checking optionally disabled on battery.
- **US-006**: As a user, I want low resource use and clear feedback if a device or permission prevents operation.
- **US-007**: As a user, I want quick access to brightness and volume for each connected monitor from the system tray or menu bar, with access to settings and quit.

### Functional Requirements

- **FR-001**: The system MUST target macOS and Windows and control supported external monitor brightness and volume through DDC. Exact OS versions, monitor models, and connection paths remain a validation dependency.
- **FR-002**: The system MUST expose unavailable or failed controls accurately rather than present them as successful.
- **FR-003**: The system MUST allow presence monitoring to be enabled or disabled and configure the present-state sampling interval, defaulting to 10 seconds. Detection MUST accept any face without enrollment or identity verification.
- **FR-004**: After two consecutive valid checks report no face, the system MUST request brightness zero, preserve the prior brightness, and continue return checks defaulting to every 1 second. Both intervals MUST be configurable. Camera errors and insufficient illumination MUST NOT count as valid no-face checks.
- **FR-005**: The system MUST restore presence-applied dimming when a face returns, subject to independent active protections. Brightness ownership and recovery behavior across manual changes, exit, sleep/resume, and disconnect/reconnect MUST avoid restoring stale values.
- **FR-006**: The system MUST provide a separately configurable screensaver brightness reduction; overlapping protections MUST preserve the strongest active reduction and restoration MUST wait until applicable reasons clear.
- **FR-007**: The system MUST provide a configurable battery option to suspend presence monitoring, ON by default. It MUST stop camera acquisition and face analysis while suspended; if presence was the only dimming reason, restore that reduction, while independent screensaver protection continues.
- **FR-008**: When illumination is insufficient for reliable face detection, the system MUST suspend presence inference rather than conclude absence. Recommended policy for design review: restore presence-only dimming, keep independent screensaver dimming, and resume presence inference after low-rate observations indicate usable light. Face inference MUST stop while suspended; camera activity for light recovery is a distinct policy from battery suspension, where acquisition is zero.
- **FR-009**: The system MUST process camera data locally and MUST NOT store or transmit images or video. It MUST use shared-camera support where available and validated, including Windows multi-app camera support and supported macOS sharing; camera sharing is conditional on OS, hardware, driver, and other-app behavior. It MUST handle unavailable sharing without disrupting another app's capture.
- **FR-010**: The system MUST provide a tray/menu-bar popup with single-click access to brightness and volume controls for multiple monitors, plus settings and quit access. The macOS interface MUST use native macOS appearance and behavior, and the Windows interface MUST follow Windows 11 appearance and behavior; both use the same application logic.
- **FR-011**: The initial interface and settings MUST support pt-BR and en-US; project documentation remains en-US.
- **FR-012**: The system MUST support low-energy hardware acceleration, such as an integrated GPU, where feasible and shown to reduce total energy use, with software fallback when acceleration is unavailable, fails, or costs more energy. No implementation library is selected here.
- **FR-013**: The system MUST minimize resource consumption. Numeric CPU, memory, camera-duty, and energy budgets remain an explicit design benchmark gate; no benchmark result is claimed by this specification.
- **FR-014**: The system MUST persist local preferences, including intervals, language choice, camera selection, per-monitor controls, and protection settings. Presence and optional launch-at-login MUST initially be disabled; battery suspension defaults to ON when presence is enabled.
- **FR-015**: Screensaver protection MUST initially be disabled and its configurable target MUST default to zero. The strongest active reduction wins; screen lock and display sleep are outside the initial trigger scope.
- **FR-016**: Manual brightness changes MUST take priority, update the baseline, and suspend dimming for the current protection episode. Normal exit or disabling a protection MUST restore its app-applied reduction unless another reason remains active. Camera failure MUST suspend presence rather than count as absence and MUST attempt to restore presence-only dimming. Failed DDC operations MUST use bounded, spaced retries with visible status; software dimming is outside the initial scope.
- **FR-017**: All redistributed libraries, runtime dependencies, and model assets MUST be open source with license/provenance records. Native APIs provided by the target operating systems remain platform facilities.
- **FR-018**: Initial macOS implementation MUST focus on Apple Silicon; Intel Macs are deferred. Unsupported monitor/connection combinations, including affected M1 paths, are accepted limitations and MUST be reported without claiming successful control. Do not assume that every M1 connection is unsupported.

## Success Criteria

### Measurable Outcomes

- **SC-001**: On every device in the agreed compatibility matrix, supported brightness and volume adjustments reach the requested value within the device's supported resolution; unsupported controls are identified individually.
- **SC-002**: With default settings, two valid no-face checks at 10-second intervals initiate presence dimming; return checks run at 1-second intervals and restore presence brightness after a valid face observation, unless another protection remains active.
- **SC-003**: Presence and screensaver overlap tests produce no premature restoration; battery transitions, manual adjustments, normal exit, and disconnect/reconnect follow the finalized protection and brightness-ownership policies.
- **SC-004**: With presence checking disabled or suspended by battery policy, the app performs no presence frame acquisition or face analysis. Screensaver protection continues according to its independent setting.
- **SC-005**: In usable lighting, measure detection accuracy, false absence, false presence, and restoration latency without identity enrollment. In insufficient lighting, verify presence inference suspends and does not classify the user as absent; validate the proposed recovery policy during design.
- **SC-006**: Measure CPU, memory, camera duty cycle, and energy use with protections off, user present, user absent, low-light suspension, and battery suspension. Numeric pass/fail budgets must be set during design before implementation.
- **SC-007**: Camera denial/unavailability and failed monitor operations produce clear status and the agreed fallback, without an unbounded rapid retry loop or false success indication.
- **SC-008**: The tray/menu-bar popup exposes per-monitor brightness and volume controls, settings, and quit; initial UI strings/settings support pt-BR and en-US. Validate native per-OS controls, system theme, keyboard/focus behavior and accessibility on each platform.
- **SC-009**: With two or more supported monitors connected, a popup adjustment affects only its selected monitor; settings survive restart and changing either interval in the GUI changes the corresponding sampling schedule. All user-facing popup, settings, and status text is available in both initial languages.
- **SC-010**: Compare end-to-end energy use of accelerated and software detection on representative supported hardware; select acceleration only when beneficial. Simulated missing/failed acceleration MUST leave software detection functional under the same protection rules.
- **SC-011**: Validate shared-camera operation concurrently with another camera application on supported Windows and macOS configurations; unavailable sharing produces visible suspended status without treating contention as absence or interrupting the other application.

## Decision Log

- Native-interface requirement supersedes the initial egui design: separate thin macOS and Windows 11 presentation layers share one Rust application core in one repository. Swift/C++ shell integration is a design choice; protection/device behavior stays in Rust.

- Prior proposed defaults not changed in the user's reply are retained: independent per-monitor settings with built-in displays excluded; two valid misses; manual adjustment precedence and normal-exit restoration; strongest reduction during overlap; opt-in screensaver target zero; local settings persistence; optional startup initially off; camera failures suspend presence; bounded DDC retries without software dimming.
- Confirmed: 10-second default presence sampling while present, configurable; 1-second default return sampling while presence-dimmed, configurable; two valid missed checks confirm absence.
- Confirmed: initial UI/settings locales pt-BR and en-US; documentation remains en-US.
- Confirmed: camera processing local, with no image storage; use shared-camera capability where supported and validated. Sharing behavior remains a platform/hardware validation dependency.
- Confirmed: battery suspension option configurable and ON by default, stopping camera acquisition and face detection.
- Confirmed: low-energy hardware acceleration (for example, integrated GPU) where feasible, with software fallback.
- Design resolution: low-light recovery uses light-only observations every 10 seconds, suspends face inference, restores owned presence-only dimming, and resumes after two usable observations. This is an engineering policy selected during design, distinct from the user's explicit requirement to suspend on insufficient light. Screensaver dimming remains independent.
- Confirmed during design: only open-source libraries/dependencies; focus macOS implementation on Apple Silicon. Connection-specific M1 limitations are accepted, without a blanket exclusion of all M1 devices.
- Design selections and numeric acceptance targets are in architecture.md. Actual OS/hardware matrix coverage, low-light calibration, DDC support, shared camera behavior, and platform feasibility remain validation gates, not verified results.

## Notes

- This specification captures user needs and proposed acceptance policies, not a library/API selection, module design, or task checklist.
- Sources for shared-camera feasibility, supplied by project research: [Windows shared camera mode](https://learn.microsoft.com/en-us/uwp/api/windows.media.capture.mediacapturesharingmode?view=winrt-26100), [Windows camera sharing announcement](https://blogs.windows.com/windows-insider/2026/09/10/announcing-windows-11-release-preview-builds-26100-9539-26200-9539/), and [Apple AVCaptureDevice](https://developer.apple.com/documentation/avfoundation/avcapturedevice/). These sources do not establish support across all OS versions, hardware, drivers, or other-app combinations.
- DDC capability and behavior, camera access, screensaver observation, power state observation, and coexistence with other camera applications are technical feasibility dependencies for design and platform validation.
- Hardware integration tests must include supported and unsupported monitor controls, simultaneous protection reasons, repeated no-face observations, camera loss, battery transitions, and manual overrides.
- User-confirmed setup decisions: modular monolith; Rust and Cargo; UI framework deferred; any-face detection; root documentation; no project-specific skills; permanent ADR history; no issue tracker; generic branch pattern; documentation language en-US.
- The user requested removal of root CLAUDE.md; AGENTS.md is the canonical agent guide.
- No code or dependencies have been introduced. The repository was initialized on the feature branch for discovery without committing the working tree.
