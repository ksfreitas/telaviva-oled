# Test strategy

## Automated evidence

On 2026-10-16, all 24 applicable Rust tests passed with the pinned local model and macOS arm64 runtime. Formatting and workspace Clippy with warnings denied also passed.

| Layer | Verified behavior | Limits |
|---|---|---|
| Core policy: 16 tests | Settings bounds, corrupt/future files, locale parity, multiple monitor identities, normalized controls, serialized actions, stale acknowledgements, two fresh absences, low-light recovery, battery suspension and AC resume, stale camera generation, manual changes and read-before-restoration, shutdown, saver/presence overlap on camera failure | Native events and actual device operations are simulated. Not every transition ordering is covered. |
| C ABI: 3 tests | Version mismatch, lifecycle, callback revision, bounded snapshots and buffer release, closed-state behavior, invalid JSON/UTF-8, oversized and null/nonempty inputs | Callers must uphold live-pointer/lifetime requirements. Reentrant callbacks and destroy racing another call are not tested or authorized by the pointer contract. |
| Inference helpers: 4 tests | Frame bounds/stride, BGR NCHW letterboxing, numeric YuNet decode/NMS, SHA-256 | Numeric decoder fixtures do not establish model accuracy. |
| Real ONNX integration: 1 test | Pinned runtime/model execution, actual face fixture yields present, neutral input yields absent, dark input yields insufficient light, invalid-frame result consistency, CoreML candidate initialization with CPU fallback | One portrait is not a recall benchmark. CoreML was advertised but CPU selected on this host; no accelerated energy benefit is established. |

The positive portrait fixture is CC0-derived, with provenance in `crates/telaviva-inference/tests/fixtures/LICENSE.txt`. It is a static test asset, never a captured webcam frame or enrollment reference.

## Commands

```sh
cargo test --workspace --all-targets --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo fmt --all -- --check
```

The runtime integration requires preparation described in the macOS build guide. CI deliberately skips `bundled_runtime_executes_pinned_yunet_model_on_cpu`, runs the remaining workspace tests and Clippy, and uploads core LCOV. Coverage results and exclusions are in [coverage.md](coverage.md).

## Native and physical acceptance

Cargo tests exclude SwiftUI/AppKit, AVFoundation, screen-saver/power notifications, code signing, app-bundle loading, and physical DDC. Native compilation and ABI smoke tests are separate evidence. Validate read-only discovery before monitor writes. Camera sharing, permission denial/removal, live lighting, battery events, two physical monitors, restoration on quit, screensaver startup, and energy budgets remain explicit acceptance checks.

No webcam frames are saved by these tests. Source review is documented in [privacy.md](privacy.md) and [protection-lifecycle.md](protection-lifecycle.md). Process measurement tooling and unverified budgets are described in [performance.md](performance.md).
