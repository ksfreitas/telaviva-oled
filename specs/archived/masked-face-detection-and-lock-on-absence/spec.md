# Feature Specification: Occlusion-robust face detection, 1% presence-check floor and lock screen on absence

**Feature Branch**: `masked-face-detection-and-lock-on-absence`
**Input**: Improvement package with three items: a detection model that handles masks and hands over the face, a 1% minimum for the presence-check brightness threshold, and an opt-in lock screen on absence.

## Context and Understanding

TelaViva OLED dims external monitors (and optionally the built-in display) when no face is seen by the webcam. Presence means "any face above a confidence of 0.8", decided locally by YuNet (OpenCV Zoo `2023mar`) through ONNX Runtime. Three gaps motivate this package:

1. **False absence with partial occlusion.** Nothing in the repository establishes recall for masked faces, hands over the face, glasses, angle or distance. Existing documents state that recall was never measured (ADR 0001, `tools/inference/README.md`, the archived validation results). A user wearing a mask or resting a hand on the face can be judged absent, so the screen dims while the user is still there. The same weakness would make an automatic lock (item 3) dangerous.
2. **Brightness-check floor.** "Check presence only above brightness" (`presence_min_brightness_percent`) accepts 0 to 90 today (the original 10 to 90 range was lowered to 0 on 2026-10-21 by the owner). The owner now wants the minimum to be 1%.
3. **No lock action.** The app only dims. It has no lock-screen code on either platform, and the documents say the opposite: the app "does not force an immediate lock or sleep" (`docs/user-guide.md:56`), and lock is "outside the initial trigger scope" (`ARCHITECTURE.md:67`). The owner wants an opt-in option that locks the session after absence is confirmed twice, with a safeguard for bad lighting.

## Feature Description

- **Detection quality.** Find, evaluate and, if it is measurably better, adopt a face detector that still reports a face when it is partly covered by a mask or a hand, keeping the "any face, no identification, local only" contract, the open-source licensing rule and the acceleration/CPU-fallback behavior.
- **Threshold floor.** The "Check presence only above brightness" control (global and per-display) stops at 1% instead of 0%, consistently in the core and both shells, without destroying users' stored settings.
- **Lock on absence.** A new off-by-default option. When protection has dimmed the screen after a confirmed absence, the app waits a configurable time (5 seconds by default) and takes one more sample. If that sample still finds no face and the lighting is good enough to trust it, the app locks the session on macOS and Windows. If lighting is unfavorable at any point, the option does not act and the screen stays dimmed.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As someone wearing a face mask, I want the app to keep seeing me as present, so the screen does not dim while I work.
- **US-002**: As someone resting a hand on my chin, mouth or cheek, I want the app to keep seeing me as present.
- **US-003**: As a user with a very dark monitor, I want to set the presence-check threshold as low as 1%, and I do not want my existing settings to be lost by this change.
- **US-004**: As a user who leaves the desk, I want the session to lock automatically after my screen has dimmed and a second check still finds nobody, so my machine is not left open.
- **US-005**: As a user in a dark room or with a bad camera image, I do not want the app to lock my session on a doubtful reading.
- **US-006**: As a user, I want to turn the lock option on and off, and to choose how long it waits before the confirming check, in Settings on macOS and on Windows, in English and Portuguese.

### Functional Requirements

#### Detection model (item 1)

