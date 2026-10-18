# Windows release package

The Windows release is an unpackaged zip with a per-user installer. It does not
use MSIX, and neither the build nor the installer needs administrator rights.
Build output is `dist\TelaVivaOLED-<version>-win-x64.zip`.

## Build

### One command build

From the repository root, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1
```

The script checks the prerequisites, fetches the pinned ONNX Runtime on the first
run, builds the app, runs `tests.exe`, and prints the zip's SHA-256. It runs from
any directory. Options:

- `-Configuration Release|Debug` selects the build configuration (default `Release`).
- `-SkipTests` skips `tests.exe`.
- `-NoZip` writes `dist\TelaVivaOLED` only, without the zip.
- `-Help` prints the usage and exits.

Prerequisites:

- Windows 11 x64 with Windows PowerShell 5.1.
- Rust stable for `x86_64-pc-windows-msvc`, with `cargo` and `rustc` on PATH or in `%USERPROFILE%\.cargo\bin`.
- Visual Studio 2022 with the Desktop development with C++ workload, the Roslyn compiler (`Microsoft.VisualStudio.Component.Roslyn.Compiler`) and Windows 11 SDK 10.0.26100.
- The Windows App SDK C++ component `Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp`. Add it in the Visual Studio Installer; the script never installs components.

### Build steps

From the repository root, after the solution builds:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File packaging\windows\build-app.ps1
```

The script builds the Rust DLLs and the solution, assembles `dist\TelaVivaOLED`,
writes the zip next to it and prints the zip's SHA-256. Use `-NoZip` to stop
after the folder. The zip holds the same folder at its root, plus the installer,
the uninstaller, this README and `licenses\`.

## Install

1. Extract the zip to any folder, for example `Downloads\TelaVivaOLED`.
2. In that folder, run:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-TelaVivaOLED.ps1
   ```

The app is installed to `%LOCALAPPDATA%\Programs\TelaVivaOLED`, and a Start menu
shortcut is created. The app is also listed in Settings > Apps. Run the installer
again to upgrade in place. Quit TelaViva OLED first, because the installer stops a
running copy from the install folder.

Options:

- `-InstallDir <path>` installs somewhere other than the default folder.
- `-NoStartMenu` skips the Start menu shortcut.
- `-LaunchAtLogin` also starts the app at sign-in. You can turn this on in the app instead.
- `-WhatIf` lists the changes without making them.

## Uninstall

Use Settings > Apps, or run `Uninstall-TelaVivaOLED.ps1` from the install folder.
It stops the app, removes the launch-at-sign-in entry, the Start menu shortcut,
the Settings > Apps entry and the install folder.

Your settings, calibration and logs are kept. To delete them too, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Programs\TelaVivaOLED\Uninstall-TelaVivaOLED.ps1" -RemoveUserData
```

Interactive runs ask you to type `DELETE` before that folder is removed.

For scripts and CI, add `-Quiet`. It never prompts and does not pause at the end.
With `-Quiet -RemoveUserData` the data folder is deleted without the `DELETE`
question, so only pass that pair when you mean it. The exit code is `0` when every
step succeeded and `1` when one failed; the failed step is named on stderr. Add
`-WhatIf` to preview the changes without making them.

## Windows SmartScreen

Release builds are not code-signed yet, so Windows may show "Windows protected
your PC" when you first run `TelaViva.exe`. Choose **More info**, then **Run
anyway**. Only do this for a zip you downloaded from the project and checked
against the SHA-256 printed by the build.

## Where data lives

- App files: `%LOCALAPPDATA%\Programs\TelaVivaOLED` (installer default)
- Settings, calibration and logs: `%LOCALAPPDATA%\TelaVivaOLED`
- Launch at sign-in: the `TelaVivaOLED` value under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- Settings > Apps entry: `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\TelaVivaOLED`

## Pin the tray icon

On Windows 11, open Settings > Personalization > Taskbar > Other system tray
icons, then turn on TelaViva OLED. Windows hides new tray icons by default.

## Privacy

TelaViva OLED does not send telemetry, and the app makes no network requests of its own.
Camera frames are processed locally.
