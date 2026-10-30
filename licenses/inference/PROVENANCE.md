# Inference artifact provenance

| Artifact | Source/version | License | SHA-256 |
|---|---|---|---|
| `assets/model/face_detection_yunet_2023mar.onnx` | OpenCV Zoo YuNet 2023mar, fetched 2026-10-16 | MIT; see `YUNET-LICENSE` | `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4` |
| `assets/model/face_detection_retinaface_mb025.onnx` | RetinaFace MobileNet0.25 ONNX (1729290 bytes), fetched 2026-10-29 from https://media.axelera.ai/artifacts/model_cards/weights/others/object_detection/Retinaface_mb0.25_pytorch.onnx (Axelera Voyager SDK model zoo, third-party re-host); MD5 `9232a08b7056937f63ba26b6a3606546` | MIT per the Axelera model card; code MIT (biubug6/Pytorch_Retinaface), see `RETINAFACE-LICENSE`. Trained on WIDER FACE, whose dataset license is CC BY-NC-ND per its site; the same caveat as YuNet, a legal check left to the owner | `f4c83825f41dba8e17b522377f77cb7e2b69ec4df75168d4cb1b18dcaf03b54c` |
| `tools/inference/runtime/macos-arm64/libonnxruntime.dylib` | Microsoft ONNX Runtime 1.23.0 macOS arm64 official release | MIT; see `ONNXRUNTIME-LICENSE` and `ONNXRUNTIME-THIRD-PARTY-NOTICES.txt` | `d3859aecdb70ea099f5b5f4185fe16f0527c6680b18731e6e96fc971ec767cca` |
| `ort` Rust crate | `2.0.0-rc.9` | MIT or Apache-2.0 | Pinned in `Cargo.lock` |
| `ort-sys` Rust crate | `2.0.0-rc.9` | MIT or Apache-2.0 | Pinned in `Cargo.lock` |

The ONNX graph has fixed input dimensions `[1, 3, 640, 640]`; the detector letterboxes each source frame to that shape. At inference time the app supplies paths to these bundled files. The detector verifies the model digest before creating a session and never downloads assets or stores frames.

Upstream sources:

- https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet
- https://media.axelera.ai/artifacts/model_cards/weights/others/object_detection/Retinaface_mb0.25_pytorch.onnx (RetinaFace MobileNet0.25; re-host of the export, kept with its SHA-256 and MD5)
- https://github.com/biubug6/Pytorch_Retinaface (weights origin and MIT code)
- https://github.com/microsoft/onnxruntime/releases/tag/v1.23.0
- https://crates.io/crates/ort/2.0.0-rc.9
- https://crates.io/crates/ort-sys/2.0.0-rc.9

## Windows x64 runtime

| Field | Value |
|---|---|
| Source | `https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip` |
| Version | Microsoft ONNX Runtime 1.20.1, Windows x64 official release (CPU provider). Not shipped: the package ships the DirectML variant below. It stays pinned as a reference and for `-Variant cpu` |
| License | MIT; the license file comes in the archive and `tools/inference/fetch-windows-runtime.ps1` copies it next to the DLL |
| Archive size | 65585496 bytes |
| Archive SHA-256 | `78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581` |
| `onnxruntime.dll` SHA-256 | `4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0` |
| Computed | 2026-10-17 |

The pins were computed locally with `tools/inference/fetch-windows-runtime.ps1 -ComputeOnly` from a fresh download into a new empty folder. The script only read the archive and extracted DLL for hashing; nothing was executed or imported. The GitHub release assets publish no digests, so these values are the only reference. The Windows runtime entries remain unchanged.

### Windows x64 DirectML runtime variant

| Field | Value |
|---|---|
| Source | `https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1` (NuGet package `Microsoft.ML.OnnxRuntime.DirectML`) |
| Version | Microsoft ONNX Runtime 1.20.1, same version as the CPU pin. This is the only runtime the Windows package ships, as `onnxruntime.dll`. It runs the CPU provider by default and the DirectML provider for the selected inference device |
| License | MIT for ONNX Runtime; `LICENSE` and `ThirdPartyNotices.txt` come from the package, and `tools/inference/fetch-windows-runtime.ps1 -Variant directml` copies them next to the DLL |
| DirectML | `DirectML.dll` is not redistributed (`directml_dll = "system"`). The in-box Windows System32 component is used and is never extracted or copied |
| Archive size | 15844459 bytes |
| Archive SHA-256 | `6763468507b7cfc777b1334b3e174c11a540ddacb7bd4354bc2e0ec89e56eec2` |
| `onnxruntime.dll` SHA-256 | `903c92c54acc57caa77d44d4c856c829eb82f5cf6755b47367a8153bd3fabeb6` (14682160 bytes) |
| Computed | 2026-10-17 |

The pins are recorded in `assets/model/manifest.toml`; `tools/inference/fetch-windows-runtime.ps1 -Variant directml` installs the DLL to `tools/inference/runtime/windows-x64-directml/` and refuses to install on a mismatch. On 2026-10-18 the `onnxruntime.dll` in a freshly built `dist\TelaVivaOLED` had the same SHA-256 as the pin. `d3d12.dll`, `dxgi.dll`, `dxcore.dll` and `DirectML.dll` are delay-loaded by this DLL (checked with `dumpbin /dependents`), so it loads and runs the CPU provider even when DirectML is missing.

### Windows x64 package contents

- The shipped package carries the DirectML build of `onnxruntime.dll` only. `onnxruntime_providers_shared.dll` is not shipped: `build-app.ps1` copies only `onnxruntime.dll`.
- `DirectML.dll` is not redistributed. The Windows App SDK ML package (Microsoft.Windows.AI.MachineLearning 2.1.74) carries a copy, which `packaging/windows/build-app.ps1` removes from the package root before zipping through its `$unusedComponents` list. The app uses the in-box `System32` component, as this document states above. The removed-file list is maintained in the script.
