# ADR-0016: Windows shell: C++/WinRT WinUI 3, unpackaged and self-contained, with a UI-free platform library

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

[ADR 0001](0001-native-shell-and-rust-core.md) chooses WinUI 3 for the Windows shell. Platform logic must be testable without XAML, and `--smoke-test` must run before any XAML initialization.

## Decision

One process with two native projects.

- `native/windows/Platform` is a static library with no XAML dependency. It talks to the core only through `CoreBridge` and to the operating system only through interfaces (`IDdcPort`, `IDisplayCatalog`, `IPowerEvents`, `IAudioOwner`, `ICameraBackend`, `ILaptopPanel`, `IKeyHook`, `IClock`, `IEfficiency`) that tests replace with fakes.
- `native/windows/TelaViva` is the C++/WinRT WinUI 3 app: tray host, flyout, Settings and the calibration dialog.
- The app is unpackaged on Windows App SDK 2.5.1, self-contained, built with the hybrid CRT (`native/windows/HybridCRT.props`), and declares per-monitor DPI v2.
- A named mutex keeps one instance.

## Alternatives Considered

- **Win32 or Direct2D-only shell.** Lower idle cost, but much more UI code. This is the fallback only if the idle budget of spike S10 is missed.
- **Rust-native shell.** Breaks ADR 0001.
- **Framework-dependent Windows App SDK with the runtime installer chained.** The installer's redistribution terms were not confirmed.

## Consequences

- Tests run as plain executables (`tests.exe`) without XAML.
- The self-contained runtime makes the package large. The 2026-10-17 assembly was 303 files and 116.7 MB before the unused-component step (see [ADR 0024](0024-windows-zip-distribution-and-hkcu-run.md)).
- The Windows App SDK license question in [ADR 0025](0025-windows-app-sdk-license-versus-open-source-rule.md) applies to this choice. The outcome of that decision determines whether this record stands as written.
- Known limitations that no other ADR in this set owns. No ADR in this set covers the laptop panel, so they are recorded here.
  - Calibration crash recovery restores only adaptive brightness. Recovery at the next launch restores the saved scheme values and deletes `calibration-session.json`. It does not restore the monitor's brightness (see [contracts](../specs/archived/windows-version/contracts.md)).
  - The WMI probe cannot tell `ApiMissing` from `NotReadable`, so the shell may report a different laptop unavailable reason than the cause. Not verified on a laptop.
- Evidence:
  - [Foundation validation](../specs/archived/windows-version/validation/foundation.md): native build with 0 warnings and 0 errors, `tests.exe` 50 of 50, `--smoke-test` exit 0, `--exit-immediately` in about 260 ms.
  - [Spike S10](../specs/archived/windows-version/validation/spike-s10-idle.md): provisional go. The tray-only shell used 0.078% of one core and a 57.6 MB working set over 60 s. A longer window and a full-app measurement are open.
  - [Spike S9](../specs/archived/windows-version/validation/spike-s9-resources.md): unpackaged `.resw` loading works.

## Non-Obvious Constraints

- The platform library must not depend on XAML, because `--smoke-test` runs before any XAML initialization.