- **FR-000**: Low consumption (CPU, memory, battery, camera time) is the first criterion when choosing the model. The goal is to REPLACE YuNet with one more complete model. Only if no single model reaches the occlusion goal within the budget, a second model is used together with the first, with a user setting to control it (owner decision, 2026-10-29). Two selectable models are not a goal.
- **FR-001**: System MUST identify a candidate detector that meets the open-source redistribution rule (license and provenance recorded as for YuNet in `licenses/inference/`), runs through ONNX Runtime on the pinned versions (macOS arm64 1.23.0, Windows x64 DirectML 1.20.1) and keeps CPU fallback.
- **FR-002**: System MUST compare the candidate with the current YuNet on a fixture set of legally redistributable images (CC0 or equivalent) covering: no occlusion, surgical or cloth mask over nose and mouth, a hand over the mouth/chin/cheek, a hand over part of the eyes, glasses, off-axis head angles, and images with no face (including a hand alone, a mask alone and non-face objects). Note (2026-10-31): no fixture image shows a hand over the eyes, so this part is not covered.
- **FR-003**: System MUST adopt the candidate only if it detects the masked and hand-occluded faces in the fixture set that YuNet misses, without more false positives on the no-face images, and within the latency/CPU/memory budget of the current model on both platforms. Note (2026-10-31): the adopted model has one recorded exception to this rule: 1 of 16 face-free images is a false positive (a tarantula on a hand), against 0 of 16 for YuNet at 0.80. See [model-decision.md](validation/model-decision.md) and [ADR 0042](../../../adr/0042-occlusion-robust-face-detection.md).
- **FR-004**: If adopted, the new model MUST be checksum-verified at load like today, packaged for macOS and Windows, covered by the existing self-test gating for accelerated devices, and its license and provenance MUST be recorded.
- **FR-005**: Presence MUST keep meaning "any face detected", with no identification or enrollment, and frames MUST stay local.
- **FR-006**: The existing light gate (frames that are too dark return "insufficient light", which is unknown presence, not absence) MUST keep working with the new model.

#### Threshold floor (item 2)

- **FR-007**: The global and per-display presence brightness threshold MUST accept values from 1 to 90 percent and reject 0 in the core validation.
- **FR-008**: The macOS slider and the Windows number box MUST start at 1 and the Windows clamp MUST use 1 as minimum.
- **FR-009**: A settings file that holds a stored threshold of 0 (global or any per-display entry, including the built-in display entry) MUST NOT cause the whole settings file to be reset. The value MUST be raised to 1 on load, and all other stored settings MUST be preserved.
- **FR-010**: Documentation that states the 0 to 90 range (user guide, `api.md`, `ARCHITECTURE.md`, `CHANGELOG.md`) MUST be updated, and ADR 0011 MUST be marked as superseded on the range.

#### Lock on absence (item 3)

- **FR-011**: System MUST provide a boolean setting, off by default, labeled in en-US and pt-BR on both platforms, that enables locking the session after confirmed absence.
- **FR-012**: When the setting is on and absence has been confirmed (the existing two-miss rule has dimmed the display), the system MUST wait the configured delay (`lock_delay_s`, 5 seconds by default, see FR-011a), then take another presence sample. The existing two-miss confirmation (ADR 0030) is kept unchanged.
- **FR-011a**: The wait before the confirming sample MUST be a user setting `lock_delay_s` of 1 to 60 seconds, default 5, shown next to the lock option on both platforms with en-US and pt-BR labels, and disabled while the lock option is off. The core rejects an update outside the range; a stored value outside the range (for example from a hand-edited file) is clamped on load instead of resetting the settings file.
- **FR-013**: If that sample finds no face and the frame had enough light, the system MUST lock the user session (macOS and Windows). It MUST NOT unlock automatically and MUST NOT wake a sleeping system.
- **FR-014**: If that sample finds a face, the existing restore behavior MUST run and no lock happens.
- **FR-015**: If lighting is unfavorable when the lock stage would start or at the confirming sample (today: "insufficient light"), the system MUST NOT lock, MUST leave the dimmed state as is, and MUST report presence as unknown as it does today. "Unfavorable lighting" means the existing too-dark gate (insufficient light) only. Overexposure and backlight detection are out of scope.
- **FR-016**: The lock stage MUST NOT run when presence protection is off, when the camera is suspended (brightness gate below threshold, display asleep, battery suspension, camera unavailable/busy/permission) or when the session is already locked.
- **FR-017**: A manual unlock followed by a face MUST restore brightness through the existing restore path. The lock option MUST NOT change any global OS screensaver, power or security preference and MUST NOT need elevated privileges or network use.
- **FR-018**: If the lock request fails or cannot be confirmed, the system MUST NOT claim the session is locked, MUST keep the display dimmed and MUST record the failure in the local diagnostic log. On macOS the most reliable mechanism is used even if it is a private API (precedent: ADR 0007), the locked state is checked afterwards, and the choice is recorded in the ADR.
- **FR-019**: The behavior MUST be identical on macOS and Windows in the same change, with tests on each platform (AGENTS.md).

