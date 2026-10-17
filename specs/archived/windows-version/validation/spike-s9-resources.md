# Spike S9: .resw string loading in the unpackaged app

Date: 2026-10-17. Machine: Windows 11 build 26300, UI language pt-BR, Windows App SDK 2.5.1 (self-contained, unpackaged).

**Decision: go.** `.resw` resources load in the unpackaged, self-contained app and the language switches at runtime without a restart. The fallback of a string table generated from the `.resw` files (`tools/windows/gen-strings.ps1`) is not needed.

## Method

`telaviva::host::Str(key)` in `Host/Strings.cpp` uses the Windows App SDK `ResourceManager` (`Microsoft.Windows.ApplicationModel.Resources`) with a `ResourceContext` whose `Language` qualifier is set from the app's own language setting (`setLanguage`). `TelaViva.exe --print-string <lang> <key> <outfile>` runs this lookup before any XAML initialization and writes the result to a file (a GUI-subsystem exe cannot print reliably to a console).

## Result

| Language | Key | Exit | Text |
|----------|-----|------|------|
| en-US | `tray.menu.quit` | 0 | Quit |
| en-US | `tray.tooltip.detected` | 0 | TelaViva OLED: face detected |
| pt-BR | `tray.menu.quit` | 0 | Sair |
| pt-BR | `tray.tooltip.detected` | 0 | TelaViva OLED: rosto detectado |
| auto | `tray.menu.quit` | 0 | Sair (this machine's UI language is pt-BR) |
| unknown tag `xx-YY` | `tray.menu.quit` | 0 | Sair (treated as auto) |
| any | `tray.missing.key` | 3 | `[tray.missing.key]` (a missing string is visible, never throws) |

The build produces `TelaViva.pri` next to the executable together with the Windows App SDK `.pri` files, and a `ResourceManager` created with the default constructor is enough (`ResourceManager::Current()` does not exist in the 2.x class).

## Notes

- Keys use dots in the `.resw` (`tray.menu.quit`) and are looked up as the path `Resources/tray/menu/quit`.
- Switching language re-resolves per call because a fresh `ResourceContext` is created for each lookup; UI that is already on screen must be re-rendered by the caller.
- `Tests/strings_keys.cpp` checks that both locales have identical keys, no empty values and no `MacBook` or `Mac` text, and passes.
- Not verified: `x:Uid` binding in XAML (the design assigns text in code), and the behavior when the user changes the Windows display language while the app runs.
