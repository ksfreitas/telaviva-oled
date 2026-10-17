# Third-party Rust dependencies

Generated from `cargo metadata --locked` for the current Cargo.lock. All license expressions below describe open-source third-party packages; they do not choose a license for TelaViva itself. Native Apple frameworks and Xcode are platform facilities. Model and inference runtime records are maintained separately in `licenses/inference/` and `assets/model/`.

| Package | Version | Declared license |
|---|---|---|
| autocfg | 1.5.1 | Apache-2.0 OR MIT |
| block-buffer | 0.10.4 | MIT OR Apache-2.0 |
| cfg-if | 1.0.5 | MIT OR Apache-2.0 |
| cpufeatures | 0.2.17 | MIT OR Apache-2.0 |
| crypto-common | 0.1.7 | MIT OR Apache-2.0 |
| digest | 0.10.7 | MIT OR Apache-2.0 |
| generic-array | 0.14.7 | MIT |
| itoa | 1.0.18 | MIT OR Apache-2.0 |
| libc | 0.2.190 | MIT OR Apache-2.0 |
| libloading | 0.8.9 | ISC |
| matrixmultiply | 0.3.11 | MIT/Apache-2.0 |
| memchr | 2.8.3 | Unlicense OR MIT |
| ndarray | 0.16.1 | MIT OR Apache-2.0 |
| num-complex | 0.4.6 | MIT OR Apache-2.0 |
| num-integer | 0.1.47 | MIT OR Apache-2.0 |
| num-traits | 0.2.19 | MIT OR Apache-2.0 |
| once_cell | 1.21.4 | MIT OR Apache-2.0 |
| ort | 2.0.0-rc.9 | MIT OR Apache-2.0 |
| ort-sys | 2.0.0-rc.9 | MIT OR Apache-2.0 |
| pin-project-lite | 0.2.17 | Apache-2.0 OR MIT |
| pkg-config | 0.3.34 | MIT OR Apache-2.0 |
| portable-atomic | 1.15.0 | Apache-2.0 OR MIT |
| portable-atomic-util | 0.2.8 | Apache-2.0 OR MIT |
| proc-macro2 | 1.0.107 | MIT OR Apache-2.0 |
| quote | 1.0.47 | MIT OR Apache-2.0 |
| rawpointer | 0.2.1 | MIT/Apache-2.0 |
| serde | 1.0.229 | MIT OR Apache-2.0 |
| serde_core | 1.0.229 | MIT OR Apache-2.0 |
| serde_derive | 1.0.229 | MIT OR Apache-2.0 |
| serde_json | 1.0.154 | MIT OR Apache-2.0 |
| sha2 | 0.10.9 | MIT OR Apache-2.0 |
| syn | 3.0.9 | MIT OR Apache-2.0 |
| tracing | 0.1.44 | MIT |
| tracing-core | 0.1.36 | MIT |
| typenum | 1.20.1 | MIT OR Apache-2.0 |
| unicode-ident | 1.0.27 | (MIT OR Apache-2.0) AND Unicode-3.0 |
| version_check | 0.9.5 | MIT/Apache-2.0 |
| windows-link | 0.2.1 | MIT OR Apache-2.0 |
| zmij | 1.0.23 | MIT |

License/notice files shipped in the downloaded crate archives were copied into `licenses/rust/<package>-<version>/`. Registry checksums are recorded in Cargo.lock. This inventory must be regenerated if the lockfile changes.

Packages without a top-level notice file in the registry archive: none.

## Windows native dependencies

Records for the Windows build under `native/windows/`, `tools/inference/` and the `dist\TelaVivaOLED` output. Versions come from `native/windows/**/*.vcxproj`, `assets/model/manifest.toml`, and the NuGet package metadata in the local package cache. License texts shipped with the app are in `licenses/windows/` (index in its `README.md`).

