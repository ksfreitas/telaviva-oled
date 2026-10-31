# Architecture: Occlusion-robust face detection, 1% presence-check floor and lock screen on absence

**Feature**: masked-face-detection-and-lock-on-absence **Date**: 2026-10-29 **Branch**: `masked-face-detection-and-lock-on-absence` **Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary

Three independent changes share one release. (1) The shared inference crate gets a measured, gated path to a detector that keeps seeing partly covered faces: a benchmark harness and a fixed acceptance rule decide between a lower confidence threshold on YuNet, a replacement by RetinaFace MobileNet0.25, or a conditional second stage. (2) The presence brightness threshold floor moves from 0 to 1 with a load-time clamp. (3) The Rust core gets an opt-in lock stage that arms when the displays are presence-dimmed, fires on a later no-face sample at least `lock_delay_s` seconds after arming (a setting, 5 by default), and asks the shells to lock the session through a one-shot request with a token and an acknowledgement.

## Technical Context

**Language/Stack**: Rust workspace (`telaviva-core`, `telaviva-inference`), Swift/AppKit shell (macOS), C++/WinRT shell and a static `Platform` library (Windows). Core and shells talk through JSON over the versioned C ABI (`tv_submit`, `tv_snapshot`), which does not change.
**Key Dependencies**: ONNX Runtime (`ort =2.0.0-rc.9`, runtime 1.23.0 on macOS arm64, 1.20.1 DirectML build on Windows), existing `login.framework` private symbol on macOS (new, isolated), `LockWorkStation` on Windows (public). Benchmark only: `image` crate (JPEG decode) behind a `bench` feature.
**Storage**: `settings.json` (one new boolean, one changed validation range). Fixture images and a manifest in the repository for the benchmark.
**Testing**: Rust contract tests, Swift test scripts (`tools/tests/*.sh`, no XCTest), Windows `tests.exe` harness, CI workflows `macos.yml` and `windows.yml`.
**Platform**: macOS Apple Silicon and Windows 11 x64, same behavior in the same change (AGENTS.md).

## Technical Decisions

### Decision 1: The model is chosen by measurement with a fixed acceptance rule, not by literature

**What**: Build a benchmark harness first, then pick the first outcome that passes the acceptance rule: O1 YuNet with a lower per-model confidence threshold; O2 replace YuNet with RetinaFace MobileNet0.25; O3 keep the primary detector and add BlazeFace as a second stage that runs only on a primary "absent" result, with a user setting. O3 is built only if O1 and O2 fail.
**Why**: The research found no published numbers for YuNet on masks or for any candidate on hands over the face. The shipped threshold of 0.8 is high compared with the values used in mask studies, so O1 is the cheapest possible gain (no new asset, no latency change). RetinaFace MobileNet0.25 (0.426 M parameters, MIT code, family best on the MAFA benchmark) is the strongest permissive replacement. The owner wants one more complete model, not two selectable ones, so O3 is the last resort (FR-000).
**Alternatives Considered**: SCRFD and the YOLO-face variants (license), CenterFace (7.3 MB, no mask data), UltraFace (less accurate than YuNet), CAFACLite (weights license undeclared, single author), two selectable models from the start (rejected by the owner).
**Trade-offs**: A small CC0 fixture set gives wide confidence intervals, so the result is indicative and needs the owner's sign-off (Decision 3).

### Decision 2: Detector family is identified by the model's SHA-256; the threshold is a per-model constant

**What**: Replace the single `MODEL_SHA256` constant in `crates/telaviva-inference/src/lib.rs` with a small table of known models. Each row has the SHA-256, the family (`yunet` or `retinaface`), the input size, the preprocessing (mean values), the confidence threshold and the NMS IoU. The ABI (`tv_inference_create_with_provider_device`, `tv_inference_detect_bgra`) is unchanged: the shell still passes the model path, and the crate decides how to decode from the verified digest. The confidence threshold is not a user setting.
**Why**: No ABI change keeps `header_contract_tests.rs`, both shells and the packaging contract stable, and the integrity check keeps its meaning (an unknown file is a `MODEL_ERROR`). The threshold is tied to the model and the benchmark, so exposing it would let the user pick a value that was never measured.
**Alternatives Considered**: a threshold parameter in the C ABI (ABI churn, both shells change), a settings key (untested values, more UI), reading `manifest.toml` at runtime (the manifest is not read at runtime today; the duplicated constant is the existing pattern).
**Trade-offs**: The table lives in code and `manifest.toml`; both must change together, as today. The `tests`/packaging scripts already verify the digest.

