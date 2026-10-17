# ADR-0024: Windows distribution: unpackaged zip with a per-user PowerShell installer and HKCU Run for launch at login

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

The Windows spec rejects MSIX (Q6). Inno Setup is not OSI-licensed and requests a commercial license. WiX v6 charges a maintenance fee for revenue use. Both conflict with the open-source rule in [AGENTS.md](../AGENTS.md) and [ADR 0001](0001-native-shell-and-rust-core.md).

## Decision

- `packaging/windows/build-app.ps1` produces a self-contained, hybrid-CRT app folder with the two Rust DLLs, the CPU `onnxruntime.dll`, the model and manifest, the self-test frame, `acceleration-profiles.toml`, licenses and `VERSION`. It also produces the zip `TelaVivaOLED-<version>-win-x64.zip`.
- The zip also contains `Install-TelaVivaOLED.ps1` and `Uninstall-TelaVivaOLED.ps1`. The installer copies the app to `%LOCALAPPDATA%\Programs\TelaVivaOLED` and adds a Start menu shortcut. The uninstaller removes the files and the Run value.
- Launch at login writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `TelaVivaOLED` = `"<exe>" --background`. The value is read back before the setting is saved.
- Unused Windows App SDK AI/ML files are removed by the `$unusedComponents` list in `build-app.ps1`, including `DirectML.dll`.
- Builds are unsigned. SmartScreen behavior is documented in the README.

## Alternatives Considered

- **Inno Setup.** Not OSI-licensed; requests a commercial license.
- **WiX v6.** Charges a maintenance fee for revenue use.
- **MSIX.** Rejected by Q6.
- **Framework-dependent build with the runtime installer chained.** Redistribution terms not confirmed (see [ADR 0025](0025-windows-app-sdk-license-versus-open-source-rule.md)).

## Consequences

- The download is larger than a framework-dependent build.
- Users run the installer or extract the zip manually.
- The size figure must be re-measured after the unused-component step.
- Evidence:
  - [Foundation validation](../specs/archived/windows-version/validation/foundation.md): assembly run of 303 files and 116.7 MB, before the unused-component step. The ONNX Runtime was not yet fetched.
  - Not run: clean-profile install (I2), launch at login on a real sign-in (I1), and SmartScreen (I3).

## Non-Obvious Constraints

- The `$unusedComponents` list is explicit. A new runtime file from the Windows App SDK stays in the package until it is reviewed.
