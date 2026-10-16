# Rust coverage

Measured on 2026-10-16 using `cargo-llvm-cov 0.9.1` and the stable toolchain's `llvm-tools-preview` component.

```sh
mkdir -p build/coverage
cargo llvm-cov -p telaviva-core --all-targets --locked --json --output-path build/coverage/core.json
cargo llvm-cov --workspace --all-targets --locked --json --output-path build/coverage/workspace.json
```

The complete run passed 24 tests: 16 policy/configuration contracts, three C ABI contracts, four inference unit tests, and one integration test executing the pinned model/runtime. The integration test includes a licensed positive face fixture, a neutral frame, insufficient-light input, and provider initialization/fallback. It does not establish real-camera recall or energy savings.

| Rust source | Line coverage | Covered / total |
|---|---:|---:|
| Core `core.rs` | 79.38% | 716 / 902 |
| Core `ffi.rs` | 89.08% | 155 / 174 |
| Core `settings.rs` | 71.55% | 83 / 116 |
| **Core total** | **80.03%** | **954 / 1192** |
| Inference `lib.rs` | 92.29% | 431 / 467 |
| **Workspace total** | **83.48%** | **1385 / 1659** |

Core function coverage: 73.91%. Core region coverage: 75.34%. Branch/MCDC coverage was not enabled and must not be inferred from line coverage. Generated JSON files are local build artifacts. The earlier core snapshot was 79.78%; this run supersedes it.

These percentages cover repository Rust source, excluding dependencies, Swift, native device adapters and physical hardware. CI emits a core LCOV artifact and runs runtime-independent Rust tests; the actual ONNX Runtime integration is verified locally on Apple Silicon after runtime preparation. No minimum percentage is enforced yet. Coverage is not a substitute for restoration ownership, native power events, shared-camera behavior, live low-light accuracy, or resource measurements.
