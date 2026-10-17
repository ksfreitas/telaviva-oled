# macOS UX parity checklist for the Windows shell

Date: 2026-10-17. Source: a read of the project history (`CHANGELOG.md`, `adr/`, `specs/archived/`) and of the macOS UI code (`native/macos/TelaViva/*.swift`, `Localization.swift`). The goal is that refinements made on the Mac over time are not forgotten on Windows. Parity is in behavior and wording, not in layout: the Windows interface keeps the Windows 11 idiom (spec FR-034). Windows tasks that carry each item are listed by ID; new tasks T150 to T169 were added to `plan.md` for the items that were missing.

## Decisions on deviations

| Topic | macOS | Windows |
|-------|-------|---------|
| Tray right click | opens the same popover | native menu with Settings and Quit (Windows idiom); left click opens the flyout |
| Brightness keys and brightness step | routed, with a step setting | not handled (decision Q5); no brightness step control |
| Permission rows (Accessibility, Input Monitoring) and their strings | shown in Keyboard tab | dropped; the hook needs no permission |
| Fine key step | Shift+Option, 1 point | Shift, 1 point (T163) |
| Value formats | `NN %` (presence threshold, with a space), `%.2g s` (rounds 1.25 to 1.2) | consistent `N%`, `N s` with up to two decimals for the return interval, `N pt` for the key step; do not copy the rounding quirk |
| Settings window | 640x720 default, 560x560 minimum, reused | wide Windows Settings layout, default 880x640 DIP, minimum 640x480, centered on the monitor of the tray icon, reused while open (T157); the owner may change the numbers |
| Unavailable control | label "<Brightness or Volume> unavailable" with no reason | reason text under the slider (spec FR-002 asks for accurate reasons) |
| Automatic brightness tab name | "Automatic brightness" | keep the name; the Windows setting itself is "adaptive brightness" and notices say so |
| Internal panel wording | "MacBook", "built-in" | "Laptop" (en-US) and "Notebook" (pt-BR); never "Mac" or "MacBook" |

## Windows wording for strings that name the Mac

Port every key of `native/macos/TelaViva/Localization.swift` to the `.resw` files with the same key names, except the dropped keys below, and use these Windows texts for the flagged ones (en-US / pt-BR):

| Key | en-US | pt-BR |
|-----|-------|-------|
| `ambient.toggle` | Follow the Laptop screen brightness | Seguir o brilho da tela do notebook |
| `ambient.help` | Calibrated external monitors follow the Laptop screen brightness, including adaptive brightness. | Os monitores externos calibrados acompanham o brilho da tela do notebook, inclusive o brilho adaptável. |
| `ambient.unavailable.noBuiltInDisplay` | This PC has no built-in laptop display. | Este computador não tem tela de notebook integrada. |
| `ambient.unavailable.brightnessNotAdjustable` | This PC has no built-in laptop display with adjustable brightness. | Este computador não tem tela de notebook integrada com brilho ajustável. |
| `ambient.unavailable.apiUnavailable` | The Windows brightness interface is not available on this system. | A interface de brilho do Windows não está disponível neste sistema. |
| `ambient.unavailable.displayOffline` | The built-in laptop display is off, asleep, mirrored, or its lid is closed. Following resumes on its own. | A tela de notebook está desligada, em suspensão, espelhada ou com a tampa fechada. O recurso volta sozinho. |
| `ambient.unavailable.unknown` | Following the Laptop brightness is not available right now. | Seguir o brilho do notebook não está disponível agora. |
| `calibration.help` | The monitor is now at this brightness. Adjust the Laptop screen until both screens look the same, then select Done to save the point. | O monitor está neste brilho. Ajuste a tela do notebook até as duas telas ficarem iguais e selecione Concluir para salvar o ponto. |
| `calibration.slider` | Laptop brightness | Brilho do notebook |
| `calibration.point.row` | Monitor %d%% → Laptop %d%% | Monitor %d%% → Notebook %d%% |
| `calibration.notice.autoUnknown` | Adaptive brightness could not be checked; the Laptop brightness may drift while you calibrate. | Não foi possível verificar o brilho adaptável; o brilho do notebook pode variar durante a calibração. |
| `calibration.notice.autoNotChanged` | Adaptive brightness could not be turned off; the Laptop brightness may drift while you calibrate. | Não foi possível desativar o brilho adaptável; o brilho do notebook pode variar durante a calibração. |
| `calibration.notice.builtInUnreadable` | The Laptop brightness could not be read, so calibration did not start. | Não foi possível ler o brilho do notebook; a calibração não foi iniciada. |
| `calibration.notice.builtInWriteFailed` | The Laptop brightness could not be changed, so calibration stopped. | Não foi possível alterar o brilho do notebook; a calibração foi interrompida. |
| `calibration.notice.builtInUnavailable` | Calibration is not possible while the Laptop screen is off or its lid is closed. | A calibração não pode ser feita com a tela do notebook desligada ou fechada. |
| `calibration.notice.recoveryFailed` | The previous calibration could not be fully restored. Check the Laptop brightness and the adaptive brightness setting. | A calibração anterior não pôde ser totalmente restaurada. Verifique o brilho do notebook e o ajuste de brilho adaptável. |
| `hardwareKeys.toggle` | Route volume and mute keys to external monitors | Direcionar teclas de volume e mudo a monitores externos |
| `hardwareKeys.help` | Volume and mute keys adjust an external monitor only while it is the default sound output; otherwise Windows controls the sound, for example with headphones. Mute sets the volume to zero and the next press restores it. Step size is set below (1 to 15 points per press); Shift: 1 point. | As teclas de volume e de mudo só ajustam um monitor externo enquanto ele for a saída de som padrão; caso contrário, o Windows controla o som, por exemplo com fones. O mudo zera o volume e o próximo toque o restaura. O passo é definido abaixo (1 a 15 pontos por toque); Shift: 1 ponto. |
| `hardwareKeys.status.listenerUnavailable` | Keyboard controls are unavailable; Windows continues handling the keys. | O controle pelo teclado está indisponível; o Windows continua tratando as teclas. |

