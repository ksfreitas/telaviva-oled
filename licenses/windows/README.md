# Windows license texts

Index of license and notice files for the Windows build. The inventory is in `licenses/DEPENDENCIES.md` (Windows section).

## Copied

| File | Origin | License |
|---|---|---|
| `WINDOWSAPPSDK-LICENSE.txt` | `%USERPROFILE%\.nuget\packages\microsoft.windowsappsdk\2.5.1\license.txt` (Microsoft.WindowsAppSDK 2.5.1 NuGet package) | Microsoft Software License Terms (not MIT); section 3(a)(i) allows redistribution of files binplaced by the NuGet package |
| `ONNXRUNTIME-LICENSE` | `tools/inference/runtime/windows-x64/LICENSE` (ONNX Runtime 1.20.1 Windows x64 release, CPU) | MIT, Copyright (c) Microsoft Corporation |
| `ONNXRUNTIME-DIRECTML-LICENSE` | `tools/inference/runtime/windows-x64-directml/LICENSE` (Microsoft.ML.OnnxRuntime.DirectML 1.20.1 package) | MIT, Copyright (c) Microsoft Corporation |

The two ONNX Runtime LICENSE files were read in full and their text is identical; they were not compared byte for byte.

## Not copied yet

These files exist but are too large (about 333 KB each) to copy with the tools used for this folder. Copy them with a shell before release (`Copy-Item`, same names, into this folder).

| File | Origin | License |
|---|---|---|
| `ThirdPartyNotices.txt` (ONNX Runtime CPU) | `tools/inference/runtime/windows-x64/ThirdPartyNotices.txt` | Third-party notices from the ONNX Runtime package |
| `ThirdPartyNotices.txt` (ONNX Runtime DirectML) | `tools/inference/runtime/windows-x64-directml/ThirdPartyNotices.txt` | Third-party notices from the ONNX Runtime DirectML package |
| `WINDOWSAPPSDK-NOTICE.txt` | `%USERPROFILE%\.nuget\packages\microsoft.windowsappsdk\2.5.1\NOTICE.txt` | Microsoft Windows App SDK notices (Microsoft Software License Terms) |

## Not in this folder

- C++/WinRT (`Microsoft.Windows.CppWinRT` 3.0.260818.1) is MIT and build-time only. Its `LICENSE` is in the NuGet cache and is not copied here because it is not redistributed.
- `windows-x64/Privacy.md` is in the ONNX Runtime runtime folder. It is not copied here; its origin was not checked.
