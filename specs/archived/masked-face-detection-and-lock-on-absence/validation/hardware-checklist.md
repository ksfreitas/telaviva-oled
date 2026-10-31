# Hardware acceptance checklist: masked face detection and lock on absence

Passed: the owner marked every row as passed on 2026-10-31, with no values recorded. Written 2026-10-29.

Every result cell below reads Pass, 2026-10-31, owner, no values recorded, except row E13, which is an automated check.

## A. Face detection model (macOS)

| # | What to do | Expected result | Result / date / who |
|---|------------|-----------------|---------------------|
| A1 | Sit in front of the camera normally, no mask. | Menu bar shows present; no dim after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A2 | Sit in front of the camera with a surgical or cloth mask over nose and mouth. | Menu bar shows present; no dim after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A3 | Sit in front of the camera with a hand over the mouth. | Menu bar shows present; no dim after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A4 | Sit in front of the camera with a tissue over nose and mouth. | Menu bar shows present; no dim after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A5 | Turn to a side view of the camera. | Menu bar shows present; no dim after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A6 | Sit in front of the camera with glasses, then with sunglasses. | Menu bar shows present; no dim after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A7 | Walk away from the camera. | Display dims after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A8 | Leave an empty chair in front of the camera, in daylight. | Display dims after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A9 | Leave an empty chair in front of the camera, under lamp light only. | Display dims after the usual two checks. | Pass, 2026-10-31, owner, no values recorded |
| A10 | Place a round or hairy object in front of the camera: a ball, a plush toy, a hand-sized spider toy. | Note whether the object counts as a face. Known limit: if it counts, the screen only stays lit. Record the result either way. | Pass, 2026-10-31, owner, no values recorded |
| A11 | Measure CPU and energy impact with the camera on at the default 10 s cadence, for the previous build and this build, under the same light and monitor setup. | Activity Monitor CPU and energy impact recorded for both builds. | Pass, 2026-10-31, owner, no values recorded |
| A12 | Repeat A11 at the 0.5 s return cadence. | Activity Monitor CPU and energy impact recorded for both builds. | Pass, 2026-10-31, owner, no values recorded |
| A13 | Settings > Advanced, inference device: select CPU, then Automatic (CoreML). | Both start without error and the self-test passes for each. | Pass, 2026-10-31, owner, no values recorded |
| A14 | Keep the room dim so the camera has insufficient light, with presence protection on. | Reported as insufficient light or unknown, not as absence. The display does not dim because of the low light. | Pass, 2026-10-31, owner, no values recorded |

## B. Threshold floor (macOS)

| # | What to do | Expected result | Result / date / who |
|---|------------|-----------------|---------------------|
| B1 | Drag the presence brightness threshold slider to its lowest position. | The slider cannot go below 1 %. | Pass, 2026-10-31, owner, no values recorded |
| B2 | Quit the app. Set `presence_min_brightness_percent` to 0 in the existing settings.json, keeping the other settings. Start the app. | Starts with the threshold at 1; all other settings kept; no `.corrupt` backup file created. | Pass, 2026-10-31, owner, no values recorded |
| B3 | Set the monitor to 1 % brightness with presence protection on. | Camera is suspended (camera indicator off). | Pass, 2026-10-31, owner, no values recorded |
| B4 | Set the monitor to 2 % brightness. | Camera runs. | Pass, 2026-10-31, owner, no values recorded |

## C. Return interval default (macOS)

| # | What to do | Expected result | Result / date / who |
|---|------------|-----------------|---------------------|
| C1 | Start with a fresh settings file (fresh profile or a settings.json moved out of the way). Open the return interval setting. | Shows 0.5 s. | Pass, 2026-10-31, owner, no values recorded |
| C2 | Set the return interval to 2 s, quit, then start the app again. | The saved value 2 s is kept. | Pass, 2026-10-31, owner, no values recorded |

## D. Lock on absence (macOS)

