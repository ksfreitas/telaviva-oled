# macOS ONNX Runtime CoreML Spike

- Machine: Apple M4 Pro Mac (`arm64`, `kslap.local`)
- OS build: macOS 27.0.1, build `26A434` (`sw_vers`)
- Driver: Apple system CoreML; no separate driver version applies
- Date: 2026-10-18
- Initial runtime: official `onnxruntime-osx-arm64-1.22.0.tgz`; archive SHA-256 `cab6dcbd77e7ec775390e7b73a8939d45fec3379b017c7cb74f5b204c1a1cc07`; dylib SHA-256 `2b885992d3d6fa4130d39ec84a80d7504ff52750027c547bb22c86165f19406a`
- Accepted runtime route: official `onnxruntime-osx-arm64-1.23.0.tgz`; archive SHA-256 `8182db0ebb5caa21036a3c78178f17fabb98a7916bdab454467c8f4cf34bcfdf`; dylib SHA-256 `d3859aecdb70ea099f5b5f4185fe16f0527c6680b18731e6e96fc971ec767cca`
- Binding: `ort =2.0.0-rc.9`, `ort-sys =2.0.0-rc.9`
- Model: `assets/model/face_detection_yunet_2023mar.onnx`
- Throwaway project: `/tmp/telaviva-ort-spike.vd6ClL/` (`Cargo.toml`, `Cargo.lock`, and `src/main.rs`); both archives and extracted dylibs are in the same directory.

## Initial 1.22.0 result

The 1.22.0 archive exports `_OrtSessionOptionsAppendExecutionProvider_CoreML`, verified with:

```sh
nm -gU /tmp/telaviva-ort-spike.vd6ClL/onnxruntime-osx-arm64-1.22.0/lib/libonnxruntime.1.22.0.dylib | grep CoreML
```

The throwaway Rust binary called `GetApi(20)`, obtained the session-options pointer from `SessionBuilder::ptr()` (`AsPointer`), registered provider `CoreML` with `MLComputeUnits=CPUAndNeuralEngine`, and committed the YuNet session. All three setup steps succeeded. The process then aborted at exit with `std::__1::system_error: mutex lock failed: Invalid argument` (exit status 134). CPU-only sessions also reproduced the failure, including with the session and dynamic-library handle intentionally leaked.

LLDB caught `__cxa_throw` during exit. The relevant backtrace:

```text
std::__1::__throw_system_error
std::__1::mutex::lock
onnxruntime::webgpu::WebGpuContextFactory::Cleanup()
std::unique_ptr<OrtEnv>::~unique_ptr()
__cxa_finalize_ranges
exit
```

This identifies an ONNX Runtime macOS teardown bug independent of CoreML and session drop order. It matches Microsoft's reports for macOS exit aborts with ONNX Runtime 1.22.0 and the Rust `ort` crate ([issue #25038](https://github.com/microsoft/onnxruntime/issues/25038), [issue #24579](https://github.com/microsoft/onnxruntime/issues/24579)).

## Compatible runtime route

The 1.23.0 runtime was fetched from `https://github.com/microsoft/onnxruntime/releases/download/v1.23.0/onnxruntime-osx-arm64-1.23.0.tgz` and verified against the hashes above. With `ort` and `ort-sys` still at 2.0.0-rc.9, the same scratch project was run once CPU-only and once with the CoreML CPUAndNeuralEngine provider. Both runs returned successful `GetApi(20)` and YuNet session creation, and both exited with status 0. The CoreML append returned a null `OrtStatusPtr`.

The scratch source is `/tmp/telaviva-ort-spike.vd6ClL/src/main.rs`; the manifest and lock file are in the same directory. The project's `tools/inference/fetch-macos-runtime.sh` now pins the 1.23.0 archive and exposes `--check` to verify the installed dylib without network access.

**T004/T005: passed with an owner-approved runtime adjustment to ONNX Runtime 1.23.0.** The original 1.22.x runtime failed its exit gate; 1.23.0 loads through the existing binding, registers CoreML, creates the YuNet session, and exits cleanly. This session setup spike does not establish which device CoreML assigns model operators to, nor does it replace the opt-in production ABI probe's ALL, Neural Engine, and GPU inference results and timings.
