# Spike S1: Rust cdylib exports on MSVC

Date: 2026-10-17. Machine: Windows 11 build 26300, Rust 1.99.1 (`x86_64-pc-windows-msvc`), MSVC 14.44.

**Decision: go.** Building each crate with `cargo rustc --lib --crate-type cdylib` exports every `tv_*` function declared in the C headers, with no `.def` file, shim or manifest change.

## Method

1. `tools/windows/build-rust-dlls.ps1` runs `cargo rustc --release --lib --crate-type cdylib -p telaviva-core --locked` and the same for `telaviva-inference`, then copies the DLLs, import libraries and PDBs to `native/windows/build/rust/`.
2. `dumpbin /exports` was run on both DLLs directly.
3. `tools/windows/check-dll-exports.ps1` compares the exported `tv_*` names with the function declarations parsed from `include/telaviva_core.h` and `include/telaviva_inference.h`.

## Result

| DLL | Size | Header functions | Exported | Mismatches |
|-----|------|------------------|----------|-----------|
| `telaviva_core.dll` | 638,976 bytes | 10 | 10 | 0 |
| `telaviva_inference.dll` | 325,632 bytes | 7 | 7 | 0 |

Core exports: `tv_abi_version`, `tv_buffer_free`, `tv_camera_lifecycle_create`, `tv_camera_lifecycle_destroy`, `tv_camera_lifecycle_step`, `tv_create`, `tv_destroy`, `tv_shutdown`, `tv_snapshot`, `tv_submit`.

Inference exports: `tv_inference_assess_light_bgra`, `tv_inference_coreml_available`, `tv_inference_create`, `tv_inference_create_with_provider`, `tv_inference_destroy`, `tv_inference_detect_bgra`, `tv_inference_selected_provider`.

The core crate only declares `rlib` and `staticlib` in its manifest; the command-line `--crate-type cdylib` override was enough, so `crates/telaviva-core/Cargo.toml` is unchanged.

A real core was also created, driven and snapshotted through `telaviva_core.dll` with P/Invoke (ABI version 1, `tv_create`, `tv_submit`, `tv_snapshot`, `tv_shutdown`, `tv_destroy`); those snapshots are the fixtures in `native/windows/Tests/fixtures/`.

## Notes

- The first run of `check-dll-exports.ps1` reported 0 exports because its regex did not accept the `name = name` form that `dumpbin` prints; the regex was fixed and the script now reports 10 of 10 and 7 of 7.
- The import libraries come out as `telaviva_core.dll.lib` and `telaviva_inference.dll.lib`; the build script also copies them as `telaviva_core.lib` and `telaviva_inference.lib`.
- This does not verify behavior under the MSVC hybrid CRT of the shell; the DLL boundary keeps the Rust CRT separate from the shell CRT by design (architecture Decision 2).