| # | What to do | Expected result | Result / date / who |
|---|------------|-----------------|---------------------|
| D1 | Start with a fresh settings file. Open the lock on absence option. | Off by default. | Pass, 2026-10-31, owner, no values recorded |
| D2 | Try to turn lock on absence on with presence protection off. | The option requires presence protection on. | Pass, 2026-10-31, owner, no values recorded |
| D3 | Turn on presence protection and lock on absence. Use good light. Walk away. Lock wait set to 5 s. | Display dims after the usual two checks; session locks after the 5 s wait. | Pass, 2026-10-31, owner, no values recorded |
| D4 | Repeat D3 with the lock wait set to 15 s. | Session locks after the 15 s wait. | Pass, 2026-10-31, owner, no values recorded |
| D5 | Use good light. Walk away and return before the wait ends. | Session never locks. | Pass, 2026-10-31, owner, no values recorded |
| D6 | Use a dark room. Walk away and wait longer than the lock wait. | Session never locks. Expected: the dimmed screen can make the image too dark for detection. | Pass, 2026-10-31, owner, no values recorded |
| D7 | Set "Require password after screen saver or display sleep" to a long delay. Use good light. Walk away and wait for the lock. | Session locks after the configured wait; the password delay setting does not postpone it. Record what the lock screen shows. | Pass, 2026-10-31, owner, no values recorded |
| D8 | Unlock normally, then sit in front of the camera. | Camera resumes within about 1 s of unlock. | Pass, 2026-10-31, owner, no values recorded |
| D9 | Continue from D8: the first face after unlock. | The first face restores brightness. | Pass, 2026-10-31, owner, no values recorded |
| D10 | After any lock during these tests, check whether the app unlocked the session. | The app does not unlock the session; the user unlocks it. | Pass, 2026-10-31, owner, no values recorded |
| D11 | Record the macOS version. Run `log show --predicate 'subsystem == "com.telaviva.oled"'` and find the private lock line. | macOS version recorded. Line recorded as `lock ok` or `lock failed`. | Pass, 2026-10-31, owner, no values recorded |
| D12 | Run the packaged ad-hoc signed app (see `packaging/macos/README.md`) and trigger a lock by absence. | The packaged app loads the private framework; the log line is `lock ok` or `lock failed`, recorded as found. | Pass, 2026-10-31, owner, no values recorded |
| D13 | Turn on keep-awake with presence together with lock on absence. Keep a face in view, then walk away. | The two options do not conflict: no lock while a face is detected; after absence, the lock happens after the wait. | Pass, 2026-10-31, owner, no values recorded |
| D14 | Start a lock-wait countdown with no face in view, then quit the app before the wait ends. | The app never locks while quitting. | Pass, 2026-10-31, owner, no values recorded |
| D15 | Remove the camera permission in System Settings, then walk away. | Session does not lock. Status shows the camera as unavailable. | Pass, 2026-10-31, owner, no values recorded |
| D16 | Cover the camera lens, then walk away. | Session does not lock. Status shows insufficient light or unknown. | Pass, 2026-10-31, owner, no values recorded |

## E. Windows (port implemented 2026-10-30)

Row E13 is an automated model check, run on 2026-10-30 (see `validation/windows-model-check.md`); it is not a lock-screen check. Every other row was marked as passed by the owner on 2026-10-31, with no values recorded.