### Decision 3: Acceptance rule for adopting an outcome (resolves SC-001)

**What**: An outcome (model plus threshold) is adopted only if all of the following hold on the fixture set, with the YuNet at 0.8 result as the baseline:
1. Pooled recall on the occluded categories (mask, hand over mouth/chin/cheek, hand over eyes) is at least 10 percentage points above the baseline.
2. Recall on unoccluded faces, glasses and tilted heads does not drop below the baseline.
3. False positives on face-free frames (hands alone, masks alone, objects) do not exceed the baseline count.
4. CPU inference p50 on one frame is at most 3 times the YuNet p50 and at most 60 ms on Apple Silicon, and the process memory increase after loading the session is at most 10 MB. The same latency check is run on Windows. The existing detector idle release (30 s) and the one-shot capture behavior stay.
5. The operating threshold is the highest value on the grid 0.30 to 0.90 (step 0.05) where the occluded recall is maximal with zero face-free false positives, plus one step as a safety margin.
If the occluded test set has fewer than 30 images, the report states the result as indicative, lists the coverage gaps and the owner confirms before adoption.
**Why**: A fixed rule written before measuring prevents choosing the result after seeing it. Rules 1 and 3 together target the stated problem (false absence) without trading it for false presence. Rule 4 protects the first project criterion (CPU, memory, battery).
**Alternatives Considered**: a percentage-point target of 20 (unreachable with the small set), a statistical significance test (not possible with the sample size), no rule (rejected: subjective).
**Trade-offs**: The thresholds (10 points, 3 times, 60 ms, 10 MB, 30 images) are judgment values. The owner delegated them to the design on 2026-10-29, so they stand as written. ADR 0041 (lock) is written in the setup phase and ADR 0042 (model) after the benchmark.

### Decision 4: Benchmark harness is a feature-gated Rust example, fixtures are small JPEGs with a license manifest

**What**: `crates/telaviva-inference/examples/occlusion_bench.rs`, compiled only with the `bench` feature (so it never enters the cdylib). It reads `crates/telaviva-inference/tests/fixtures/occlusion/manifest.toml` (see contracts.md), decodes JPEG fixtures with the `image` crate, runs each candidate model over the threshold grid and prints a markdown report: per-category recall with Wilson 95% intervals, face-free false positives, latency p50/p95 and the coverage gaps. Candidate models outside the known-model table load through an unpinned loader available only under `bench`. The same example runs on Windows for latency and for the DirectML check.
**Why**: Same runtime, decoder and preprocessing as production, so the numbers describe what ships. JPEGs keep the repository small (a PPM at 320 px is about 300 KB each). Recall does not depend on the platform, only latency, memory and execution-provider compatibility do.
**Alternatives Considered**: a Python script with `onnxruntime` (different preprocessing path, a second implementation of each decoder), committing PPM fixtures (repository size), fetching images at test time (network use needs an ADR and breaks offline CI).
**Trade-offs**: One more dev dependency (`image`, MIT/Apache-2.0), recorded in `licenses/DEPENDENCIES.md`.

### Decision 5: RetinaFace decoder follows the reference decode exactly