Dropped keys (macOS only): `hardwareKeys.brightnessStep`, `hardwareKeys.requestPermission`, `hardwareKeys.openAccessibility`, `hardwareKeys.accessibility`, `hardwareKeys.inputMonitoring`, `hardwareKeys.granted`, `hardwareKeys.notGranted`, `hardwareKeys.requestInputMonitoring`, `hardwareKeys.openInputMonitoring`, `hardwareKeys.status.inputMonitoringRequired`, `hardwareKeys.status.permissionRequired`, `hardwareKeys.status.permissionRevoked`. Keep `presenceUnavailable` out (unused on macOS). Camera statuses use the 17 `cameraStatus.*` keys as they are, with the pt-BR lowercase wording ("Câmera: funcionando"). The Settings help text on `displayHelp` already names docks; on Windows also mention hubs and adapters.

Keys that would be both a resource and a scope are renamed with a `.title` suffix (`settings.title`, `protection.title`) because the Windows resource system forbids it; code must use these names.

## Checklist

Status values are filled during the final review (T169): Pass, Not run, or Deviation with the reason.

### Flyout

| # | macOS behavior | Source | Windows task |
|---|----------------|--------|--------------|
| F1 | 360 wide popup, 12 spacing, card list height = first three cards then scroll, screen cap `max(120, visible - 150 - 100)` | `PopupLayout.swift:4-25` | T049, T047 |
| F2 | Header "TelaViva OLED" with a Refresh button ("Refresh status") that refreshes core and DDC | `MonitorPopup.swift:12-18` | T152 |
| F3 | Empty state "No controllable displays" with the DDC explanation | `MonitorPopup.swift:31-34` | T050 |
| F4 | "Core service is not linked" notice when the core cannot start | `MonitorPopup.swift:35-38` | T153 |
| F5 | Card: name, optional protection toggle, brightness row (hidden when followed), volume row, protection brightness row | `MonitorPopup.swift:55-181` | T048, T129, T162 |
| F6 | Each slider shows a right-aligned value label `N%` (monospaced digits); slider follows the local draft while a write is pending | `MonitorPopup.swift:128-141`, `ManualMonitorControls.swift` | T150, T046 |
| F7 | Failed apply: line "Could not apply; showing confirmed value" under the slider | `MonitorPopup.swift:143-147` | T151 |
| F8 | Unavailable control is visibly unavailable (Windows adds the reason) | `MonitorPopup.swift:149-152` | T048 |
| F9 | Footer camera status "Camera: <status>" on one line with ellipsis, plus Settings and Quit | `MonitorPopup.swift:39-52` | T161, T047 |
| F10 | Per-monitor protection toggle only when presence or screensaver protection is on, default on | `MonitorPopup.swift:65-67,105-126` | T162 |
| F11 | Protection brightness slider capped at `min(confirmed brightness, lower draft)`, clamped and saved on show and on every brightness change, disabled when the cap is 0 | `MonitorPopup.swift:87-103,155-172` | T162 |
| F12 | Followed card: volume and protection rows only | `MonitorPopup.swift:69-71,82-85` | T129 |
| F13 | Writes ignored silently while the display is asleep or the identity is not stable; slider keeps the confirmed value; at most 128 tracked drafts | `ManualMonitorControls.swift:125-141` | T046 |
| F14 | Tray icon: filled dot, no dot, ring; accessible names "TelaViva OLED: face detected / no face detected / insufficient light"; absent before the first snapshot | `StatusItemIcon.swift`, `MenuBarController.swift:278-288` | T022, T021, T081 |

