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