**What** (only if O2 is selected): input is 640x640 BGR, letterboxed as today, minus the per-channel mean (104, 117, 123), no scaling. Outputs are identified by last dimension (4 = location, 2 = confidence already softmaxed, 10 = landmarks), not by name or order. Priors are generated from the config (min sizes `[[16,32],[64,128],[256,512]]`, steps `[8,16,32]`, variances `[0.1, 0.2]`, clip off, 16800 priors at 640x640). The score is the second column of the confidence output. Boxes are decoded only for priors above the threshold; the landmarks are never decoded. NMS IoU 0.4. Presence is the maximum kept score at or above the operating threshold. The light gate still runs first. The static 640x640 export is used first; 480 or 320 exports are evaluated in the benchmark only if latency fails rule 4.
**Why**: These are the verified reference values; mapping outputs by shape avoids relying on export-dependent names. Skipping decode below the threshold keeps post-processing cheap.
**Alternatives Considered**: decoding all 16800 priors (needless work), relying on output names (export-dependent).
**Trade-offs**: The graph contains a shape chain feeding `Resize`; CoreML may partition it to CPU. This is acceptable because CoreML is a preference with CPU fallback (ADR 0027); the file may be simplified once with `onnx-simplifier` and the simplified file is the one pinned, with its provenance recorded. The ONNX file is chosen between the yakhyo release and the Axelera mirror after inspecting opset, shapes and outputs: the one with the clearest weights-license statement (the Axelera zoo lists MIT) is preferred. The WIDER FACE data license caveat applies to the current and the new model alike and stays a legal check for the owner.

### Decision 6: Second stage (O3) runs only after a primary "absent"

**What**: If needed, BlazeFace short range (128x128, Apache-2.0 model card, converted to ONNX with the conversion recorded) runs on the same frame only when the primary detector returns absent and the frame passed the light gate. A new setting `occlusion_assist` (boolean, default on, Advanced page) turns it off. Nothing is built for it unless O1 and O2 fail rule 3 or 4.
**Why**: The cost on frames where a face is found stays identical to today. This matches the owner's fallback instruction ("use both and create a setting").
**Alternatives Considered**: always running both (doubles the cost), a model selector (rejected by the owner).
**Trade-offs**: One more asset and one more setting in two shells, and a BlazeFace conversion step with its own provenance.

### Decision 7: Threshold floor is a validation change plus a load-time normalization

**What**: `Settings::validate` accepts 1 to 90 for `presence_min_brightness_percent` and for every `presence_min_brightness_by_display` entry. During `load`, before validation, any value in `[0, 1)` is raised to 1 (the same place where protection levels and sampling intervals are already clamped), so a stored 0 never triggers the whole-file reset. Values that were invalid before (negative, above 90, not a number) keep their existing behavior. The comparison stays strict "above": a monitor at or below the threshold suspends the camera.
**Why**: Without normalization, every user with a stored 0 (the previous legal minimum, including the `builtin` entry) would lose all settings on upgrade.
**Alternatives Considered**: validate-and-reset (loses settings), changing the comparison to `<` (not requested; would also change the 0 and 1 outcomes).
**Trade-offs**: A user who had 0 silently gets 1; with the strict rule the camera behavior at a DDC value of 0 or 1 does not change, so no user-visible regression.

### Decision 8: Lock stage lives in the core; the shells execute a one-shot request

**What**: The core keeps a small `LockStage` (Idle, Armed with the arming sample time, Requested with a token, Locked, Failed). It is recomputed at the end of every `presence()` call and after every accepted command, from the invariant `lock_on_absence && any_presence_dimmed() && session_active && camera_mode == "face"`. When the invariant is false the stage returns to Idle (this covers a present sample, insufficient light, unavailable camera, session lock, option off and protection off). The Locked stage is the exception for the session part of the invariant: it stays while the session is inactive and ends when the session becomes active or no display is presence-dimmed any more. The stage arms on the absent sample that produced the dim. An absent sample taken at least `lock_delay_s * 1000` ms after arming (by `captured_at_ms`) moves it to Requested. `lock_delay_s` is a number setting of 1 to 60, default 5; a change takes effect for the current episode because the delay is read when the sample is evaluated. The snapshot gets a `lock` block with `requested` and a `token`. The shell acts once per new token and answers with a `lock_result` command. A failed result keeps the dim and does not retry in the same absence episode.
**Why**: Dimming is applied to all protected displays together (misses are counted on each sample for every awake protected display), so "any display presence-dimmed" is the single condition and resolves the multi-monitor question in the spec. Insufficient light already clears all dims (ADR 0002), so FR-015 is satisfied by construction with no new lighting code. While dimmed the camera samples at the return interval (1 s by default), so the confirming sample arrives within one return interval after the configured delay (about 5 to 6 seconds with the defaults), and any present sample in between cancels the stage. The core stays free of OS calls, as required.
**Alternatives Considered**: a new `pending_actions` kind (capped at 8 and one per monitor, a lock is not per monitor), a controls-style effect (heavy machinery for one request), putting the timer in the shells (duplicated logic, two implementations to keep equal).
**Trade-offs**: A new command and a new snapshot block, documented in `api.md`. The C ABI and the header do not change. Older binaries reject the new settings field and reset the settings file, as already documented for earlier toggles (ADR 0003 amendments).

