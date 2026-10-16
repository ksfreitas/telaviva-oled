# ADR 0002: Shared policy core with native device adapters

- Status: Accepted; implementation and acceptance remain in progress
- Date: 2026-10-16
- Feature: external-monitor-control-and-oled-presence-protection
- Spec: [Archived feature specification](../specs/archived/external-monitor-control-and-oled-presence-protection/spec.md)

## Context

The application needs native desktop controls for external monitor brightness and volume, plus optional presence and screensaver brightness protections. DDC support varies by monitor and connection. Camera observations must remain local, insufficient light must not count as absence, and app-owned brightness must be restored only when ownership is still confirmed. These requirements span shared policy and OS-specific device behavior.

## Decision

Use a modular desktop application with one shared Rust policy/settings core and local inference crate, connected to native OS shells and device adapters through a versioned C ABI. Rust owns policy, settings validation, action generations and brightness ownership. Native adapters own OS events and device calls. Keep camera frames inside capture/inference; send classifications and status to the core. Use YuNet with ONNX Runtime and CPU fallback; use acceleration only for measured device profiles with an energy benefit. On macOS, the current implementation uses SwiftUI/AppKit and an isolated IOAV adapter. Windows remains a planned C++/WinRT WinUI 3 implementation.

Presence and screensaver protections are independent reasons. Only two valid no-face observations trigger presence dimming. Low-light and camera errors suspend presence rather than count as absence. Before restoration, read the current brightness and restore only if it still matches the app's last applied value. Battery suspension stops presence acquisition while allowing independent screensaver protection to continue.

## Alternatives Considered

- A shared custom-drawn egui interface was superseded by the stated per-OS native appearance requirement.
- A service plus UI, an always-running async executor, and shared mutable callback state were considered against the selected single-process event-driven coordinator; the architecture records bounded channels and explicit ownership as the selected approach.
- A generic webcam wrapper and OpenCV VideoCapture were not selected because shared access and energy behavior require platform-specific validation.
- OpenCV DNN, BlazeFace, GPU-only inference, repeated external DDC CLI launches, software dimming, and independent protection loops were considered in the design and rejected for the stated dependency, fallback, transport, or ownership reasons.

## Consequences

The policy and inference logic can be exercised without physical devices, while each native adapter still requires platform-specific integration and acceptance. The macOS implementation exists; Windows implementation and validation are deferred. DDC behavior remains connection-specific. CoreML is only a candidate on macOS and no energy benefit is established. Native screensaver startup, camera sharing, live camera behavior, writes/restoration, resource budgets, and distribution readiness remain open gates.

## Non-Obvious Constraints

- A readable DDC value does not prove writes or restoration work; the recorded Dell result is read-only and specific to one host/connection.
- Brightness zero is a requested value and does not guarantee that a panel is off or that OLED aging is prevented.
- Insufficient illumination is unknown presence, not absence. Low-light recovery uses periodic light-only observations and requires two usable samples before resuming face inference.
- No webcam frames, images, biometric templates, or identity data are persisted or sent through the core ABI.
- The first implementation targets Apple Silicon macOS. M1 support is connection-specific; Intel Macs are deferred. Windows and physical hardware acceptance remain deferred or incomplete as documented in the archived validation reports.
