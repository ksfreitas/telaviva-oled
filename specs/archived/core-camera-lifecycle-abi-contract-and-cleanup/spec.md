# Chore: Move the camera lifecycle into the core, test the C headers, and clean up docs, build and localization

## Chore Description

Three related improvements that make the Rust core carry more of the platform-independent policy before the Windows shell starts, and remove documentation and build drift found during a code survey.

1. **Move the camera lifecycle state machine into `telaviva-core`.** `native/macos/TelaViva/CameraLifecycle.swift` (lines 5-118, the `CameraLifecycle` struct and its `FrameDecision` enum) is pure logic with no Apple SDK types and already takes time as a parameter (`now: TimeInterval`). It decides session retention, the 5 s acquisition deadline, the 5 s warm-up/settle window, the once-per-sample dark settle, the stale-callback check and the assessment token. It moves to Rust as `camera_lifecycle.rs` and is exposed through a small new C ABI handle (`TvCameraLifecycle`, three functions, JSON events in and JSON results out, matching the project's existing JSON protocol). Swift keeps only a thin wrapper with the same public API, so `PresenceCamera.swift` changes as little as possible. Swift keeps the real timers (`DispatchSource`, `asyncAfter`), the monotonic clock and AVFoundation. Rust has no clock: the shell passes `now` in seconds on every event, as it does today.
2. **Test the hand-written C headers against the Rust ABI.** `include/telaviva_core.h` and `include/telaviva_inference.h` are checked by no test. Add a std-only Rust test per crate that (a) pins every exported function with an explicitly typed function-pointer cast, so the Rust side cannot drift without a compile error, and (b) parses the header text and compares prototypes, struct fields and constants against that pinned table. No new dependency and no CI change: the existing `cargo test --workspace --all-targets --locked` step runs it. Also fix the documentation drift in the headers that the survey found.
3. **Clean up documentation, the Xcode project and localization.**
   - Repoint the two live instructions that still treat the 7-line archived stub `specs/oled-monitor-protection/architecture.md` as the design (`AGENTS.md:27` and `ARCHITECTURE.md:36`) to the permanent documents.
   - Fix `native/macos/TelaViva.xcodeproj/project.pbxproj` so Release no longer links `target/debug/` Rust libraries.
   - Remove the duplicated localization catalog: the core serves 21 `.ftl` messages in every snapshot, but only 4 are read by Swift and the other 116 UI strings live in `Localization.swift`. Native shells become the single owner of UI strings; the core keeps the `locale` setting and returns status codes only.

**Decisions taken in this plan (record them in ADRs, step 8):**

- The camera lifecycle moves to the core as a new, additive ABI surface. `tv_abi_version()` stays `1`: `api.md:95` and the settings paragraph already treat additive changes as "additive to core ABI version 1".
- UI strings are owned by each native shell. This partially supersedes the "static locale catalogs cover current messages" statement in ADR 0001.
- `CameraLifecycle` ownership split: the core's `camera_mode` and `capture_mode` (core.rs `camera_mode` ~776, `snapshot_value` ~1326-1334) remain the only policy for *whether* the camera runs; the new lifecycle only owns session timing for the current mode. The shell passes the snapshot's mode and capture mode into the lifecycle, exactly as it does today.

**Explicitly out of scope (deferred, with reasons in Notes):** moving the calibration session machine (`AmbientCalibrationController`), moving the `HardwareKeyRouter` keystroke latch, moving `chooseFrameRate`/`frameDuration`, removing the unused `os_locale` option, and building the Windows shell.

## Relevant Files

Use these files to complete the chore:

- `native/macos/TelaViva/CameraLifecycle.swift`: source of truth for the behavior to port (lines 5-118 move; lines 120-181 hold the frame-rate helpers that stay in Swift and move to a new file).
- `native/macos/Tests/CameraLifecycleTests.swift`: 246-line fake-clock test; sections at lines 26-175 define the behavior the Rust tests must reproduce; lines 177-242 test frame-rate helpers and stay in Swift.
- `native/macos/TelaViva/PresenceCamera.swift`: the only consumer of `CameraLifecycle` (timer scheduling around 331-337, frame handling around 411-438, `cancelDeadlines` use at 542). Grep every `lifecycle.` use and make the wrapper cover each one.
- `native/macos/TelaViva/CoreBridge.swift`: shows how the Swift side uses the C ABI, `#if TELAVIVA_CORE` guards, `TvBuffer` handling and `ffiLock`; also holds the `messages` snapshot field (line 275) to remove.
- `native/macos/TelaViva/Localization.swift`: Swift catalog; the `coreKey` mapping at lines 10-11 is removed.
- `native/macos/TelaViva/MonitorPopup.swift`: the only reader of core `messages` (lines 13, 42, 70, 72).
- `crates/telaviva-core/src/ffi.rs`: existing C ABI pattern to copy (null checks, `catch_unwind` at 49-51, 64 KiB input cap at 79/121, `TvBuffer` ownership, status constants at 15-20).
- `crates/telaviva-core/src/lib.rs`: module wiring (6 lines).
- `crates/telaviva-core/src/core.rs`: `localized_messages` (~1352-1375) and the `messages` key in `snapshot_value` (line 1341) are removed.
- `crates/telaviva-core/tests/core_contract.rs`: test `locale_catalogs_have_matching_keys_and_auto_locale_uses_native_locale` (line 523) and any other `messages`/`status-ready` assertion (for example line 538) must be rewritten.
- `crates/telaviva-core/tests/ffi_contract.rs`: existing ABI test pattern; extend for the new lifecycle functions.
- `crates/telaviva-core/locales/en-US.ftl` and `pt-BR.ftl`: deleted.
- `crates/telaviva-inference/src/lib.rs`: exports (lines 66-335), `repr(C)` types (25-36) and private status/provider constants (38-48) that the header test must read; the test lives inside the crate because the constants are private.
- `include/telaviva_core.h` and `include/telaviva_inference.h`: the hand-written headers; add the lifecycle declarations and fix the documentation drift listed in step 3.
- `native/macos/TelaViva/TelaViva-Bridging-Header.h`: includes both headers; no change expected.
- `api.md`: public interface document. Update for the lifecycle ABI, the removed `messages` field and the header test. Line 87 repeats the incorrect claim that `tv_inference_detect_bgra` can return `3`.
- `AGENTS.md` and `ARCHITECTURE.md`: stale links (`AGENTS.md:14`, `AGENTS.md:27`, `ARCHITECTURE.md:36`) and localization statements (`ARCHITECTURE.md:20`, `:113-114`).
- `native/macos/TelaViva.xcodeproj/project.pbxproj`: lines 77-78 (`TELAVIVA_CORE_LIB`, `TELAVIVA_INFERENCE_LIB`), plus the Swift source lists (lines 7-27) that need the new `CameraFrameRate.swift`.
- `packaging/macos/build-app.sh`: `SOURCES` array at line 41 needs the new Swift file.
- `packaging/macos/README.md`: document the Xcode project's prerequisites and that `scripts/build-macos.sh` is the verified build path.
- `tools/tests/test-camera-lifecycle.sh`: compiles `CameraLifecycle.swift` standalone; must switch to `CameraFrameRate.swift`.
- `tools/tests/test-core-bridge.sh`: template for the new bridge test script.
- `.github/workflows/macos.yml`: add one step for the new bridge test after "Test asynchronous Rust bridge" (the Rust release library exists only after "Build Rust libraries for Apple Silicon").
- `adr/0001-native-shell-and-rust-core.md`, `adr/0003-camera-lifecycle-and-display-suspension.md`, `adr/0013-user-managed-calibration-points.md`: format reference and statements to amend or link. The highest existing ADR is 0013.
- `CHANGELOG.md`: Unreleased entries.
- `specs/oled-monitor-protection/README.md`, `architecture.md`, `contracts.md`: stay as navigation stubs (consistent with `ARCHITECTURE.md:106` and the "archive rather than discard" convention at `ARCHITECTURE.md:92`); only their referrers change.

### New Files

- `crates/telaviva-core/src/camera_lifecycle.rs`: Rust port of the lifecycle state machine and its JSON event/result adapter.
- `crates/telaviva-core/tests/camera_lifecycle_contract.rs`: Rust port of the fake-clock tests, plus ABI-level tests of the three new functions.
- `crates/telaviva-core/tests/header_contract.rs`: header-versus-ABI test for `telaviva_core.h`.
- `crates/telaviva-inference/src/header_contract_tests.rs`: header-versus-ABI test for `telaviva_inference.h` (a `#[cfg(test)]` module, so it can read the private constants).
- `native/macos/TelaViva/CameraFrameRate.swift`: the frame-rate helpers moved out of `CameraLifecycle.swift` (pure Swift, no ABI dependency).
- `native/macos/Tests/CameraLifecycleBridgeTests.swift`: Swift tests for the wrapper, linked against the Rust core library.
- `tools/tests/test-camera-lifecycle-bridge.sh`: builds and runs the bridge test, modeled on `tools/tests/test-core-bridge.sh`.
- `adr/0014-camera-lifecycle-in-the-core.md` and `adr/0015-native-shells-own-ui-strings.md`: decision records (renumber if another ADR was added in the meantime).

## Step by Step Tasks

IMPORTANT: Execute every step in order, top to bottom. Verify each step (build, tests) before starting the next. Do not commit.

### 1. Repoint the stale spec references

- In `AGENTS.md:27`, replace the instruction to follow `specs/oled-monitor-protection/architecture.md` with one that points to `ARCHITECTURE.md`, `api.md` and the ADRs in `adr/` (especially `adr/0001-native-shell-and-rust-core.md`), keeping the same listed design points (native SwiftUI/AppKit and WinUI 3 shells, one shared Rust core with a versioned C ABI, local YuNet inference through ONNX Runtime, conditional low-energy acceleration). Leave the `## Delegation` section untouched.
- In `AGENTS.md:14`, change the project-map entry for `specs/oled-monitor-protection/` so it says the directory only holds navigation pointers to archived records (matching `ARCHITECTURE.md:106`).
- In `ARCHITECTURE.md:36`, replace the links to the stub `architecture.md` and `contracts.md` ("the feature architecture", "runtime contracts") with links to the archived originals under `specs/archived/external-monitor-control-and-oled-presence-protection/` and to `api.md`. Keep the stub files themselves.
- Check `ARCHITECTURE.md:125` (links `specs/camera-lifecycle-and-display-suspension.md`, itself a navigation stub) and fix it the same way if it points a reader to a stub for live content.
- Do not rewrite the archived architecture document. `ARCHITECTURE.md:106-110` already documents that its paths predate archival.

### 2. Fix the Xcode link paths

- In `native/macos/TelaViva.xcodeproj/project.pbxproj` lines 77-78, keep Debug at `$(PROJECT_DIR)/../../target/debug/libtelaviva_{core,inference}.a` and change Release to `$(PROJECT_DIR)/../../target/aarch64-apple-darwin/release/libtelaviva_{core,inference}.a`. That is the path `scripts/build-macos.sh:49-52` actually produces. Change only `TELAVIVA_CORE_LIB` and `TELAVIVA_INFERENCE_LIB` in the Release block.
- Add a short section to `packaging/macos/README.md`: the `.xcodeproj` has no Rust build phase, so Debug needs `cargo build --workspace --locked` first and Release needs `cargo build --workspace --release --target aarch64-apple-darwin --locked`; it does not bundle the ONNX Runtime, models or licenses, so `scripts/build-macos.sh` remains the verified build path (see ADR 0009, line 49).
- Do not add a Run Script phase and do not edit `packaging/macos/build-app.sh` defaults in this step (see Notes).

### 3. Header-versus-ABI contract tests, plus header documentation fixes

Design (std only, no new dependency):

- Each test keeps a table of every exported function: `name`, the canonical C prototype with parameter names removed (for example `int32_t tv_submit(TvCore *, const uint8_t *, size_t)`), and an explicitly typed function-pointer cast of the real Rust function (for example `let _: unsafe extern "C" fn(*mut TvCore, *const u8, usize) -> i32 = ffi::tv_submit;`). The cast makes the Rust signature a compile-time fact. For functions that are `extern "C"` but not `unsafe`, use the matching type.
- The test reads the header with `include_str!`, strips `/* ... */` and `// ...` comments, joins lines, splits declarations on `;`, and canonicalizes each function prototype: put spaces around `*`, collapse whitespace, split parameters on top-level commas, drop the trailing parameter identifier when a parameter has two or more tokens other than `*` (so `void` stays `void`), and render `const uint8_t * json` as `const uint8_t *`. It then asserts the set of header function names equals the table's names and that each canonical prototype equals the table entry.
- Struct fields (`TvBuffer`, `TvInferenceResult`) are compared as ordered `(type, name)` pairs against a table, with a fixed C-to-Rust type map (`uint8_t *` to `*mut u8`, `size_t` to `usize`, `int32_t` to `i32`, `float` to `f32`, `uint32_t` to `u32`, `uint64_t` to `u64`).
- Constants: parse both `#define NAME value` (strip the `u` suffix) and anonymous `enum { NAME = value }` entries into a name-to-integer map and compare to a table that lists the header name, the expected value and the Rust constant it must equal. Known name differences are explicit entries in the table: header `TV_CORE_ABI_VERSION` versus Rust `ABI_VERSION`; header `TV_INFERENCE_ERROR_RUNTIME`/`_MODEL`/`_PANIC`/`_INVALID_ARGUMENT` versus the Rust private names (read them in `crates/telaviva-inference/src/lib.rs:38-48`). The Rust `SUCCESS = 0` (inference lib.rs:42) has no header name; allowlist it explicitly with a comment.
- Opaque types: assert the header declares `TvCore` and `TvInferenceHandle` as incomplete typedefs, and allowlist the existing `#[repr(C)]` on `TvInferenceHandle` (it holds a `Mutex<Session>` and is opaque to C) with a comment.
- A new `pub` exported function that is missing from the table must fail the test. Do this by scanning the Rust source with `include_str!("../src/ffi.rs")` (core) or `include_str!("lib.rs")` (inference) for `extern "C" fn tv_` names and asserting the set equals the table names, so forgetting a table entry fails loudly.

Files:

- `crates/telaviva-core/tests/header_contract.rs` for `include/telaviva_core.h` (public API only: `telaviva_core::ffi::*`; make the `TV_*` status constants `pub` if they are not already).
- `crates/telaviva-inference/src/header_contract_tests.rs` with `#[cfg(test)] mod header_contract_tests;` in `crates/telaviva-inference/src/lib.rs`, because the status/provider constants are private. Do not make those constants public just for the test.

Header and doc fixes found by the survey (comments and docs only; no ABI change):

- `include/telaviva_inference.h:46-48` says `tv_inference_detect_bgra` can return `3` (unavailable). It never does; only `tv_inference_create_with_provider` returns it (`crates/telaviva-inference/src/lib.rs:107`). Correct the text, and make the `tv_inference_create` comment (`:34`) list its real return codes (`0`, `-1`, `-3`, and `3` where applicable; verify against the source before writing).
- `include/telaviva_core.h:22`: mention that `tv_submit` returns `TV_CLOSED` after shutdown except where the core lets `hardware_result` through (see `crates/telaviva-core/src/ffi.rs:130-141`).
- `api.md:87`: remove the same incorrect claim about `tv_inference_detect_bgra`.
- Add a sentence to `api.md` and to `ARCHITECTURE.md`'s testing paragraph noting that the headers are checked against the Rust ABI by `header_contract` tests.

Run `cargo test -p telaviva-core --test header_contract` and `cargo test -p telaviva-inference header_contract` after this step. Both must pass against the current headers, which the survey found to match.

### 4. Port `CameraLifecycle` to Rust

Create `crates/telaviva-core/src/camera_lifecycle.rs` and add `pub mod camera_lifecycle;` to `crates/telaviva-core/src/lib.rs`.

- Read `native/macos/TelaViva/CameraLifecycle.swift` lines 5-118 in full and port the struct line by line. Fields: `generation`, `camera_id`, `mode` (default `"disabled"`), `capture_mode`, `always_on`, `settling_until`, `assessment_deadline`, `acquisition_deadline`, `assessment_token` (`u64`, use `wrapping_add` where Swift uses `&+=`, at Swift lines 66, 77, 112), `dark_settle_used`. Computed: `eligible`, `acquisition_pending`, `should_keep_session`, `accepts_callback(generation)`. Methods: `reconcile`, `begin_acquisition(now)`, `acquired_frame`, `begin_light_assessment(now)`, `begin_dark_settle(now)`, `is_current_assessment(token)`, `frame(at)`, `timed_out(at)`, `cancel_deadlines` (must reset `dark_settle_used`, Swift line 116). `FrameDecision` becomes an enum with `DiscardWarmup`, `AssessLight`, `AssessPresence`, `AssessmentExpired`. Time is `f64` seconds supplied by the caller; the module must not read any clock. Keep the hard-coded 5 s windows (Swift lines 55, 64-65, 73-74) and the same boundary comparisons (`<` versus `<=`) as the Swift code.
- Add a JSON adapter in the same module: `pub fn step(&mut self, event_json: &[u8]) -> Result<String, StepError>` (or equivalent) that parses one event object, applies it and returns one result object. Reject unknown fields and unknown event types the way `Core::submit_json` does (see core.rs ~137-266; reuse its duplicate-key rejection helper `parse_json_unique` if it can be shared without changing its behavior).
  - Events (`type` plus fields): `reconcile {mode, capture_mode, generation, camera_id, always_on}`, `begin_acquisition {now}`, `acquired_frame {}`, `begin_light_assessment {now}`, `begin_dark_settle {now}`, `is_current_assessment {token}`, `frame {now}`, `timed_out {now}`, `cancel_deadlines {}`, `accepts_callback {generation}`.
  - Result: always `{"value": <event-specific>, "state": {...}}`. `value` is a Boolean for `reconcile` (stop the session), `begin_dark_settle`, `is_current_assessment`, `timed_out` and `accepts_callback`; a string (`discard_warmup`, `assess_light`, `assess_presence`, `assessment_expired`) for `frame`; and `null` otherwise. `state` is a flat object with every field Swift reads from the lifecycle: `generation`, `camera_id`, `mode`, `capture_mode`, `always_on`, `eligible`, `acquisition_pending`, `should_keep_session`, `settling_until`, `assessment_deadline`, `acquisition_deadline`, `assessment_token`. Before finalizing, grep every `lifecycle.` use in `PresenceCamera.swift` and add any field it reads that is missing from this list.
- Port the tests to `crates/telaviva-core/tests/camera_lifecycle_contract.rs` with an explicit fake clock (a plain `f64` that the test advances). Reproduce, test for test, the sections of `native/macos/Tests/CameraLifecycleTests.swift` at lines 26-175: session reuse and stale callbacks (26-41); warm-up discard, post-settle assessment, frozen-stream timeout and the 5 s acquisition (43-70); delayed pre-settle frame (72-86); suspension, camera replacement and shutdown stop the session (88-114); identical snapshot keeps the return or one-frame session (116-131); dark settle for single-frame face mode, 5 s, once per sample, final report (133-165); light-only never dark-settles (167-175). Add tests for token wrapping (`u64::MAX` wraps to `0` without panic in debug builds), unknown event type, unknown field, malformed JSON, oversized input, and that two events with the same `now` are deterministic. Test names should be descriptive snake_case sentences, matching `core_contract.rs`.

### 5. Add the `TvCameraLifecycle` C ABI

- In `crates/telaviva-core/src/ffi.rs` add, following the existing pattern exactly (null checks, `catch_unwind`, status codes `TV_OK`..`TV_SIZE_LIMIT`, 64 KiB input cap, Rust-owned `TvBuffer` freed by `tv_buffer_free`):
  - `tv_camera_lifecycle_create(out: *mut *mut TvCameraLifecycle) -> i32`
  - `tv_camera_lifecycle_step(handle: *mut TvCameraLifecycle, json: *const u8, len: usize, out: *mut TvBuffer) -> i32`
  - `tv_camera_lifecycle_destroy(handle: *mut TvCameraLifecycle)`
- `TvCameraLifecycle` is an opaque handle wrapping a `Mutex<CameraLifecycle>`; reuse the existing safety comments and the same `Send`/`Sync` reasoning as `TvCore` (`ffi.rs:37-38`). A malformed or rejected event returns `TV_INVALID_ARGUMENT` and leaves the state unchanged; a poisoned mutex or panic returns `TV_INTERNAL`.
- Declare the three functions and the opaque typedef in `include/telaviva_core.h`, with doc comments that state ownership (the buffer must be freed with `tv_buffer_free`) and that the shell supplies all time.
- Extend the step 3 table in `crates/telaviva-core/tests/header_contract.rs` with the three new functions (their `extern "C"` names are caught by the source scan, so omitting them fails the test).
- Add ABI tests to `crates/telaviva-core/tests/camera_lifecycle_contract.rs` (or `ffi_contract.rs`) for create/step/destroy, null handles, null/zero-length input, oversize input, buffer ownership and a `step` on a destroyed-then-recreated handle.
- Do not change `tv_abi_version()` and keep `TV_CORE_ABI_VERSION` at `1`. Add the functions to `api.md` (function table at line 13) and describe them as additive to ABI version 1.

### 6. Rewire Swift to the new ABI

- Create `native/macos/TelaViva/CameraFrameRate.swift` containing the pure frame-rate code now at `CameraLifecycle.swift:120-181` (`sessionFrameRate`, `FrameRateRange`, `FrameRateEdge`, `FrameRateChoice`, `frameRateEdgeTolerance`, `chooseFrameRate`, `frameDuration`). Move it verbatim; this code stays in Swift.
- Rewrite `native/macos/TelaViva/CameraLifecycle.swift` as a thin wrapper that keeps the same public surface used by `PresenceCamera.swift` (`reconcile`, `beginAcquisition`, `acquiredFrame`, `beginLightAssessment`, `beginDarkSettle`, `isCurrentAssessment`, `frame(at:)`, `timedOut(at:)`, `cancelDeadlines`, `acceptsCallback`, and the read-only state properties). It owns an opaque `TvCameraLifecycle` pointer, encodes each call as the step 4 JSON event, decodes the result (including `state`), and exposes the cached state as properties. Free the returned `TvBuffer` with `tv_buffer_free` on every path, create the handle in `init`, destroy it in `deinit`, and treat any non-`TV_OK` status as a programmer error that stops the session (return the "stop" value) rather than crashing. Mirror the `#if TELAVIVA_CORE` guards used in `CoreBridge.swift` so the file still compiles in test targets built without the ABI.
- Keep `FrameDecision` as a Swift enum decoded from the result string so `PresenceCamera.swift` needs no change beyond what the grep in step 4 requires. Confirm single ownership: the wrapper is mutated only on the camera queue (`PresenceCamera.swift:25`), the same as the struct today.
- Add `CameraFrameRate.swift` to the `SOURCES` array in `packaging/macos/build-app.sh:41` and to `project.pbxproj` (file reference, build file and group entries; copy the pattern of an existing Swift file such as `CameraSelection.swift`).
- Change `tools/tests/test-camera-lifecycle.sh` so the first compile uses `CameraFrameRate.swift` plus a new `native/macos/Tests/CameraFrameRateTests.swift` (the frame-duration and `chooseFrameRate` sections now at `CameraLifecycleTests.swift:177-242`, moved without changes). Delete the lifecycle-behavior sections (lines 1-175 of the old test file) from the Swift test, because they now live in Rust. The `CameraSelection` and `CameraLocalization` compiles in that script stay.
- Create `native/macos/Tests/CameraLifecycleBridgeTests.swift` and `tools/tests/test-camera-lifecycle-bridge.sh` (copy `tools/tests/test-core-bridge.sh`, link `CameraLifecycle.swift` and the test with `-D TELAVIVA_CORE`, the bridging header and the core library). Keep the tests small: one happy path through acquisition, warm-up discard and assessment; one stale-callback rejection; one malformed-state failure path; and an assertion that the wrapper frees buffers (create and step 1000 times without growth in RSS is overkill; a loop that calls `step` 10 000 times and completes is enough).
- Add one step to `.github/workflows/macos.yml` after "Test asynchronous Rust bridge" named "Test camera lifecycle bridge" that runs `tools/tests/test-camera-lifecycle-bridge.sh`. It must come after the Rust release library is built.
- Rename the README test list entry accordingly (`README.md` "Development checks" list: add `tools/tests/test-camera-lifecycle-bridge.sh`).

### 7. Remove the duplicate localization catalog

- In `crates/telaviva-core/src/core.rs` remove `localized_messages` (~1352-1375), its `include_str!` calls and the `messages` key in `snapshot_value` (line 1341). Delete `crates/telaviva-core/locales/en-US.ftl` and `pt-BR.ftl`. Grep for any other `messages`, `locales/` or `.ftl` reference in the repository (including `Cargo.toml` `include` lists, packaging scripts and docs) and remove it.
- Keep the `locale` setting (`settings.rs:19-20`, default `auto`, validation at `:232-233`) unchanged: the shells read it to choose their catalog. Keep parsing `os_locale` in `tv_create` options unchanged so the documented ABI contract does not change, and mark it in `api.md` as accepted but no longer affecting snapshot content (follow-up in Notes).
- Rewrite `locale_catalogs_have_matching_keys_and_auto_locale_uses_native_locale` (`core_contract.rs:523`) into a test that asserts the snapshot no longer has a `messages` key and that `locale` values `auto`, `en-US` and `pt-BR` still validate and invalid ones are rejected (reuse the existing settings validation test at `core_contract.rs:20` where possible). Update every other test that reads `messages` (for example the `status-ready` assertion near `core_contract.rs:538`).
- Swift: remove the `messages` field from `CoreSnapshot` (`CoreBridge.swift:275`), the `coreKey` mapping and the snapshot-messages branch in `AppText.string` (`Localization.swift:9-14`, especially `:10-11`), and the `messages:` argument at `MonitorPopup.swift:13, 42, 70, 72`. The Swift dictionary already holds `app`, `settings`, `brightness` and `volume` (verify the four keys exist in both locales before deleting the mapping). Update `native/macos/Tests/CoreBridgeTests.swift`, `CameraLocalizationTests.swift` and any fixture that decodes or builds a snapshot with `messages`.
- Update docs that mention core messages or Fluent catalogs: `api.md`, `ARCHITECTURE.md:20` and `:113-114`, and the archived-spec-derived text that is still in the live docs. Do not edit archived specs.

### 8. Write the ADRs and documentation

- Read `adr/0013-user-managed-calibration-points.md` and `adr/0003-camera-lifecycle-and-display-suspension.md` for the format and tone (Status, Date, Feature, Spec, Context, Decision, Alternatives Considered, Consequences). Create the two ADRs listed in New Files, using the next free numbers after the current highest.
  - ADR 0014, "Camera lifecycle in the core": session timing moves to the core behind `TvCameraLifecycle`; Swift keeps timers, the monotonic clock and AVFoundation; additive to ABI 1; the core's `camera_mode`/`capture_mode` stay the only policy for whether the camera runs; alternatives considered: sending every frame decision through `tv_submit` (rejected: each accepted command bumps the revision and republishes the snapshot, so per-frame decisions would amplify snapshot publishing, see `specs/snapshot-publish-amplification.md`), leaving it in Swift (rejected: Windows would duplicate the deadlines and the dark-settle rule), and UniFFI (rejected: the ABI is 14 functions plus three, JSON-based, and the C# generator is community-maintained). Amend ADR 0003 with a one-line link to the new ADR; do not rewrite its text.
  - ADR 0015, "Native shells own UI strings": partially supersedes the statement in ADR 0001 that static locale catalogs cover current messages in the core; the core keeps the `locale` setting and returns status codes; each shell owns its catalog (`Localization.swift` now, resource files on Windows); alternatives considered: a core-owned catalog served in every snapshot (rejected: it grows each snapshot, see the snapshot amplification spec, and only 4 of 21 keys were read) and a single catalog fetched once through a new function (rejected for now: larger change, and WinUI conventionally uses its own resources).
- Update `CHANGELOG.md` under Unreleased with entries per the repository's existing style: camera lifecycle moved to the core (additive ABI), header contract tests and header documentation fixes, Xcode Release library paths, core snapshot no longer carries `messages`, removed `.ftl` catalogs, and the repointed documentation.
- Update `api.md`: the new three functions, the removed `messages` field, the `os_locale` note and the header tests.
- Update `README.md` "Project layout" only if a path it lists changed. Keep the statement that hardware acceptance of the camera is pending: the lifecycle wiring in `PresenceCamera.swift` is not covered by an automated test and has not been re-verified on hardware after this change (the repository's rule is to keep intended behavior distinct from hardware-verified behavior).

### 9. Run Validation Commands

Run every command listed in Validation Commands, fix any failure at its cause, and re-run until all pass. Report any step that could not be run.

## Validation Commands

Execute every command to validate the chore is complete with zero regressions.

- `cargo fmt --all -- --check`: formatting.
- `cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu`: all Rust tests, including the new `camera_lifecycle_contract`, `header_contract` and `header_contract_tests`.
- `cargo test -p telaviva-core --test header_contract`: header test for the core ABI.
- `cargo test -p telaviva-inference header_contract`: header test for the inference ABI.
- `cargo clippy --workspace --all-targets --locked -- -D warnings`: lints, as CI runs them.
- `cargo build --workspace --release --target aarch64-apple-darwin --locked`: builds the libraries the Swift tests and the app link against.
- `tools/tests/test-camera-lifecycle.sh`, `tools/tests/test-camera-lifecycle-bridge.sh`, `tools/tests/test-core-bridge.sh`, `tools/tests/test-manual-monitor-controls.sh`, `tools/tests/test-popup-layout.sh`, `tools/tests/test-builtin-brightness-monitor.sh`, `tools/tests/test-ambient-calibration.sh`, `tools/tests/test-ddc-transaction.sh`, `tools/tests/test-hardware-key-router.sh`, `tools/tests/test-default-audio-output.sh`, `tools/tests/test-media-key-probe.sh`, `tools/tests/test-status-item-icon.sh`, `tools/tests/test-branding-assets.sh`: the full native test list from `README.md`.
- `./scripts/build-macos.sh`: builds, packages and ad-hoc signs the app with the changed Swift sources; then `"build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva" --smoke-test` to confirm the ABI handshake and snapshot still work with `messages` removed.
- `plutil -lint native/macos/TelaViva.xcodeproj/project.pbxproj`: the edited project file parses.
- `xcodebuild -project native/macos/TelaViva.xcodeproj -configuration Release -showBuildSettings | grep TELAVIVA_`: confirms the Release paths now point at `target/aarch64-apple-darwin/release/`; repeat with `-configuration Debug` and confirm `target/debug/`.
- `grep -rn "oled-monitor-protection/architecture.md" AGENTS.md ARCHITECTURE.md`: no live instruction still points at the stub.
- `grep -rn "\.ftl\|localized_messages\|\"messages\"" crates native api.md ARCHITECTURE.md README.md packaging`: no leftover reference to the removed catalog.

## Notes

- **Deferred on purpose: the calibration session machine.** `AmbientCalibrationController.swift` is about 70% pure and time-free, but [ADR 0008](../../../adr/0008-core-owned-ambient-follow-and-per-monitor-protection.md) line 26 says calibration is a shell-owned session, and ADR 0013 says the shell inserts points. Moving it needs a superseding ADR, new commands (`calibration_begin`, `calibration_slider`, `calibration_confirm`, `calibration_remove_point`, `calibration_end`, `calibration_recover`, `builtin_result`), an effects channel for built-in display writes, and a decision about who owns `calibration-session.json`. It is a feature-sized follow-up, not a chore. A smaller follow-up worth doing first is a dedicated point-edit command, because `saveCurve` sends the whole settings object (`CoreBridge.swift:200-202`) and can revert a concurrent unpublished settings change.
- **Deferred on purpose: the `HardwareKeyRouter` latch.** The keystroke latch answers synchronously inside a CGEvent tap, and [ADR 0004](../../../adr/0004-external-monitor-hardware-keys-and-fluid-controls.md) (line 15) says that tap never calls the core; [ADR 0005](../../../adr/0005-configurable-hardware-key-step.md) (line 45) says the router stays standalone. Only the derivations (feature `pending`/`tolerance`/`failed` at `MacDDCController.swift:185-192`) and step clamping versus the core's range rejection (`settings.rs:267-272`) are candidates, and they need an ADR first.
- **Stays in Swift: `chooseFrameRate` and `frameDuration`.** They take frame-rate range data from AVFoundation and configure the device; they are device configuration, not session policy. They move to `CameraFrameRate.swift` only to keep the lifecycle file ABI-bound and the frame-rate tests runnable without the Rust library.
- **Not verified on hardware.** `PresenceCamera.swift` has no automated test; the lifecycle wiring is covered only by the wrapper tests and the packaged-app smoke test. Do not claim camera acceptance in docs or the changelog.
- **Why Option B for the headers (no cbindgen).** The survey found that cbindgen cannot reproduce the hand-written headers (prose comments, `enum {}` constants, the `TV_CORE_ABI_VERSION` name, parameter names, ordering), would force Rust source edits made only for tooling (renaming the version constant, exposing private constants, dropping `repr(C)` on the handle), and adds a tool install to CI. The std-only test gives the same drift protection without those costs. An optional follow-up is a `llvm-nm` check of the built `.a` files (CI installs `llvm-tools-preview` only in the coverage step; Xcode's `nm` cannot read rustc's LLVM objects).
- **`os_locale` is now inert.** It is still accepted by `tv_create` for ABI compatibility. Removing it is a separate, additive-to-breaking decision for the next ABI change.
- **`packaging/macos/build-app.sh:8-9` defaults to `target/release/`** (no target triple) while every script, the CI and the docs use `target/aarch64-apple-darwin/release/`. It works only because callers pass the environment variables explicitly. Left unchanged here to keep the chore bounded; worth a one-line follow-up.
- **The Xcode project is not the verified build path.** It has no Rust build phase, no test target, does not bundle the ONNX Runtime, models or licenses and omits CoreGraphics from its frameworks (which `build-app.sh:46` links). This chore only fixes the Release library path and documents the prerequisites.
- **Archived-architecture drift** (planned `crates/telaviva-core/src/platform/*`, `build.rs`, `ddc_bridge.*`, Fluent, `cargo-deny`, the 2 s watchdog versus the implemented 5 s, and others) is already acknowledged by `ARCHITECTURE.md:106-110`; this chore only stops live instructions from pointing there.
- **Language rules:** documentation and commit messages are en-US; code comments are English. The repository has no `// I18N` rule in `AGENTS.md` or `ARCHITECTURE.md`, so none is added.
- **Do not commit.** This plan was written for `/codeadvisor-dev:implement --ff`; committing is a separate user decision.

## Implementation Status

All nine steps are done and verified (2026-10-17). Changes from the plan:

- [x] 1. Stale spec references repointed. `ARCHITECTURE.md:125` now links the archive README of the camera lifecycle record.
- [x] 2. Xcode Release paths fixed; `xcodebuild -showBuildSettings` confirms Debug at `target/debug/` and Release at `target/aarch64-apple-darwin/release/`.
- [x] 3. Header contract tests added for both crates. Header comment fixes were wider than planned: both create functions return `3`, and `tv_inference_create` also returns `-2` and `-4`. The `ARCHITECTURE.md` sentence about the tests sits after the "Implementation validation gates" list because there is no testing paragraph.
- [x] 4. `CameraLifecycle` ported to `camera_lifecycle.rs` with a local duplicate-key check (the core's helper is private). The `u64::MAX` token-wrap test is a unit test inside the module because the token is private.
- [x] 5. `TvCameraLifecycle` ABI added. `Send`/`Sync` is enforced by a compile-time assertion instead of `unsafe impl`, because `Mutex<CameraLifecycle>` already is both. No test reaches `TV_INTERNAL`.
- [x] 6. Swift wrapper is a `final class` that faults closed on any failed call. `darkSettleUsed` and the unused timeout parameters were dropped. `PresenceCamera.swift` needed no change.
- [x] 7. The `.ftl` catalogs were at the repository-root `locales/`, not under `crates/telaviva-core/`; both were removed. `MonitorPopup.swift` had 9 call sites, not 4. `os_locale` is kept behind `#[allow(dead_code)]`.
- [x] 8. ADRs 0014 and 0015 written; ADRs 0001 and 0003 carry one-line cross references; changelog and `api.md` updated. `ARCHITECTURE.md` localization text is at lines 20 and 115, not 113-114.
- [x] 9. Validation commands run: `cargo fmt --check`, 188 Rust tests (runtime test skipped as in CI), clippy with `-D warnings`, the 13 native test scripts, `scripts/build-macos.sh`, the packaged-app `--smoke-test`, `plutil -lint`, and the grep checks. Not run: `xcodebuild build` (the Xcode project is not the verified build path) and any camera hardware check.