### Decision 9: The camera pauses while the session is locked by this feature

**What**: When the stage is Locked and the shell reports the session inactive, the core reports `camera.mode = "suspended"`. When the session becomes active again the stage returns to Idle, the camera generation is bumped and `begin_camera_resume()` runs (first sample about 1 s later), exactly like keep-awake. Brightness stays dimmed while locked (an OLED shows the lock screen otherwise) and the first present sample after unlock restores it through the existing path. If keep-awake with presence is also on, its existing rule (session inactive clears the dims) wins.
**Why**: No camera or battery cost while nobody can use the machine, and a clean resume on return.
**Alternatives Considered**: keep sampling while locked (battery cost for no benefit).
**Trade-offs**: Both shells must report the session lock state whenever either keep-awake or the lock option is on. Today the report is tied to the keep-awake controller, so this is a required change in each shell (see risks).

### Decision 10: macOS locks through one isolated private symbol and verifies the result; no fallbacks

**What**: A new `native/macos/TelaViva/SessionLocker.swift` is the only file that references `SACLockScreenImmediate` (`dlopen` of `/System/Library/PrivateFrameworks/login.framework/login`, `dlsym`, fail closed when absent), following the ADR 0007 isolation rule. After the call it polls the session state (the existing `CGSSessionScreenIsLocked` read, then the `com.apple.screenIsLocked` notification as a second signal) for up to 3 seconds and reports `ok` only when the lock is confirmed. A failure is logged and reported with `ok: false`. There is no osascript, `pmset` or keystroke fallback.
**Why**: It is the only route without a permission prompt that locks immediately regardless of the "require password" delay. The owner chose the most reliable mechanism, private or not, with verification and logging. A synthesized keystroke needs Accessibility and Automation permissions and was reported to fail on macOS 26.
**Alternatives Considered**: `CGSession -suspend` (removed in macOS 26), `pmset displaysleepnow` (locks only if the password delay is immediate), AppleScript Control+Command+Q (permissions, fragile), Shortcuts (user setup, unverified).
**Trade-offs**: Apple may remove the symbol in any release. The behavior is then "lock unavailable": no lock, a diagnostic line, the display stays dimmed, and the user guide says the feature depends on a system function that may change. The symbol must be tested on each macOS release and on the packaged, signed build.

### Decision 11: Windows locks with `LockWorkStation` behind an interface in `Platform`

**What**: `native/windows/Platform/SessionLocker.{h,cpp}` define `ISessionLocker` and a Win32 implementation that calls `LockWorkStation()`. A UI-free `LockCoordinator` in `Platform` owns the last token, the 3-second confirmation window and the `lock_result` submission. Confirmation comes from the existing `WM_WTSSESSION_CHANGE` / `WTS_SESSION_LOCK` callback already wired in `AppController`. A zero return or no lock event within the window reports `ok: false`.
**Why**: Same split as keep-awake (`IKeepAwakeBackend`), so the logic is tested with fakes (`FakeScheduler`, a fake locker) and the Win32 header stays in one `.cpp`. `LockWorkStation` is public, needs no privilege and works from the interactive-session tray process; its return value only says the request was started, so the session event is the confirmation.
**Alternatives Considered**: calling the API directly from `AppController` (not testable), `rundll32 user32.dll,LockWorkStation` (spawns a process for nothing).
**Trade-offs**: No documented fallback exists beyond the user pressing Win+L; policy and Dynamic Lock interactions were not documented and are covered by the hardware checklist.

