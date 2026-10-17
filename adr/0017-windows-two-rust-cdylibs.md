# ADR-0017: Windows Rust core and inference as two cdylibs with a C ABI

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

Linking two Rust staticlibs into one MSVC binary commonly collides on duplicated standard library symbols. A Rust CRT inside the hybrid-CRT shell is also a risk.

## Decision

- Build `telaviva_core.dll` and `telaviva_inference.dll` with `cargo rustc --lib --crate-type cdylib`, through `tools/windows/build-rust-dlls.ps1`.
- The shell links their import libraries.
- The core crate manifest is unchanged.
- Both DLLs keep ABI version 1.

## Alternatives Considered

- **One combined crate re-exporting both ABIs.** Exports from dependency rlibs are unreliable on MSVC.
- **Two staticlibs.** Duplicate symbols.

## Consequences

- The build ships two copies of the standard library and two DLLs.
- Every export change must pass `tools/windows/check-dll-exports.ps1` against the headers.
- The DLL boundary keeps the Rust and shell CRTs separate. Spike S1 checked exports only and does not test behavior under the shell's hybrid CRT.
- Evidence:
  - [Spike S1](../specs/archived/windows-version/validation/spike-s1-exports.md): `telaviva_core.dll` 10 of 10 exports and `telaviva_inference.dll` 7 of 7 match the headers, with 0 mismatches.
  - [Foundation validation](../specs/archived/windows-version/validation/foundation.md): the DLL export check and the core snapshot round trip.

## Non-Obvious Constraints

- Behavior under the shell's hybrid CRT is untested. Treat any CRT-related failure as open until it is validated.
- Related: [ADR 0001](0001-native-shell-and-rust-core.md) defines the core and inference split that this record implements on Windows.
