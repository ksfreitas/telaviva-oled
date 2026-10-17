# Windows toolchain notes

Recorded 2026-10-17 on the development machine (Windows 11 Pro build 26300, AMD Ryzen 7 9800X3D, desktop). These are facts observed while building, not claims about other machines.

## Versions

| Item | Value |
|------|-------|
| Rust | 1.99.1, host `x86_64-pc-windows-msvc`, `%USERPROFILE%\.cargo\bin` (on the user PATH; a session opened earlier may need a restart) |
| Visual Studio | 2022 Community 17.14.42, MSBuild 17.14.60 |
| MSVC | 14.44.35207, toolset v143 |
| Windows SDK | 10.0.26100.0 |
| Windows App SDK | `Microsoft.WindowsAppSDK` 2.5.1 (resolves WinUI 2.3.9) |
| C++/WinRT | `Microsoft.Windows.CppWinRT` 3.0.260818.1 |
| .NET SDK | none installed (runtime only) |

## Results so far

- `cargo check` and `cargo test --workspace --all-targets --locked` pass on Windows (196 tests; the macOS-only ONNX smoke test runs 0 tests).
- `native/windows/Platform` (static library) and `native/windows/Tests` (`tests.exe`) build with `msbuild /restore` in Release x64.
- `native/windows/TelaViva` (WinUI 3, unpackaged, self-contained) restores and runs metadata merge, but the XAML compile passes never run, so `App.xaml.g.h` is not generated and C++ compilation of `App.xaml.cpp` fails with `C1083`.

## Findings that shaped the project files

1. **No .NET SDK.** The inline MSBuild tasks in `Microsoft.WindowsAppSDK` and `Microsoft.WindowsAppSDK.ML` (`GetUnreferencedDependencies`, `GetWindowsMLUnreferencedDependencies`) cannot compile (`MSB4036`). Both checks only apply to `packages.config` projects, so `TelaViva.vcxproj` sets `WindowsAppSDKVerifyTransitiveDependencies` and `WindowsAppSDKMLVerifyTransitiveDependencies` to `false`.
2. **Empty static library.** A static library with no `.cpp` produces no `.lib`, which broke the `Tests` link; `Platform/Platform.cpp` keeps it non-empty until real sources exist.
3. **No UWP application type.** `MSBuild\Microsoft\VC\v170\Application Type` is empty, so the sample's `ApplicationType` `Windows Store` leaves `TargetPlatformVersion` empty (`MSB8036` from the C++/WinRT target). `TelaViva.vcxproj` is treated as an unpackaged desktop project instead, with no `ApplicationType`.
4. **Missing WinUI C++ tooling (blocker for the UI project).** `MSBuild\Microsoft\WindowsXaml` does not exist and the components `Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp` and `Microsoft.VisualStudio.Workload.Universal` are not installed. The native XAML targets that wire `MarkupCompilePass1` and `MarkupCompilePass2` into a C++ build come with that tooling, so the XAML compiler never runs. The Visual Studio Installer must add `Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp` (elevation required) before the UI project can build.

5. **Update 2026-10-17: WinUI C++ tooling installed.** `Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp` was added with the Visual Studio Installer (`setup.exe modify --add ... --passive --norestart`, elevated). The first attempt exited with 8006 because leftover `MSBuild`, `mspdbsrv` and `vctip` processes from earlier builds triggered the `VSProcessesRunning` pre-check; the retry after stopping them exited 0. After that, `MSBuild\Microsoft\WindowsXaml` and the `Windows Store` application type exist, so `TelaViva.vcxproj` keeps `ApplicationType` `Windows Store` and lists the runtime identifiers `win10-x64;win-x64`.
6. **Platform needs the C++/WinRT package.** The Windows SDK does not ship `winrt/*.h` projection headers; `cppwinrt.exe` generates them from the `Microsoft.Windows.CppWinRT` package. `Platform.vcxproj` therefore references that package (no XAML, no Windows App SDK) so platform code can use WinRT classes such as `Windows.Data.Json`.
7. **No Roslyn compiler in the VS install (second blocker).** `MSBuild\Current\Bin\Roslyn` does not exist and `Microsoft.VisualStudio.Component.Roslyn.Compiler`, `Microsoft.Component.MSBuild` and the NuGet component were missing, so every inline MSBuild task failed to compile (`GenerateAppManifestFromAppxFragments`, which the self-contained Windows App SDK build needs). Those components were added with the same elevated installer command.
8. **Stray character in `pch.cpp`.** The first build exposed `a// Copyright` at the start of `TelaViva/pch.cpp`, which broke every compile; a scan of all new source files found no other leading-character defects. Builds now use `msbuild -nr:false` so no MSBuild nodes stay alive and block the VS installer.

## Open

- Re-run the full solution build after the WinUI C++ component group is installed; if `ApplicationType` `Windows Store` is needed again, restore it (finding 3 is then moot).
- Check in CI that `windows-latest` has the same components, or add a setup step.
- Confirm `/W4` and `/std:c++20` actually reach `cl` (the `Directory.Build.props` `ItemDefinitionGroup` is read before `Microsoft.Cpp.props`).
