# Local YuNet inference

The inference crate runs the pinned OpenCV Zoo YuNet ONNX model through the bundled ONNX Runtime CPU library. The model has a fixed 640×640 input. Frames remain in memory for one call; the detector returns only presence, confidence, or an insufficient-light status.

`runtime/macos-arm64/libonnxruntime.dylib` is the arm64 macOS runtime used by tests and packaged builds. Recreate it from Microsoft's pinned official release with `./tools/inference/fetch-macos-runtime.sh`. The script downloads only when explicitly run and verifies both archive and library checksums. No runtime download occurs in the application.

Before signing a macOS app, copy the verified assets into its bundle with `python3 tools/inference/prepare.py --resources-dir MyApp.app/Contents/Resources --frameworks-dir MyApp.app/Contents/Frameworks`.

The native shell resolves both absolute bundle paths and passes them to `tv_inference_create`; the C ABI is defined in `include/telaviva_inference.h`. For light recovery, `tv_inference_assess_light_bgra` checks illumination without initializing or running the model. CPU remains the default. `tv_inference_create_with_provider` accepts a CoreML candidate only when a measured device profile opts in; unavailable or failed CoreML session setup falls back to CPU, and the selected candidate can be queried per handle. Runtime provider availability does not establish an energy benefit.

Run inference crate checks with `cargo test -p telaviva-inference --all-targets --locked`. The integration test loads the actual bundled runtime and model, checks low-light handling, and runs a synthetic neutral frame through ONNX Runtime. It does not establish face recall or webcam-specific illumination thresholds.