### Decision 12: Settings, strings and documentation follow the keep-awake toggle pattern

**What**: One boolean `lock_on_absence` (default false) and one number `lock_delay_s` (default 5, range 1 to 60), both `#[serde(default)]` and whitelisted in `update_settings`, with the ten-step path used by `keep_awake_with_presence` (core, Swift `CoreBridge` and `SettingsView`, `Localization.swift`, Windows `JsonCodec`, `SettingsModel`, `GeneralPage`, both `Resources.resw`, tests, docs). The toggle sits with the presence options and is disabled while presence protection is off. The delay control (a slider or stepper on macOS, a number box on Windows, whole seconds) sits under the toggle and is disabled while the toggle is off. `update_settings` rejects a `lock_delay_s` outside 1 to 60; `Settings::load` clamps a stored value into the range, like the sampling intervals, so a hand-edited file never causes a whole-file reset. Documentation changes: `docs/user-guide.md` (the current statement that the app never forces a lock is replaced; the dark-room behavior and the "never unlocks" rule are added; the threshold range is updated), `ARCHITECTURE.md` (trigger scope), `api.md` (setting, `lock` block, `lock_result`), `CHANGELOG.md`, ADR 0041 (lock), ADR 0042 (model decision, written when the benchmark result is known), and a "superseded" note on the range in ADR 0011.
**Why**: One established pattern, both platforms in one change, strings in en-US and pt-BR.
**Alternatives Considered**: placing the option on the Advanced page (it is a user-facing behavior, not a tuning knob).
**Trade-offs**: A broad but mechanical change set.

## Architecture Overview

Before: the camera samples faces through YuNet (accepted at 0.8). Two no-face samples dim all protected displays; one present sample restores them; insufficient light is unknown presence and clears the dims. Nothing locks the session, and the threshold accepts 0 to 90.

After: the same flow, with these additions. The detector decision can change the accepted threshold, the model or add a second stage, with no change to the status codes the shells and the core already understand. The core derives a lock stage from the presence-dim state and issues one lock request per absence episode. The shells execute it, confirm it and report back. The threshold control stops at 1.

### Component Structure

**New Files/Modules:**
- `crates/telaviva-inference/examples/occlusion_bench.rs` - benchmark harness (feature `bench`)
- `crates/telaviva-inference/tests/fixtures/occlusion/manifest.toml` and the JPEG fixtures - fixture set with the license and source of each image
- `crates/telaviva-inference/src/retinaface.rs` - decoder and priors (only if O2)
- `assets/model/` entry and `licenses/inference/` provenance for any adopted model
- `crates/telaviva-core/src/lock.rs` - `LockStage` state and transition logic, unit-testable without the core
- `crates/telaviva-core/tests/lock_on_absence.rs` - contract tests for the lock flow
- `native/macos/TelaViva/SessionLocker.swift` - isolated private symbol and confirmation; `LockCoordinator` for token handling
- `native/macos/Tests/SessionLockTests.swift` and `tools/tests/test-session-lock.sh` - macOS tests with a fake locker
- `native/windows/Platform/SessionLocker.{h,cpp}`, `LockCoordinator.{h,cpp}` - interface, Win32 backend, coordinator
- `native/windows/Tests/session_lock.cpp` - Windows tests with fakes
- `adr/0041-lock-session-on-confirmed-absence.md`, `adr/0042-*.md` (model decision)

