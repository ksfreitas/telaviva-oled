# Changelog

## Unreleased

### Added

- Optional continuous camera capture, suspension while all eligible external displays sleep, and a five-second exposure-settling period for low-light recovery on macOS.
- Deterministic native camera lifecycle tests, display-state policy/ABI coverage, and localized inline camera status wording.
- One-command Apple Silicon macOS build through `scripts/build-macos.sh`, compatible with Sh, Bash, and Zsh.
- Finalized the camera lifecycle decision in ADR 0003 and archived its standalone plan and unrun macOS hardware acceptance checklist.
- Corrected the macOS CI `lipo` invocation to verify the packaged arm64 executable.

- Baseline project documentation and CodeAdvisor conventions for the planned Rust application on macOS and Windows.

- Shared Rust policy core and versioned C ABI, with automated settings, protection ownership, battery, low-light, and lifecycle checks.
- Initial local YuNet/ONNX Runtime inference crate with pinned model/runtime provenance and CPU fallback.
- Native macOS menu-bar shell and Apple Silicon DDC adapter under integration; hardware acceptance remains pending.
- Core coverage reporting and macOS CI checks. Windows implementation is deferred.

- MIT project license and public GitHub README with macOS build, usage, privacy, compatibility and contribution guidance.
- Git ignore rules for local build outputs, downloaded runtimes, IDE state and local signing material.

- Consolidated the initial feature documentation into ADR 0002, the cumulative architecture overview and `api.md`; archived the original specification, plan and validation history without marking pending acceptance complete.
