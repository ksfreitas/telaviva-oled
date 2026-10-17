# ADR-0025: Windows App SDK license versus the open-source-only rule

**Status:** Proposed (owner decision open)
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

[AGENTS.md](../AGENTS.md) and [ADR 0001](0001-native-shell-and-rust-core.md) require that redistributed dependencies be open source. The Windows App SDK source is MIT-licensed on GitHub. The NuGet runtime that the app redistributes (WinUI 3 and the Windows App SDK packages) ships under Microsoft Software License Terms, not MIT (see [licenses/DEPENDENCIES.md](../licenses/DEPENDENCIES.md)). The redistribution terms were not confirmed when the shell decision in [ADR 0016](0016-windows-shell-winui3-unpackaged-platform-split.md) was written.

## Decision

No option is chosen in this record. The owner must choose one of the following options before this record can be accepted.

- **(a) Accept the Microsoft Software License Terms** for the redistributed runtime as a stated exception to the open-source-only rule. The ADR and README then name the exception and the license file that ships in `licenses/windows/`. [ADR 0016](0016-windows-shell-winui3-unpackaged-platform-split.md) stands as written.
- **(b) Replace the shell framework** so that no Microsoft-licensed runtime is redistributed. The architecture's fallback is a Win32 or Direct2D shell. Cost: much more UI code and a loss of native Fluent controls and materials. The S10 and S11 results do not carry over.
- **(c) Build the Windows App SDK runtime from its MIT-licensed source** and redistribute that build. Feasibility is not evaluated in this repository.

## Alternatives Considered

The three options above are the alternatives. The choice among them is open.

## Consequences

- The choice decides whether [ADR 0016](0016-windows-shell-winui3-unpackaged-platform-split.md) stands as written, and it affects [ADR 0024](0024-windows-zip-distribution-and-hkcu-run.md), which packages the runtime.
- Until the choice is made, this record cannot be finalized.
- Evidence: [licenses/DEPENDENCIES.md](../licenses/DEPENDENCIES.md), Windows native dependencies table; [architecture](../specs/archived/windows-version/architecture.md), Decision 12 and Technical Constraints.