**Modified Files:**
- `crates/telaviva-inference/src/lib.rs` - known-model table, family dispatch, per-model threshold, `bench` feature; `Cargo.toml` features
- `crates/telaviva-core/src/settings.rs` - range 1 to 90, load normalization, `lock_on_absence`
- `crates/telaviva-core/src/core.rs` - whitelist, lock stage sync, `lock_result` command, camera suspension while locked, snapshot `lock` block
- `native/macos/TelaViva/CoreBridge.swift`, `SettingsView.swift`, `Localization.swift`, `MenuBarController.swift`, `DiagnosticLog.swift` (new `session` category), `PresenceCamera.swift` (model filename constant)
- `native/windows/Platform/JsonCodec.{h,cpp}`, `SettingsModel.{h,cpp}`, `native/windows/TelaViva/GeneralPage.xaml.cpp`, `Host/AppController.cpp`, both `Resources.resw`
- `assets/model/manifest.toml`, `tools/inference/prepare.py`, `packaging/macos/build-app.sh`, `packaging/windows/build-app.ps1`, `Install-TelaVivaOLED.ps1` (only if the model file changes)
- Tests that assert the old range: `core_contract.rs`, `settings_payload.cpp`, Swift range constants
- `docs/user-guide.md`, `README.md` (if it mentions the range or lock), `ARCHITECTURE.md`, `api.md`, `CHANGELOG.md`, `adr/0011-camera-brightness-activation-threshold.md`, `licenses/DEPENDENCIES.md`, `licenses/inference/PROVENANCE.md`

### Component Relationships

`PresenceCamera` (macOS) and `CameraLifecycleDriver`/`InferenceHost` (Windows) call the inference ABI and submit `presence_observation` to the core. The core updates dims and the lock stage, and publishes a snapshot. `MenuBarController` (macOS) and `AppController` (Windows) hand `snapshot.lock` to the lock coordinator, which calls the locker, then submits `lock_result`. Session lock/unlock events keep flowing to the core as `keep_awake_session`.

### Data Flow

**Absence with lock on, good light**: sample absent (miss 1), sample absent (miss 2) -> displays dim, stage Armed at that sample's time -> samples every return interval stay absent -> the first absent sample at least `lock_delay_s` after arming sets `lock.requested` with a token -> the shell locks and confirms -> `lock_result ok` -> stage Locked -> session lock event -> camera suspends -> unlock -> camera resumes, a present sample restores brightness.

**Return before the lock**: a present sample clears the dims -> the invariant fails -> stage Idle, no request.

**Dark room**: the dim lowers the light, a sample is insufficient light -> dims cleared (brightness restored) -> stage Idle -> no lock. The cycle repeats while absent, and the lock never fires.

**Lock failure**: `lock_result ok:false` -> stage Failed, dim stays, one diagnostic line, no retry until the episode ends (a present sample or insufficient light).

**Threshold at load**: `settings.json` with 0 -> normalization raises it to 1 -> validation passes -> all other settings preserved.

## Implementation Approach

### User Story Mapping

**US-001 / US-002: masks and hands over the face**
- Files involved: `crates/telaviva-inference/src/lib.rs`, the benchmark example and fixtures, `assets/model/`, packaging scripts, `PresenceCamera.swift` and `AppController.cpp` (filename constant only), docs and ADR 0042.
- Key components: benchmark harness, known-model table, RetinaFace decoder (conditional), second stage (conditional).
- Dependencies: fixture set (Decision 4), acceptance rule (Decision 3).
- Testing approach: the benchmark report is the evidence and is archived with the validation reports; the smoke tests keep asserting the CC0 portrait is present and a neutral frame is absent with the adopted model; fixtures that the adopted model detects get a regression test.

**US-003: threshold floor of 1%**
- Files involved: `settings.rs`, `SettingsView.swift`, `SettingsModel.cpp`, `GeneralPage.xaml.cpp`, docs, ADR 0011 note.
- Key components: validation range, load normalization, slider and number box minimums.
- Dependencies: none.
- Testing approach: `core_contract.rs` (0.0 rejected, 1.0 accepted, stored 0 loads as 1 with other keys kept, `builtin` entry), `settings_payload.cpp` clamp cases, a Swift constant test for the slider range.

**US-004 / US-005: lock after absence, never on bad light**
- Files involved: `lock.rs`, `core.rs`, `settings.rs`, both shells, strings, docs, ADR 0041.
- Key components: `LockStage`, `lock` snapshot block, `lock_result`, `SessionLocker`, `LockCoordinator`.
- Dependencies: session lock state reported by both shells whenever the option is on.
- Testing approach: contract tests for arming, the configured delay (default and a changed value, plus the range check and the load clamp), cancellation by present, insufficient light, unavailable, session inactive, option off and protection off; one request per episode; failed result; camera suspension and resume; default-off equivalence. Shell tests with fake lockers for token deduplication, confirmation timeout and the failure report. Hardware checklist items on both platforms.

