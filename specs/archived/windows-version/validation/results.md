# Windows validation results (T139)

Date: 2026-10-17. Machine: Windows 11 Pro build 26300, AMD Ryzen 7 9800X3D desktop, AMD Radeon iGPU (driver 32.0.21041.1000), NVIDIA RTX 4080 SUPER (driver 32.0.16.1742), one DELL S3225QC over HDMI, Lenovo Performance RGB and IR cameras. No battery, no NPU, no internal panel. Source of truth for every row: `hardware-checklist.md`.

## Automated verification (all green on this machine)

| Check | Result |
|-------|--------|
| `cargo fmt --all -- --check` | clean |
| `cargo clippy --workspace --all-targets --locked -- -D warnings`, also `-p telaviva-inference --features directml` and `--target aarch64-apple-darwin` | clean |
| `cargo test --workspace --all-targets --locked` | all pass (the Windows-only inference tests included, runtime fetched) |
| `cargo check --target aarch64-apple-darwin --all-targets` | compiles (the macOS Swift shell and its tests were not built or run: not possible on this machine) |
| Native build, `msbuild TelaViva.sln /p:Platform=x64 /p:Configuration=Release` | 0 diagnostics from our code (an environmental Roslyn CS1668 warning about a missing `atlmfc\lib\x64` directory comes from the XAML compiler task) |
| `build\x64\Release\tests.exe` | 592 tests, 0 failed |
| `TelaViva.exe --smoke-test` | exit 0, also from the installed copy |
| DLL export check against the C headers | core 10 of 10, inference 9 of 9 |

The GitHub Actions workflow is written but has never run.

## Rows by status

| Status | Rows |
|--------|------|
| Pass | D1, D2, U1 (automated capture only), A1, L1 |
| Partial | U7, A2, A5, I2 |
| Not run | D3 to D10, U2 to U6, P1 to P7, A3, A4, K1 to K4, L2, L3, I1, I3 |

Details and numbers are in the checklist rows, `measurements.md`, `performance.md`, `spike-s2-identity.md`, `spike-s2b-audio.md` and `ux-parity.md`.

## What the README and release notes may claim

May claim:
- A native Windows 11 shell exists (tray icon, flyout, Settings with General, Keyboard and Automatic brightness pages) and builds and passes its automated tests.
- The DDC read path works on one monitor (DELL S3225QC over HDMI): brightness and volume were read, with a stable monitor id.
- Face detection runs locally on the CPU and detects the test face; the measured CPU latency is 5.27 ms per detection on this machine.
- DirectML produced the same detection result as CPU on this machine's two GPUs; **no energy benefit was measured** and the shipped default is CPU only (empty allowlist).
- On a desktop with no internal panel, Laptop follow is reported unavailable with the right reason and writes nothing.
- The app's idle cost with all protections off on this machine: effectively 0% of one core and about 59 MB.
- An unsigned per-user zip installer exists and installed and uninstalled cleanly in an isolated folder on this profile.

Must not claim (not run, or no evidence):
- Any DDC write behavior, sleep or resume behavior, multi-monitor, dock, DisplayPort or HDR behavior.
- Presence protection working end to end with a real camera, privacy-denied or shared-camera handling, or IR camera hiding.
- Volume keys controlling a monitor, mute, behavior with an elevated window, or behavior with any GPU vendor other than the NVIDIA HDMI path whose container id matched.
- Anything about Laptop follow, calibration, lid pause or crash recovery on a real laptop; battery suspension.
- Battery or energy savings of any kind, NPU support, ARM64 support, Efficiency mode latencies.
- Visual quality at light theme, high contrast, 100/150/225% scaling, or with Narrator; the rendered look at all (the session was locked during the review).
- That the installer works in a clean profile, that launch at login works on a real sign-in, or SmartScreen behavior.

## Open decisions for the owner

1. The redistributed Windows App SDK runtime ships under Microsoft Software License Terms, not an open-source license; this conflicts with the AGENTS.md rule "open-source redistributed dependencies only" (see `adr-notes.md`).
2. Whether to keep 8 DIP flyout card spacing and the 640 DIP height cap (F1 in `ux-parity.md`).
3. The DirectML adapter index to physical GPU mapping is inferred from latency and must be confirmed against the DXGI order before any acceleration profile is recorded.
