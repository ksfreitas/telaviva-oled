# ADR 0001: Native shells with a shared Rust policy core

- Status: Accepted; implementation in progress
- Date: 2026-10-16

## Context

TelaViva OLED needs native macOS and Windows interfaces, shared monitor protection rules, and low idle resource usage. Presence detection must remain local, distinguish insufficient illumination from absence, and stop camera acquisition under the configured battery policy. DDC capabilities vary by monitor and connection.

## Decision

Use one Cargo workspace with a shared Rust policy/settings core and a separate local inference crate. Native SwiftUI/AppKit on macOS and the planned WinUI 3 shell on Windows communicate with policy through a versioned C ABI. Bounded UTF-8 JSON commands and revisioned snapshots carry state and hardware actions; camera frame data stays outside the policy ABI.

Native adapters own operating-system devices, notifications, and serial DDC execution. Rust owns protection reasons, sampling policy, action generations, saved brightness, and restoration ownership. Brightness restoration requires a fresh read matching the app's last owned value. Manual or external changes must be preserved.

Use the open-source YuNet model and ONNX Runtime with CPU as the default. CoreML is an explicit candidate with CPU fallback; enable acceleration in production only after the specified energy and correctness measurements. Redistributed dependencies and model assets require retained open-source licenses and provenance. Apple system frameworks remain platform facilities.

Initial implementation targets Apple Silicon macOS. Windows implementation and verification are deferred by the user. Compatibility, including M1, is evaluated per display and connection.

## Consequences

The application avoids a web UI runtime and can use each platform's native controls. Shared policy can be tested without camera or DDC hardware, while platform behavior still needs separate acceptance. Two native shells and a documented ABI require integration work and explicit ownership/lifetime rules.

The first implementation groups policy in `core.rs`, settings in `settings.rs`, and ABI in `ffi.rs`; native Swift executes device actions. Static locale catalogs cover current messages; a full Fluent formatter is deferred until needed. These implementation details supersede the speculative file tree, without changing the responsibility boundary.

## Validation limits

Rust tests and coverage establish automated policy/inference evidence. They do not establish universal DDC support, simultaneous camera sharing, screensaver startup reconciliation, live face recall, accelerated energy savings, or release readiness. See the current [validation reports](../specs/archived/external-monitor-control-and-oled-presence-protection/validation/results.md) and [implementation plan](../specs/archived/external-monitor-control-and-oled-presence-protection/plan.md).