| # | What to do | Expected result | Result / date / who |
|---|------------|-----------------|---------------------|
| E1 | Threshold floor: slider minimum, settings with 0, monitor at 1 % and 2 % (as B1 to B4). | Same as B1 to B4. | Pass, 2026-10-31, owner, no values recorded |
| E2 | Return interval default and kept value (as C1 and C2). | Same as C1 and C2. | Pass, 2026-10-31, owner, no values recorded |
| E3 | Lock option off by default; requires presence protection on (as D1 and D2). | Same as D1 and D2. | Pass, 2026-10-31, owner, no values recorded |
| E4 | Good light, walk away; lock wait 5 s and 15 s using LockWorkStation. | Session locks after each wait. | Pass, 2026-10-31, owner, no values recorded |
| E5 | Return before the wait ends; dark room (as D5 and D6). | Session never locks in either case. | Pass, 2026-10-31, owner, no values recorded |
| E6 | Lock confirmation: observe the WTS session lock event. | The app reports the lock only after the WTS lock event arrives. | Pass, 2026-10-31, owner, no values recorded |
| E7 | Dynamic Lock on and off, and a screen saver lock policy set. | Record the behaviour. No error from the app's lock and the OS lock. | Pass, 2026-10-31, owner, no values recorded |
| E8 | Sleep and resume (Modern Standby) with lock on absence on. | No lock is issued from an absence observation made before sleep. | Pass, 2026-10-31, owner, no values recorded |
| E9 | Packaging: check the installed package contains the model file the app loads. The package holds face_detection_retinaface_mb025.onnx. | Package contents match the model the app loads. | Pass, 2026-10-31, owner, no values recorded |
| E10 | DirectML device self-test on the Radeon iGPU and the RTX 4080 SUPER, and CPU. | Self-test passes on each device. | Pass, 2026-10-31, owner, no values recorded |
| E11 | Latency and memory of the new model on CPU and DirectML, compared with YuNet. | Values recorded for both models under the same conditions. | Pass, 2026-10-31, owner, no values recorded |
| E12 | Repeat A1 to A10 on the Windows RGB camera. | Recorded per row; same expectations as section A. | Pass, 2026-10-31, owner, no values recorded |
| E13 | Windows model check (CPU latency, DirectML). | Automated, not a lock-screen check. | done 2026-10-30, see windows-model-check.md |
| E14 | Keep-awake with presence together with lock on absence (as D13), on Windows. Keep a face in view, then walk away. | The two options do not conflict: no lock while a face is detected; after absence, the lock happens after the wait. | Pass, 2026-10-31, owner, no values recorded |
| E15 | Unlock normally, then sit in front of the camera (as D8). | Camera resumes within about 1 s of unlock. | Pass, 2026-10-31, owner, no values recorded |
| E16 | Lock with insufficient light: cover the camera lens, then walk away (as D16 and A14). | Session does not lock. Status shows insufficient light or unknown. | Pass, 2026-10-31, owner, no values recorded |
| E17 | Return before the wait ends with the default lock wait, then with a changed lock wait. | Session never locks in either case. | Pass, 2026-10-31, owner, no values recorded |
| E18 | Failed lock path: make the lock request fail, then observe the status. | The app reports the lock as failed and does not report the session as locked. | Pass, 2026-10-31, owner, no values recorded |

### Windows files changed (done 2026-10-30)

- `native/windows/Platform/JsonCodec.h:51` and `native/windows/Platform/JsonCodec.cpp:919`: return interval default 0.5.
- `native/windows/Platform/SettingsModel.h:19` and `native/windows/Platform/SettingsModel.cpp:210`: return interval default.
- `native/windows/Platform/SettingsModel.cpp` and `native/windows/TelaViva/GeneralPage.xaml.cpp`: threshold floor 1 %. Tests already written in `native/windows/Tests/settings_payload.cpp`.
- Lock option: new `SessionLocker` and `LockCoordinator`; `JsonCodec` lock block and `lock_result`; `SettingsModel`; `GeneralPage` (`GeneralPage.xaml`, `GeneralPage.xaml.cpp`, `GeneralPage.xaml.h`); both `native/windows/TelaViva/Strings/en-US/Resources.resw` and `native/windows/TelaViva/Strings/pt-BR/Resources.resw`; `AppController` wiring in `native/windows/TelaViva/Host/AppController.cpp`; new `native/windows/Tests/session_lock.cpp`.
- `.github/workflows/windows.yml`: bench test step.
- Model packaging: `packaging/windows/build-app.ps1`, `packaging/windows/Install-TelaVivaOLED.ps1`, model file name in `native/windows/TelaViva/Host/AppController.cpp` (about line 1346), `crates/telaviva-inference/tests/ort_yunet_smoke_windows.rs` and `crates/telaviva-inference/tests/directml_bench_windows.rs`, `crates/telaviva-inference/tests/directml_provider_windows.rs`.
- Windows docs.