| Component | Version | License | Source | Redistribution |
|---|---|---|---|---|
| Microsoft.WindowsAppSDK (Windows App SDK) | 2.5.1 | Microsoft Software License Terms (`license.txt` in the package; not MIT) | https://github.com/microsoft/windowsappsdk (project URL in the nuspec); NuGet `Microsoft.WindowsAppSDK` 2.5.1 | Self-contained deployment (`WindowsAppSDKSelfContained`). Files binplaced by the package may be redistributed (license section 3(a)(i)); they are the runtime DLLs, `.winmd`, `.pri` and language resources in `dist\TelaVivaOLED`. |
| WinUI 3 (Microsoft.WindowsAppSDK.WinUI) | 2.3.9 | Microsoft Software License Terms (package `license.txt`) | Part of the Windows App SDK; NuGet `Microsoft.WindowsAppSDK.WinUI` 2.3.9 | Redistributed as part of the Windows App SDK runtime. |
| C++/WinRT (Microsoft.Windows.CppWinRT) | 3.0.260818.1 | MIT (package `LICENSE`) | https://github.com/Microsoft/cppwinrt | Build-time only (projection headers). Not redistributed. |
| ONNX Runtime, CPU (`onnxruntime.dll`) | 1.20.1 | MIT (`licenses/windows/ONNXRUNTIME-LICENSE`) | https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip | Redistributed: `onnxruntime.dll` is copied to `dist\TelaVivaOLED` (default provider, pinned in `assets/model/manifest.toml`). `onnxruntime_providers_shared.dll` is not shipped: `build-app.ps1` copies only `onnxruntime.dll` (CPU runtime only). |
| ONNX Runtime, DirectML package | 1.20.1 | MIT (`licenses/windows/ONNXRUNTIME-DIRECTML-LICENSE`) | https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1 | Reserved variant, not shipped by default. Its `onnxruntime.dll` would be redistributed. |
| DirectML.dll | Not pinned by this repo | Microsoft Software License Terms of the package that carries the file (see the next row) | Carried by Microsoft.Windows.AI.MachineLearning 2.1.74 (NuGet cache, `runtimes\win-x64\native`) | Not redistributed. `packaging/windows/build-app.ps1` removes it from the package root before zipping (`$unusedComponents`), so the in-box System32 copy is used. |
| Microsoft.Windows.AI.MachineLearning | 2.1.74 | Microsoft Software License Terms (package `license.txt`) | NuGet `Microsoft.Windows.AI.MachineLearning` 2.1.74 (via Microsoft.WindowsAppSDK.ML 2.1.94) | Binplaced files that match `$unusedComponents` in `build-app.ps1` are removed before zipping (including `DirectML.dll`); the remaining files are redistributed. |
| YuNet face detection model | 2023mar | MIT (see `licenses/inference/YUNET-LICENSE` and `assets/model/manifest.toml`) | https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet | Redistributed: copied to `dist\TelaVivaOLED\models`. |
| Windows SDK Build Tools (Microsoft.Windows.SDK.BuildTools) | 10.0.26100.4654 | Microsoft (license at https://aka.ms/WinSDKLicenseURL, per the nuspec) | https://aka.ms/WinSDKProjectURL (project URL in the nuspec) | Build-time tooling. Not expected in the app output; not checked file by file. |
| Microsoft Visual C++ runtime and STL | MSVC toolset v143 (Visual Studio 2022) | Microsoft Visual Studio terms (not a redistributable pin) | `native/windows/HybridCRT.props` | Hybrid CRT: the C++ runtime and STL are linked statically (`/MT` Release, `/MTd` Debug). The UCRT is linked dynamically through its import library. Nothing from the Visual C++ redistributable is copied by `packaging/windows/build-app.ps1`. |

Dependency packages restored with Microsoft.WindowsAppSDK 2.5.1 (all Microsoft Software License Terms, per their nuspec `license.txt`; only the 2.5.1 file was read): Microsoft.WindowsAppSDK.Base 2.0.4, Microsoft.WindowsAppSDK.Foundation 2.3.12, Microsoft.WindowsAppSDK.InteractiveExperiences 2.1.9, Microsoft.WindowsAppSDK.DWrite 2.1.0, Microsoft.WindowsAppSDK.Widgets 2.0.5, Microsoft.WindowsAppSDK.AI 2.5.5, Microsoft.WindowsAppSDK.ML 2.1.94, Microsoft.WindowsAppSDK.Search 2.5.5, Microsoft.WindowsAppSDK.Runtime 2.5.1. Microsoft.Web.WebView2 1.0.3719.77 is also in the cache (its `LICENSE.txt` is not in this folder). The package that pulls it in was not identified.

The Rust crates that ship in `telaviva_core.dll` and `telaviva_inference.dll` are the rows in the Rust table above; this section adds no Rust rows.

No asset outside the open-source set is bundled by the Windows build, except the Microsoft Software License Terms components listed above (Windows App SDK, WinUI 3 and the Windows App SDK packages, including Microsoft.Windows.AI.MachineLearning), which are redistributed under their own terms.
