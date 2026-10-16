# macOS implementation validation

Date: 2026-10-16. This log distinguishes executable checks from physical-device acceptance. Windows implementation and validation are deferred by the user's explicit scope change.

## Environment

- Apple Silicon arm64 host; Apple M4 Pro.
- Rust 1.99.1 and Cargo 1.99.1, stable aarch64-apple-darwin target.
- Xcode 27.0, Swift 6.4, macOS SDK available.
- External DELL S3225QC plus the built-in display discovered through read-only system information. DDC control support is not inferred from display enumeration.

## Development evidence

- Initial shared-core suite: 8 policy/configuration tests plus 2 C ABI tests passed; the suite is being extended for additional ownership and lifecycle edge cases.
- Shared-core Clippy check initially passed with warnings denied.
- Both Rust static libraries built using `cargo build -p telaviva-core -p telaviva-inference --locked`.
- The first actual ONNX inference integration test failed on a neutral image, returning a runtime error instead of a valid no-face outcome. Direct model metadata inspection established that the pinned original model expects `[1, 3, 640, 640]`, while the initial preprocessor supplied 320×320. Fix and rerun are required before inference can be reported operational.
- The original native Swift shell had a preliminary compile before complete core/inference/DDC integration. This does not establish the final app build, GUI functionality, camera behavior, or monitor control.

## Subsequent detector and core checks

- The model input was corrected to 640×640. The integration run then exposed incorrect scalar indexing of multidimensional outputs; the decoder now reads contiguous output slices. The actual pinned model and runtime smoke test passes on dark and neutral frames. This verifies execution and the quality/no-face paths, not face recall.
- The isolated test executable, run directly through `/usr/bin/time -l` without Cargo compilation, passed in approximately 0.08 seconds with a maximum resident set size of 49,872,896 bytes (approximately 47.6 MiB). This is a short-process peak for the detector smoke test, not whole-app steady-state memory or a camera pipeline benchmark.
- Core tests were extended to 17 passing tests, including read-before-restoration ownership checks, stale action acknowledgements, disablement with pending writes, and camera failure handling.
- Core line coverage was measured at 79.78%; details and exclusions are in [coverage.md](coverage.md).
- A CoreML candidate was attempted. The runtime advertises availability, but candidate session compilation on this host failed to create its temporary working directory; CPU fallback remained operational. No successful CoreML execution, accuracy equivalence, or energy reduction has been validated.

## Outstanding acceptance

Final results will be appended as commands complete. No successful face-recognition accuracy, energy savings, signing/notarization, simultaneous camera sharing, or monitor write/restoration behavior is claimed by this initial log. The application detects faces without identifying people.

## Expanded Rust validation

- All 24 workspace tests pass: 16 core policy/configuration, three C ABI, four inference helpers, and one actual model/runtime integration test. New cases cover AC resume with stale camera generations and null/nonempty or invalid UTF-8 inputs.
- The real YuNet integration now detects a face in a CC0-derived portrait fixture. Dark and neutral frame checks also pass. This is a positive execution check, not a real-scene recall benchmark.
- Negative inference return codes now agree with `out_result.status` and zero confidence, including guarded panic/error exits.
- Formatting and workspace Clippy pass. Updated line coverage is 80.03% core, 92.29% inference, and 83.48% workspace Rust source. See [coverage.md](coverage.md).

## Native integration build

- The integrated Swift/AppKit/SwiftUI app built through `packaging/macos/build-app.sh`. A separately built root-check bundle also passed `--smoke-test` (exit 0) and strict deep code-sign verification.
- Xcode project parsing and an arm64 Xcode build passed. The earlier macro sandbox error did not recur under the current unrestricted session.
- The initial read-only DDC CLI returned an empty list even though system information reports the Dell. This revealed an IORegistry topology mismatch after CoreGraphics/CoreDisplay enumeration, rather than proof that no external display exists. Investigation/fix is in progress; no successful physical VCP read is claimed yet.
- Host OS: macOS 27.0.1, build 26A434. Native GUI interactions, camera capture and monitor writes were not exercised by these build/ABI checks.

## Physical read-only DDC result

- The IORegistry lookup was corrected for `IOMobileFramebufferShim` and the DCP proxy's direct provider `interface-name`. The final release bundle discovers one external Dell and one IOAV transport.
- Root independently ran `--diagnostics`: DELL S3225QC brightness VCP 0x10 returned current 10 / maximum 100; volume VCP 0x62 returned 0 / 100. Both controls report supported. No Set VCP operation was sent. See [monitor-matrix.md](monitor-matrix.md) for limits.
- Final bundle includes localized camera descriptions, settings/error messages, pinned assets and third-party notices. Release Rust and Swift optimized builds, ABI smoke, strict ad-hoc signature verification and plist checks pass. The local signature does not establish notarization or clean-account distribution acceptance.

## Initial idle app measurement

The optimized native app ran with protections disabled and no saved settings. After five seconds of warm-up, 60 one-second samples reported RSS 84.31–84.42 MiB and approximately 0.017% mean CPU (one logical core basis). The test process was then stopped. This is a short idle baseline, not active-camera/energy/normal-exit acceptance. See [performance.md](performance.md).

## Latest-source Xcode check

An arm64 Xcode build with `CODE_SIGNING_ALLOWED=NO` also passed after the final DDC, settings, localization and camera-release source edits. DerivedData output is separate from the sampled release bundle. The packaging-script bundle remains the runnable artifact, with verified assets and notices included.
