# Inference artifact provenance

| Artifact | Source/version | License | SHA-256 |
|---|---|---|---|
| `assets/model/face_detection_yunet_2023mar.onnx` | OpenCV Zoo YuNet 2023mar, fetched 2026-10-16 | MIT; see `YUNET-LICENSE` | `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4` |
| `tools/inference/runtime/macos-arm64/libonnxruntime.dylib` | Microsoft ONNX Runtime 1.20.1 macOS arm64 official release | MIT; see `ONNXRUNTIME-LICENSE` and `ONNXRUNTIME-THIRD-PARTY-NOTICES.txt` | `530cdb5a0de774677d369a83ec8912b0242e9769acd12d34b61412ee6ae368ae` |
| `ort` Rust crate | `2.0.0-rc.9` | MIT or Apache-2.0 | Pinned in `Cargo.lock` |
| `ort-sys` Rust crate | `2.0.0-rc.9` | MIT or Apache-2.0 | Pinned in `Cargo.lock` |

The ONNX graph has fixed input dimensions `[1, 3, 640, 640]`; the detector letterboxes each source frame to that shape. At inference time the app supplies paths to these bundled files. The detector verifies the model digest before creating a session and never downloads assets or stores frames.

Upstream sources:

- https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet
- https://github.com/microsoft/onnxruntime/releases/tag/v1.20.1
- https://crates.io/crates/ort/2.0.0-rc.9
- https://crates.io/crates/ort-sys/2.0.0-rc.9

## Windows x64 runtime

| Field | Value |
|---|---|
| Source | `https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip` |
| Version | Microsoft ONNX Runtime 1.20.1, Windows x64 official release (CPU provider, default) |
| License | MIT; the license file comes in the archive and `tools/inference/fetch-windows-runtime.ps1` copies it next to the DLL |
| Archive size | 65585496 bytes |
| Archive SHA-256 | `78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581` |
| `onnxruntime.dll` SHA-256 | `4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0` |
| Computed | 2026-10-17 |

The pins were computed locally with `tools/inference/fetch-windows-runtime.ps1 -ComputeOnly` from a fresh download into a new empty folder. The script only read the archive and extracted DLL for hashing; nothing was executed or imported. The GitHub release assets publish no digests, so these values are the only reference. The macOS entries above are unchanged.

### Windows x64 DirectML runtime variant

| Field | Value |
|---|---|
| Source | `https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1` (NuGet package `Microsoft.ML.OnnxRuntime.DirectML`) |
| Version | Microsoft ONNX Runtime 1.20.1, same version as the CPU pin; reserved variant, not shipped by default |
| License | MIT for ONNX Runtime; `LICENSE` and `ThirdPartyNotices.txt` come from the package, and `tools/inference/fetch-windows-runtime.ps1 -Variant directml` copies them next to the DLL |
| DirectML | `DirectML.dll` is not redistributed (`directml_dll = "system"`). The in-box Windows System32 component is used and is never extracted or copied |
| Archive size | to be recorded |
| Archive SHA-256 | to be recorded |
| `onnxruntime.dll` SHA-256 | to be recorded |
| Computed | to be recorded |

The pins are `TODO-compute` in `assets/model/manifest.toml`; `tools/inference/fetch-windows-runtime.ps1 -Variant directml` refuses to install until they are recorded from `-ComputeOnly` output. The DLL is installed to `tools/inference/runtime/windows-x64-directml/`, separate from the CPU runtime.

### Windows x64 package contents

- The shipped package carries the CPU `onnxruntime.dll` only. `onnxruntime_providers_shared.dll` is not shipped: `build-app.ps1` copies only `onnxruntime.dll`.
- `DirectML.dll` is not redistributed. The Windows App SDK ML package (Microsoft.Windows.AI.MachineLearning 2.1.74) carries a copy, which `packaging/windows/build-app.ps1` removes from the package root before zipping through its `$unusedComponents` list. The app uses the in-box `System32` component, as this document states above. The removed-file list is maintained in the script.
