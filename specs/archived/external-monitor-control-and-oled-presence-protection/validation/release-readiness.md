# macOS local testing readiness

Date: 2026-10-16. Status: local development bundle available; hardware acceptance and distribution release incomplete. Windows is deferred.

## Verified

- Optimized arm64 `.app` built at `build/macos/TelaViva OLED.app`, with pinned model/runtime checks, localized camera descriptions and third-party notices.
- Native ABI smoke test returns 0; strict ad-hoc signature verification passes. Xcode project build evidence is in [results.md](results.md).
- All 24 Rust tests pass, including actual positive-face YuNet execution. Workspace Rust line coverage is 83.48%; formatting and Clippy pass. Swift/device behavior is excluded from that percentage.
- DELL S3225QC read-only VCP brightness and volume return supported values on the recorded M4 Pro connection.
- A short protections-off idle run measured 84.31–84.42 MiB RSS and approximately 0.017% CPU. This does not establish active-camera resource or energy budgets.

## Not yet accepted

- Physical monitor writes and ownership-checked restoration, including manual OSD changes, disconnection, failure and normal quit.
- Two external monitors, M1, different cables/docks and unsupported hardware matrix.
- UI click-through, both locales, saved-settings restart and actual launch-at-login behavior.
- Camera permission, live detection/low light, removal and sharing with another app; power-event transitions on actual battery.
- Screensaver notifications and already-active startup reconciliation. Unknown startup does not trigger protection.
- Active/suspended resource measurements, detector release, matched acceleration accuracy/latency and energy comparisons. CPU remains default.
- Developer ID signing, notarization and clean-account installation.

Presence and screensaver protections default to disabled. The app detects any face without identifying or enrolling a person. A successful read/build/test must not be presented as complete OLED protection or broad platform compatibility.