### Edge cases

- Sample 2 is dark because dimming reduced the monitor light that illuminates the face (monitor-only lighting): the result is "insufficient light", so the app does not lock. This is intended, and the user guide must say so.
- The user returns between the dim and the confirming sample: the sample finds a face, brightness is restored, no lock.
- Keep-awake with presence is on: a lock caused by this feature is a session lock, so keep-awake already suspends the camera while the session is inactive. The two options must not fight each other.
- Several monitors, each with its own miss count: dimming applies to all protected displays together, so the lock stage arms when any display is presence-dimmed (design decision 8).
- A user setting from a newer build read by an older binary: older binaries reject unknown settings fields and reset (see ADR 0003 amendments), so the new field needs the same caution as earlier toggles.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On the agreed fixture set, the chosen model detects a higher share of masked and hand-occluded faces than YuNet and produces no more false positives on the no-face images. The acceptance rule (at least 10 percentage points of pooled occluded recall, no worse on the other categories, no more false positives, within the latency and memory budget) is fixed in design decision 3. Note (2026-10-31): the adopted model meets the recall part of this rule (19 of 19 occluded faces, against 14 of 19 for YuNet at 0.80) but has the false-positive exception recorded in [model-decision.md](validation/model-decision.md) and [ADR 0042](../../../adr/0042-occlusion-robust-face-detection.md).
- **SC-002**: The chosen model's inference time and memory use on the two platforms stay within the current budget (the existing 250 ms self-test budget, one-shot CPU use) and the existing device self-test still passes. Note (2026-10-31): the adopted model's macOS CPU p50 is 62.4 ms, above the 60 ms cap of design decision 3 rule 4, and its memory increase is within the 10 MB limit on macOS (4.1 MB) but not on Windows (17.3 MB); see [model-decision.md](validation/model-decision.md) and [windows-model-check.md](validation/windows-model-check.md).
- **SC-003**: The threshold control cannot be set to 0 on either platform, 1 is accepted, and upgrading with a stored 0 keeps all other stored settings.
- **SC-004**: With the lock option on, a user absent from the camera for the confirmation time plus the configured delay (5 seconds by default) has the session locked on macOS and Windows, with adequate light.
- **SC-005**: With the lock option on and an insufficient-light result at either check, the session is never locked by this feature in any test.
- **SC-006**: With the lock option off (default), behavior is identical to the previous release.
- **SC-007**: Automated tests on the core, macOS and Windows cover the range change, the stored-0 migration, the lock sequence and its lighting guard, and the new strings exist in en-US and pt-BR.

## Clarification Needed

- Resolved in design: the model is chosen by measurement with the acceptance rule of design decision 3 (SC-001), and the multi-monitor lock condition is settled in design decision 8.
- The training data of the current YuNet (WIDER FACE, CC BY-NC-ND 4.0) may restrict commercial use of the weights. This already applies to the shipped model, so it is a legal check for the owner, not a blocker for this feature.

## Model research (2026-10-29)

Web research through Exa by two Haiku agents. Numbers come from search excerpts and must be re-read in the primary sources before they go into repository documents. Items marked [U] are unverified.

**Headline: no candidate is verified as better than YuNet on masks, and none has published numbers for hands over the face.** A swap cannot be justified by the literature. It has to be justified by measurement on our own frames.

