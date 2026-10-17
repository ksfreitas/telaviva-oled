# Foundation pipeline run (plan T030)

Date: 2026-10-17. Machine: Windows 11 build 26300, Visual Studio 2022 17.14 with the WinUI C++, Roslyn and MSBuild components added (see `toolchain.md`).

## Commands and results

| Step | Command | Result |
|------|---------|--------|
| Rust format | `cargo fmt --all -- --check` | pass |
| Rust lint | `cargo clippy --workspace --all-targets --locked -- -D warnings` | pass |
| Rust tests | `cargo test --workspace --all-targets --locked` | pass (196 tests, macOS-only ONNX smoke test runs 0) |
| Rust DLLs | `tools/windows/build-rust-dlls.ps1` | `telaviva_core.dll`, `telaviva_inference.dll` and import libraries built |
| DLL exports | `tools/windows/check-dll-exports.ps1` | 10 of 10 core and 7 of 7 inference exports match the headers |
| Native build | `msbuild native\windows\TelaViva.sln /restore /p:Platform=x64 /p:Configuration=Release /nr:false` | 0 warnings, 0 errors: `Platform.lib`, `tests.exe`, `TelaViva.exe` |
| Native tests | `tests.exe` (fixtures from `TELAVIVA_TEST_FIXTURES`) | 50 tests, 0 failed |
| Smoke test | `TelaViva.exe --smoke-test` | exit 0 (core ABI 1, inference DLL loads), also from the assembled `dist` folder |
| XAML start | `TelaViva.exe --exit-immediately` | exit 0 in about 260 ms |
| Single instance | second `TelaViva.exe` launch | exits by itself with code 0, first instance stays alive |
| Strings | `TelaViva.exe --print-string <lang> <key> <file>` | en-US, pt-BR, auto resolve; missing key returns `[key]` (S9, see `spike-s9-resources.md`) |
| Tray icons | `tools/windows/make-tray-icons.ps1` | 6 `.ico` files, 5 sizes each, all entries validated and loadable |
| App icon | `tools/windows/make-app-icon.ps1` | `packaging/windows/Resources/AppIcon.ico`, 7 sizes, monitor only on a transparent background (owner decision) |
| Assembly | `packaging/windows/build-app.ps1` | `dist\TelaVivaOLED`: 303 files, 116.7 MB, no `tests.exe` or `.pdb`, ONNX Runtime not fetched yet |
| Idle cost | 60 s sample of the shell with the tray host only | 0.078% of one core, 57.6 MB working set (S10, see `spike-s10-idle.md`) |

## Not verified in this run

- The tray icon appearing in the notification area and its context menu: no automated check; the owner should confirm by running `TelaViva.exe` and looking at the tray (and the overflow menu, since Windows 11 may place a new icon there).
- `AppLifecycle` shutdown drain: nothing creates the core in the app yet, so the quit path only calls the exit callback.
- `.github/workflows/windows.yml`: written and reviewed by reading, never run on GitHub Actions.
- The application icon is not yet embedded in `TelaViva.exe` (task T022b) and the Settings window does not exist yet.
- GPU use and wake-ups of the idle shell.

## Deviations from the plan

- T003: one `tests.exe` with a test registry replaces one executable per test file (MSBuild cannot build several executables from one project with a glob).
- T004: the project keeps `ApplicationType` `Windows Store` because that application type wires the XAML compile passes into the C++ build; this needs the Visual Studio component `Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp`.
- T012: `Platform` references the `Microsoft.Windows.CppWinRT` package (the Windows SDK does not ship the projection headers), without XAML or the Windows App SDK.
- T022a: the Windows application icon is the monitor only, without the rounded-square tile (owner decision), composed from the layered artwork.
