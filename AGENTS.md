# TelaViva OLED

## Purpose and status

TelaViva OLED is a planned Rust desktop application for macOS and Windows. It controls external monitor brightness and volume through DDC and provides OLED-oriented brightness protection based on webcam presence and screensaver state.

Implementation is in progress. The repository contains a Cargo workspace, shared Rust policy/C ABI code, local ONNX inference, and a native macOS shell. See the implementation plan and validation reports for completed checks; do not infer physical hardware support or release readiness from code presence. The user has deferred Windows implementation and validation to separate work.

## Project map

- `ARCHITECTURE.md`: intended architecture, requirements, unresolved design questions, and CodeAdvisor conventions.
- `CHANGELOG.md`: release history, starting with Unreleased.
- `adr/`: permanent architecture decision records.
- `specs/oled-monitor-protection/`: requirements, technical architecture, and runtime contracts for the initial application.
- `crates/telaviva-core/`: shared policy, settings and native-shell C ABI.
- `crates/telaviva-inference/`: image quality assessment and local YuNet/ONNX detection.
- `native/macos/`: native SwiftUI/AppKit shell and macOS device adapters.
- `packaging/macos/`: local application build and packaging instructions.

## Development guidance

- Use Rust and Cargo for a modular monolithic application targeting macOS and Windows.
- Prioritize minimal CPU, memory, camera, and battery usage when evaluating designs.
- Presence means detecting any face, without identifying a specific person or requiring face enrollment.
- Keep intended behavior distinct from implemented and hardware-verified capabilities.
- Determine monitor capabilities, platform limitations, and failure behavior during design; do not assume uniform DDC support.
- Follow the selected design in `specs/oled-monitor-protection/architecture.md`: native SwiftUI/AppKit and WinUI 3 shells, one shared Rust core with a versioned C ABI, local YuNet inference through ONNX Runtime, and conditional low-energy acceleration.
- Use only open-source redistributed dependencies and assets. Initial macOS support targets Apple Silicon; Intel is deferred and DDC compatibility remains connection-specific, including on M1.
- Follow the process conventions in ARCHITECTURE.md. Documentation and commit language default to en-US.

## Delegation

- For long tasks, delegate code analysis and implementation to the most recent available Luna agents (`gpt-6-luna`).
- Coordinate delegated work from the primary agent.