- **Evidence on masks.** Heavy occlusion (4 facial regions covered) is the main failure mode for every detector, even those trained with mask data (5 to 26 AP on MAFA heavy). Detectors trained with occlusion or mask data do best on masked faces: on MAFA, RetinaFace 86.61 AP50, AntiCov (RetinaFace-MobileNet) 80.55, AIZooTech 85.44, MTCNN 43.47 (Batagelj et al. 2021). Nothing was found for YuNet 2023mar on masks, and nothing for hands over the face.
- **Cheap lever first.** The shipped decoder accepts a face only at confidence 0.8 (`CONFIDENCE_THRESHOLD`); the YuNet demo default is 0.9 and mask studies sweep much lower thresholds. A lower or tuned threshold may recover masked faces with no model change and no extra cost. The false-positive effect on hands and masks alone must be measured (FR-003).
- **Permissive candidates (code and weights licenses as found):**

| Candidate | Size / input | License | Notes |
|---|---|---|---|
| YuNet 2023mar (current) | 232 KB, 640x640 fixed | MIT | WIDER Hard 0.7503; baseline |
| RetinaFace MobileNet0.25 | about 1.7 MB, 640x640 ONNX (`yakhyo/tiny-face-pytorch`, MIT) | MIT (WIDER caveat) | WIDER Hard 80.99; family is the best on MAFA; decode outside the graph |
| BlazeFace short range (MediaPipe) | about 224 KB, 128x128 | Apache-2.0 model card | Front-camera, within 2 m; weak alone on shaded faces; a third-party cascade with YuNet recovered many misses [U]; needs tflite to ONNX conversion |
| CenterFace | 7.3 MB | MIT code, weights license not checked | No mask data; heavier |
| UltraFace RFB-320/640 | about 1.2 MB | MIT | Lower accuracy than YuNet |

- **Excluded:** SCRFD and all InsightFace weights (non-commercial research only), YOLOv5/8/11-face variants (GPL/AGPL), AIZOO FaceMaskDetection and any MAFA-trained model (data license unclear), YOLO-FaceV2 (weights license not found), CAFACLite (best published occlusion numbers, but single author, created 2026-05, weights license undeclared; usable only as an internal benchmark if the owner clears it).
- **Platform constraints.** CoreML supports no NonMaxSuppression in the graph and needs static shapes and constant parameters for Resize/Slice/Pad/Split, so decoding and NMS stay outside the graph, as today. DirectML supports ONNX opset up to 20. No compatibility reports were found for any candidate, so each one needs a CoreML and DirectML run in design.
- **Test images.** WIDER FACE, MAFA, FFHQ, MaskedFaceNet, Hand2Face and similar sets cannot be committed (non-commercial, no-derivatives, EULA or unclear). Usable: the owner's own recorded footage released as CC0 with participants' consent (cleanest), COFW (CC BY 4.0, source-image rights not checked), and individual Wikimedia Commons or Pixnio files confirmed as CC0/public domain. Pexels and Unsplash photos are not suitable to commit.
- **Method for the benchmark.** Label frames by occlusion (none, partial under 30% of the face, heavy) and by tags (mask, hand over mouth, hand over eyes, sunglasses, side or tilted head). Sweep the confidence threshold and choose the operating point on a separate split. Report per-category recall with confidence intervals, and false positives per frame on face-free frames, including hands alone and masks alone. Downscale and compress frames to webcam conditions. Use a frame-level "any face present" metric.

**Direction for design (not a decision).** (1) Build the fixture set and a benchmark harness with YuNet at several thresholds as the baseline. (2) Add RetinaFace MobileNet0.25 as the main replacement candidate. (3) Only if no single model reaches the goal within the consumption budget, evaluate BlazeFace as a low-cost second stage that runs only after a YuNet miss. That keeps the per-frame cost of the common case unchanged, in line with FR-000.

