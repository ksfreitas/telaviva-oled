# macOS Camera Lifecycle Acceptance

Status: partially verified on physical hardware on 2026-10-18. Ten of the sixteen scenarios passed, one passed in part, and five were not run (reasons in the table). Automated test and build results are recorded separately in the feature plan. Windows is deferred.

## Run environment

- Date: 2026-10-18, guided run: the owner performed each physical step and reported what was observed; the results below are those reports, not instrumented measurements.
- macOS 27.0.1, MacBook Pro with Apple M4 Pro, built-in camera (Câmera do MacBook Pro) and built-in Liquid Retina XDR display.
- One external monitor: Dell S3225QC, connector `dispext0`, stable identity, DDC brightness supported (brightness 34 at the start). The physical cable type was not recorded.
- App: version 0.1.1 installed in `/Applications`, with a binary identical (`cmp`) to the local release build of this commit. The owner's real settings were used (presence protection, Keep camera on and screensaver protection on).
- Only one external monitor was available, so scenarios that need two could not run. No frame data was saved or transmitted.

## Results

| # | Scenario | Expected behavior | Result |
|---|---|---|---|
| 1 | Existing settings | All preferences load; Keep camera on defaults off. | Passed. The existing settings file with all preferences was used by the app. The default-off behavior for new and old files is asserted by the Rust settings tests. |
| 2 | Enable Keep camera on | Session stays open between normal checks; inference follows configured interval. | Passed for the open session: the camera indicator stayed on continuously. The inference cadence was not measured. |
| 3 | Face returns while always-on enabled | Brightness restores through ownership checks; camera remains open. | Passed. The Dell dimmed and restored on return, with the camera open. |
| 4 | Disable Keep camera on | Normal checks return to one-frame capture; return checks still retain capture. | Passed. |
| 5 | All eligible external displays sleep | Camera stops, checks suspend, no new write reaches sleeping targets. | Passed with `pmset displaysleepnow`: the camera indicator went off. The absence of a DDC write was inferred from the unchanged brightness, not observed directly. |
| 6 | Wake one eligible display | Camera resumes and brightness is read before any protection/restoration write. | Passed: the camera came back and the brightness after wake matched the one before sleep. The readback itself was not observed. |
| 7 | One awake and one sleeping monitor | Monitoring continues; sleeping monitor receives no hardware writes. | Not run: needs two eligible external monitors. |
| 8 | Awake display at brightness zero | The camera suspends as low brightness, not as display sleep (`suspended_low_brightness`), and resumes when the level rises above the threshold. | Passed. The original expectation, "monitoring continues", was superseded by the camera brightness threshold of [ADR 0011](../../../../adr/0011-camera-brightness-activation-threshold.md). Brightness zero still does not mean display sleep. |
| 9 | Low light | No absence is inferred from darkness; next retry waits 10 seconds, captures for five seconds to settle exposure, then assesses fresh light. | Partly passed. The status showed low light when the owner left the frame, and the Dell neither dimmed nor restored. The 10-second and five-second timings were not measured. |
| 10 | Light improves | Two usable-light assessments resume face checks; warmup frames do not count. | Not run: recovery was not demonstrated. See the observation below. |
| 11 | Sleep/disable/battery during settling | Camera and timers stop; late results cannot dim or restore brightness. | Not run: needs timing inside the five-second settling window. |
| 12 | Camera removed, busy, or permission denied | Error/suspension is shown without unbounded frame waits. | Not run: revoking camera permission or unplugging the camera was not attempted. |
| 13 | Screensaver and display sleep overlap | Screensaver remains an independent protection reason and is reconciled after wake. | Passed. |
| 14 | Brightness changed externally while asleep | Wake preserves the external adjustment through fresh readback/ownership checks. | Not run: no reliable way to change the monitor while macOS reports it asleep. Covered by the core ownership tests only. |
| 15 | App exits during capture/settling | Camera shuts down; late callbacks cannot restart it. | Passed. The camera indicator went off after quitting and did not return. |
| 16 | Portuguese and English settings | Inline statuses use localized lowercase phrases; Portuguese ready status is exactly Câmera: funcionando. | Passed in Portuguese: the status line reads exactly `Câmera: funcionando`. English was not checked on screen; the automated localization test asserts it. |

## Observation (open, not a checklist failure)

With the MacBook camera, the owner reported the low-light state while a bright window was behind them, and also when leaving the frame. Darkness is the designed trigger (mean luma below 18 of 255, `MIN_MEAN_LUMA`), so leaving a dim room is expected. A bright backdrop should raise the mean luma, so the report during a backlit scene is unexplained and was not diagnosed or measured. It cleared on its own during the run. If it recurs, plan it separately as a bug.

## Limits

`CGDisplayIsAsleep` follows OS-reported state. Manual monitor power-off may remain logically awake on some connections; record that limitation rather than inferring physical power state from brightness or a successful build. No power-consumption or CPU measurement was taken, and the results above support no claim about monitors other than the Dell S3225QC on this Mac.
