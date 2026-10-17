# ADR-0023: Windows UI says "Laptop" and "Notebook"; core identifiers unchanged

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

The macOS copy says "MacBook". A Windows built-in panel is not a MacBook, and Windows copy must not name a Mac. [ADR 0015](0015-native-shells-own-ui-strings.md) makes each shell own its UI strings.

## Decision

- In Windows resources only, English text says "Laptop" and Portuguese (pt-BR) text says "Notebook".
- No Windows resource contains "MacBook" or "Mac". `native/windows/Tests/strings_keys.cpp` checks this, along with key parity between `en-US` and `pt-BR`.
- Core identifiers and ABI names are unchanged (for example `builtin_display` and `noBuiltInDisplay`).
- The rule covers UI text only. The core and the macOS shell keep "MacBook".

## Alternatives Considered

- **"MacBook" everywhere.** Wrong on Windows.
- **Renaming core identifiers.** ABI churn with no user benefit.

## Consequences

- The same concept has different text on each platform.
- Shared keys keep the same names across locales.
- Evidence:
  - [Spike S9](../specs/archived/windows-version/validation/spike-s9-resources.md): `strings_keys` passes. The check covers both locales and the "Mac" rule.
  - Spec Q10, FR-017 and SC-009.
  - Hardware rows L1 to L3 (laptop rows) are not run.

## Non-Obvious Constraints

- A new Windows string key must exist in both `en-US` and `pt-BR`, or `strings_keys` fails.