## Decisions

Answered by the owner on 2026-10-29:

1. **Model**: research a detector that supports masks and hands over the face, with low consumption as the focus (web research through Exa). Replace YuNet with one more complete model if possible. Otherwise use two models with a setting. Two selectable models are not wanted as the plan.
2. **Stored threshold 0**: raise to 1 on load and keep the other settings (FR-009).
3. **Lock sequence**: keep the existing two-miss confirmation, dim, wait 5 s, one more sample, lock if still absent (FR-012).
4. **Wait time**: first answered as fixed at 5 seconds; changed by the owner on 2026-10-29 to a user setting `lock_delay_s` (1 to 60 seconds, default 5) (FR-011a, FR-012). The range is a design choice made when the setting was added.
5. **Unfavorable lighting**: the existing too-dark gate only (FR-015).
6. **macOS lock**: most reliable mechanism even if private, verify the locked state, log failures (FR-018).
7. **Test images**: only individual files from Wikimedia Commons or Pixnio confirmed as CC0 or public domain, checked one by one, with the license and URL of each recorded next to the fixture. The owner does not record own footage. Consequences: the fixture set will be smaller and its coverage uneven (masks, hands over the face, glasses, angles, face-free frames), so the recall comparison is indicative, with wide confidence intervals. Images of identifiable people need a check that reuse is not restricted (the Commons people categories warn about this), and gaps in coverage must be listed in the benchmark report instead of being filled with images of unclear license.

## Notes

- **Item 1 is research-heavy.** No fixtures, baseline recall or candidate evaluation exist. Candidates listed in discussion (for example other YuNet exports, SCRFD, RetinaFace, BlazeFace, UltraFace, YOLO-face variants) are unverified here: their licenses (several are non-commercial or copyleft), operator coverage on CoreML/DirectML and masked-face behavior must be checked in design. Replacing the model touches the SHA constant, the fixed 640 input, the 12 named YuNet outputs and the YuNet-specific decoder in `crates/telaviva-inference/src/lib.rs`, plus `manifest.toml`, `prepare.py`, the build scripts and the installer.
- **Item 2 detail.** The comparison is strict "above": a monitor at or below the threshold suspends the camera. With a floor of 1, a DDC monitor at 0% or 1% still suspends the camera, so the change only narrows the accepted range. Note (2026-10-31): for a user who had a stored 0, a monitor at exactly 1% now suspends the camera where it ran before; see [ADR 0044](../../../adr/0044-presence-brightness-threshold-floor-of-one-percent.md). A stored 0 today makes `validate()` fail and replaces the whole settings file with defaults (backup written to `.json.corrupt`), so a migration is required. Files to touch are listed in the discovery report: `settings.rs:387-416`, `SettingsView.swift:171`, `SettingsModel.cpp:18,211-228`, `GeneralPage.xaml.cpp:52,453`, tests in `core_contract.rs`, `settings_payload.cpp` and Swift, plus docs.
- **Item 3 detail.** The core makes no OS calls. The natural shape is a snapshot field that tells the shells when to lock, attached to the "absent" arm of `presence()` in `core.rs`, with the configured delay measured from the sample times and the samples arriving at the return cadence. This adds a capability the documents currently exclude, so it needs a new ADR (next number 0041) and updates to `docs/user-guide.md:56`, `ARCHITECTURE.md:67` and the keep-awake notes.
- **Lighting today.** "Unfavorable lighting" is only "too dark" (mean luma below 18). Overexposure, backlight and contrast are not assessed, and ADR 0003 records an open low-light report with a bright window behind the user.
- **Dependency between items.** The lock is more disruptive than dimming, so it benefits from item 1 first. The three items are otherwise independent and can be planned as separate phases of one release.
- **Locking and Windows.** No existing code calls a lock API. Windows has a public call. macOS has no public lock API, and the project already accepted private APIs for built-in brightness (ADR 0007).
