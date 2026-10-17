# ADR-0007: Use private DisplayServices and CoreBrightness APIs for built-in display brightness

**Status:** Accepted
**Date:** 2026-10-16
**Feature:** macbook-brightness-follow
**Spec:** [spec](../specs/archived/macbook-brightness-follow/spec.md)

## Context

The macbook-brightness-follow feature must read the built-in display brightness, set it during calibration, and switch macOS automatic brightness off during calibration and restore it afterward. No public, stable API does any of this on Apple Silicon.

AGENTS.md requires open-source redistributed dependencies and treats Apple system frameworks as platform facilities (ADR 0001). ADR 0002 records the isolated IOAV adapter for macOS DDC. The repository already relies on undocumented CoreDisplay and IOAVService symbols behind fail-closed behavior. No ADR records a policy for undocumented display APIs, and this feature adds more of them.

## Decision

Use two private macOS frameworks. Both are loaded at runtime and never linked:

- DisplayServices: `DisplayServicesCanChangeBrightness`, `DisplayServicesGetBrightness` and `DisplayServicesSetBrightness` (float, 0.0 to 1.0), resolved with `dlopen` and `dlsym`.
- CoreBrightness, through the Objective-C runtime: `CBClient`, then `CBDisplayClient`, then `CBDisplayBrightnessClient`, using `isAutobrightnessEnabledWithError:` to read and `setAutoBrightnessEnabled:error:` to set automatic brightness.

All use lives behind the `BuiltInDisplayBackend` protocol in `native/macos/TelaViva/BuiltInDisplayBackend.swift`, which is the only file that touches these symbols. The built-in display is identified with the public `CGDisplayIsBuiltin`, and `DisplayServicesCanChangeBrightness` gates availability.

Fail closed: a missing symbol, a nonzero status or a missing built-in display means unavailable. The feature is then disabled, and the popup shows brightness sliders again.

No code is copied from open-source references. Only API facts are used.

## Alternatives Considered

- `IODisplaySetFloatParameter`: second-hand reports say it does not work on Apple Silicon. This is unverified and was not tested.
- Linking the frameworks: rejected, because the build breaks if the symbols move.
- Reading the raw ambient light sensor: rejected. It is model-specific, and it is unnecessary because automatic brightness already moves the built-in level.
- `CBALCGetDisplayAutoBrightnessEnabled`: rejected. It returned 0 while the daemon reported automatic brightness as on.
- `DisplayServicesBrightnessChanged`: rejected. It is absent on macOS 27.

## Consequences

- Mac App Store distribution is excluded under App Review guideline 2.5.1. The app is built locally and ad-hoc signed.
- Whether notarization accepts dlopen'd private frameworks is unverified.
- Behavior on macOS 14, 15 and 26, on a Mac mini and with the lid closed is unverified. Fail-closed detection and the Phase 0 checks in the plan cover these cases.
- Brightness is observed by polling at 1 s, because the signature of the brightness-change callback is unverified. Each poll is one float read.

## Non-Obvious Constraints

- `DisplayServicesSetBrightness` and the automatic-brightness setter were not invoked during research. Only the getters ran, on a MacBook Pro (Apple M4 Pro, macOS 27.0.1). Both setters are pending hardware acceptance.
- The result type of the automatic-brightness getter comes from reflection. It is not confirmed by any documented contract.
- A failed brightness read is unknown, not zero. The monitor keeps the last good value.
- A failure to read or toggle automatic brightness during calibration is non-fatal. Calibration continues and shows a notice that the level may drift.
- Only `BuiltInDisplayBackend.swift` may reference these private symbols. Other files depend on the protocol.