### Settings

| # | macOS behavior | Source | Windows task |
|---|----------------|--------|--------------|
| S1 | Three tabs General, Keyboard, Automatic brightness; settings save on every change, no Save button; loaded once | `SettingsView.swift:47-69` | T057, T056 |
| S2 | Window opens, closes the flyout, re-centers on the invoking monitor, reused on second open | `MenuBarController.swift:296-333` | T157 |
| S3 | Language picker (system, English (US), Português (Brasil)) applies to Settings text immediately | `SettingsView.swift:80-85` | T059 |
| S4 | Launch at login: toggle reverts on failure and an alert shows "Could not update launch at login" with details | `SettingsView.swift:70-98` | T156 |
| S5 | OLED protection section: presence toggle with help, threshold slider 10 to 90 default 20 (disabled when presence off) with help, keep camera on with help, dim on screensaver, suspend on battery, intervals 5 to 300 and 0.25 to 5 | `SettingsView.swift:100-121` | T080, T158 |
| S6 | "Monitors in protection" list in General: heading, help, one toggle per monitor (default on), empty state | `SettingsView.swift:122-137` | T159 |
| S7 | Availability section: camera picker with "Use default camera" first, inline "Camera: <status>", two help captions | `SettingsView.swift:139-148` | T160, T161 |
| S8 | Keyboard tab: toggle, status line (off, active, unavailable), volume step slider 1 to 15 default 5 shown "N pt", disabled when off | `SettingsView.swift:156-176` | T113, T163 |
| S9 | Fine step with a modifier (Shift on Windows), mute as volume zero with restore | `HardwareKeyRouter.swift` | T163, T112 |

### Automatic brightness

| # | macOS behavior | Source | Windows task |
|---|----------------|--------|--------------|
| A1 | Follow toggle disabled when the laptop panel is unavailable unless already on; help; unavailable reason text | `AmbientBrightnessView.swift:28-47` | T127, T116 |
| A2 | Per-monitor status: Calibrated, Not calibrated, Brightness control not supported, Monitor identity not stable | `AmbientBrightnessView.swift:54-83` | T164 |
| A3 | Calibration panel hidden for unsupported or unstable monitors and while the panel is unavailable | `AmbientBrightnessView.swift:61` | T164 |
| A4 | Points list: ascending rows "Monitor N% → Laptop M%", Calibrate and Remove, Remove disabled for the active point | `AmbientBrightnessView.swift:170-193` | T165 |
| A5 | Add row: 1 to 100, default first unused of 25, 50, 75, 100 else first unused integer; disabled when saved or at 32 points | `AmbientBrightnessView.swift:196-219` | T165 |
| A6 | Hint when fewer than two points | `AmbientBrightnessView.swift:113-116` | T165 |
| A7 | Calibration dialog: 420 wide, title, help, waiting caption until the monitor is within 2 points, slider bounded by neighbor points, Cancel (Esc) and Done (default, disabled until anchored) | `AmbientBrightnessView.swift:123-236` | T166, T128 |
| A8 | Calibration ends and restores everything on Done, Cancel, dialog close, leaving the tab, closing Settings, display sleep, laptop unavailable, app quit | `AmbientBrightnessView.swift:72`, `MenuBarController.swift:225-227,317-319` | T167 |
| A9 | Six notices shown once under the monitor list, no auto-dismiss, cleared when a new calibration starts | `AmbientBrightnessView.swift:240-261` | T168 |
| A10 | Crash recovery of an interrupted calibration at launch | `MenuBarController.swift:216` | T119, T121, T122 |

### Other refinements from the changelog

| # | Refinement | Windows task |
|---|-----------|--------------|
| C1 | Popup height follows the monitor count and scrolls beyond three; Settings window is tall enough | T049, T157 |
| C2 | Camera stays off while every eligible monitor is at or below the threshold | T080 |
| C3 | First presence sample 1 s after resume; 5 s dark settle (core timing) | T073 |
| C4 | Missing pt-BR strings caught by key parity | T024 |
| C5 | Restore retried up to three times after a failed wake readback (core) | T040, T074 |
