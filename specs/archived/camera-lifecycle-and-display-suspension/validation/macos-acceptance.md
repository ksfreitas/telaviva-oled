# macOS Camera Lifecycle Acceptance

Status: physical camera, display sleep/wake, and DDC behavior below remain unverified until exercised on hardware. Automated test and build results are recorded separately in the feature plan. Windows is deferred.

Record macOS version, Mac model, camera, monitor models, and connection paths with each run. Use a local development bundle; no frame data should be saved or transmitted.

| Scenario | Expected behavior | Result |
|---|---|---|
| Existing settings | All preferences load; Keep camera on defaults off. | Not run |
| Enable Keep camera on | Session stays open between normal checks; inference follows configured interval. | Not run |
| Face returns while always-on enabled | Brightness restores through ownership checks; camera remains open. | Not run |
| Disable Keep camera on | Normal checks return to one-frame capture; return checks still retain capture. | Not run |
| All eligible external displays sleep | Camera stops, checks suspend, no new write reaches sleeping targets. | Not run |
| Wake one eligible display | Camera resumes and brightness is read before any protection/restoration write. | Not run |
| One awake and one sleeping monitor | Monitoring continues; sleeping monitor receives no hardware writes. | Not run |
| Awake display at brightness zero | Monitoring continues. | Not run |
| Low light | No absence is inferred from darkness; next retry waits 10 seconds, captures for five seconds to settle exposure, then assesses fresh light. | Not run |
| Light improves | Two usable-light assessments resume face checks; warmup frames do not count. | Not run |
| Sleep/disable/battery during settling | Camera and timers stop; late results cannot dim or restore brightness. | Not run |
| Camera removed, busy, or permission denied | Error/suspension is shown without unbounded frame waits. | Not run |
| Screensaver and display sleep overlap | Screensaver remains an independent protection reason and is reconciled after wake. | Not run |
| Brightness changed externally while asleep | Wake preserves the external adjustment through fresh readback/ownership checks. | Not run |
| App exits during capture/settling | Camera shuts down; late callbacks cannot restart it. | Not run |
| Portuguese and English settings | Inline statuses use localized lowercase phrases; Portuguese ready status is exactly Câmera: funcionando. | Not run |

`CGDisplayIsAsleep` follows OS-reported state. Manual monitor power-off may remain logically awake on some connections; record that limitation rather than inferring physical power state from brightness or a successful build.
