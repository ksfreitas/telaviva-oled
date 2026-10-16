# macOS feasibility evidence

Date: 2026-10-16. Scope: Apple Silicon macOS implementation. Windows is deferred.

## Host

- Apple M4 Pro, arm64; macOS 27.0.1 (26A434).
- Rust/Cargo 1.99.1, Swift 6.4, Xcode 27.0.
- System display information reports an external DELL S3225QC and built-in Color LCD. This establishes enumeration, not DDC support. The displays were reported asleep during the initial probe.

## Executed checks

- Shared Rust libraries compile in debug and release profiles.
- The integrated native bundle compiles; its C ABI smoke client returns exit 0.
- The Xcode project parses and an arm64 Xcode build succeeds.
- The local bundle passes strict deep code-sign verification with an ad-hoc signature. This is not Developer ID signing or notarization.
- Pinned ONNX Runtime and YuNet execute on CPU, including positive-face, neutral and low-light checks. CoreML candidate setup falls back to CPU on this host; accelerated energy savings are unverified.

## Capability gates

| Capability | Current evidence | Remaining gate |
|---|---|---|
| Native ABI | Build and smoke client pass | UI interaction and lifecycle acceptance |
| Apple Silicon DDC | Read-only CLI successfully reads Dell brightness 10/100 and volume 0/100 after correcting IORegistry mapping | Controlled writes/restoration and broader connection matrix |
| Camera | AVFoundation adapter and permission handling compile; generation/power policy has core tests | Actual capture, contention/sharing, permission denial, removal and lighting |
| Screensaver | Starts unknown; independent policy and notification adapter exist | Validate start/stop and already-active startup; lock/sleep are not substituted |
| Energy/resources | Release build and process-sampling harness exist | State-specific measurements and matched energy comparisons |

Unsupported DDC controls stay unavailable. Presence starts disabled, and no webcam capture or physical monitor write has been performed for these feasibility checks. No uniform M1 or connection compatibility claim is made.