**US-006: the option in Settings, en-US and pt-BR**
- Files involved: `Localization.swift`, `SettingsView.swift`, `GeneralPage.xaml.cpp`, both `Resources.resw`, `strings_keys.cpp` parity test.
- Testing approach: key-parity tests, round-trip tests of the setting on each side.

## Integration Points

- Core to shells: `lock` snapshot block and `lock_result` command over the existing JSON channel; no header or ABI version change.
- Shells to core: session lock state (`keep_awake_session`) must be reported whenever keep-awake or the lock option is on.
- Packaging: only changes if a new model file is adopted (`manifest.toml`, `prepare.py`, `build-app.sh`, `build-app.ps1`, installer).
- Licenses: new dev dependency `image`, any adopted model and the fixture images need entries in `licenses/`.

## Technical Constraints

- Both platforms in the same change, with en-US and pt-BR strings and tests on each (AGENTS.md).
- No new network use; the lock does not change any OS preference and needs no elevation.
- Presence still means any face, with no identification; frames stay local.
- The light gate (insufficient light is unknown, not absence) is unchanged (ADR 0002).
- The existing two-miss rule (ADR 0030) is unchanged; the wait before the confirming sample is the `lock_delay_s` setting (1 to 60 s, default 5).
- Only redistributable assets: fixture images must be CC0 or public domain, checked per file, with source and license recorded.
- Private symbol use is isolated to one file per platform and fails closed (ADR 0007 pattern); diagnostic lines never carry identifiers (ADR 0034).

## Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| The CC0 fixture set is small and uneven | Wide intervals; a wrong model choice | Fixed acceptance rule, safety margin step, owner sign-off under 30 occluded images, gaps listed in the report |
| Lock fires on a false absence (user present but undetected) | Unwanted lock, password prompt | Opt-in and off by default, two misses plus the configured delay (5 s by default, at least 1 s) of continuous absence, light gate, benefit of the item 1 recall work, one request per episode |
| `SACLockScreenImmediate` removed or changed | No lock on macOS | Fail closed, log, keep the dim, test on each macOS release and the signed build, documented limitation |
| macOS lock-state signals were reported unreliable on macOS 26 | A false "locked" or a false failure | Confirm with the session dictionary and the notification together; report only confirmed locks; hardware check |
| Hardened runtime or signing blocks `dlopen` of the private framework | No lock in the packaged app | Verify on a notarized or locally signed build before release |
| Shells report session lock state only when keep-awake is on | The core thinks the session is active after a lock; camera keeps sampling; no resume signal | Required change (Decision 9): report whenever either option is on; contract test and shell tests |
| RetinaFace costs more than YuNet | Battery and CPU | Budget rule 4, smaller input exports, CPU path measured on both platforms, one-shot capture and idle release unchanged |
| RetinaFace shape chain (`Resize`) not supported by CoreML or DirectML | Model runs on CPU only | Preference with CPU fallback (ADR 0027), simplified graph pinned with its provenance, self-test gating unchanged |
| WIDER FACE license (CC BY-NC-ND) may restrict the trained weights, for the current model too | Legal exposure | Flagged for the owner; not a blocker for this feature |
| Older binaries reject the new settings field and reset the file | Settings loss on downgrade | Same caveat as earlier toggles; noted in the changelog |
| Windows policy or Dynamic Lock changes `LockWorkStation` behavior | Lock not confirmed | Confirmation by the WTS lock event, report `ok:false`, hardware checklist |
| Stored 0 threshold after upgrade | Settings reset | Load normalization (Decision 7) and a test |

## Open Questions

None blocking. Two items are verified during implementation, not decided: the exact ONNX file for RetinaFace (opset, shapes and outputs are read from the file before its decoder is written) and whether the packaged macOS build allows the `dlopen` of the login framework.

## Next Steps

1. Review this architecture.
2. Resolve any open questions.
3. Run `/codeadvisor-dev:plan`.
