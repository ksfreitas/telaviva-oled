# Changelog

## Unreleased

### Fixed

- Reliable macOS DDC requests for the connected LG ULTRAFINE: transmit requests twice with short spacing, validate complete replies, and confirm the requested brightness or volume through readback before reporting success. A one-point brightness change and restoration were verified on this LG connection.
- Popup brightness and volume sliders now follow the pointer immediately from a local draft, while a bounded background coordinator sends only the latest target to the monitor and reconciles on real DDC completion. Interactive feel on the physical LG is still pending acceptance.
- Keyboard routing now also requires and requests Input Monitoring, re-evaluates permissions when the app is activated or Settings opens (no polling), retries creating the event tap after permission changes, shows each permission's state with its request button disabled once granted, and local ad-hoc builds use an identifier-based designated requirement so permissions survive rebuilds (existing grants must be re-added once).
- Brightness keys that the keyboard sends as plain key codes 144 and 145 are now recognized and routed to the external monitor under the cursor; they were ignored before.
- Hardware brightness and volume keys pressed quickly no longer make the target jump back: the key router now ignores monitor readbacks while its own DDC writes are in flight and for one second afterwards, then compares again, so external changes and monitor clamping are still followed.
- A dimmed monitor that wakes while its DDC channel is not ready is now restored: a failed wake readback or restore write is retried up to three times while ownership of the dimmed value is kept, instead of dropping ownership and leaving the monitor at its dimmed brightness.

### Changed

- Default hardware-key step is now 5 points (was 6.25) and Shift+Option moves 1 point (was 1.5625).

### Added

- Configurable hardware-key step for brightness and volume (1 to 15 points each, default 5) in Settings; Settings window is taller.
- Optional macOS keyboard brightness and volume routing to the supported external monitor under the cursor, with Accessibility status and settings.
- Hardware mute key for the external monitor that is the Mac's sound output: it sets the monitor volume to zero and the next press restores the previous level (emulated through the volume control; the monitor's own mute is not used). Other outputs keep macOS mute.
- Generation-checked manual commands and deterministic keyboard-routing tests covering held keys, repeat accumulation, and stale targets.
- Behavior change: hardware volume keys now adjust an external monitor only while it is the Mac's default sound output (matched by display EDID identifier), instead of the monitor under the cursor, so headphones and other outputs stay under macOS control. Brightness keys still follow the cursor.
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
