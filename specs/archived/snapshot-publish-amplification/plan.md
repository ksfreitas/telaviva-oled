# Chore: Snapshot publish amplification

## Chore Description

Deferred from [always-on-camera-resource-optimization](../always-on-camera-resource-optimization/plan.md) (task 5). Every accepted core command repeats work that only needs to happen once. Reduce it without changing behavior, the snapshot content or the C ABI.

Re-verified on branch `core-command-queue` on 2026-10-18 (after the macOS and Windows controls adapters, [ADR 0029](../../../adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md)):

- **Core (both platforms).** `ffi.rs` `tv_submit` builds a full snapshot (`snapshot_value()`, including a JSON tree) only to read `revision` for the snapshot-ready callback. `Core` has no revision accessor (the `revision` field is private), so that is 1 snapshot build per accepted submit with a callback registered. `snapshot_value` is `&self` and has no side effect, and `revision` changes only at the end of `submit_json` (one increment per accepted command), so an accessor returns exactly the value the snapshot would carry.
- **macOS shell.** `CoreBridge.submit` calls `refresh()` after an accepted submit while the snapshot-ready callback also calls `requestSnapshotRefresh()`, so one command can cause two snapshot reads (the `refreshAgain` coalescing bounds bursts, not the single-command double). The callback does not run once the core is stopped (`tv_submit` skips it after `tv_shutdown`), so during shutdown restoration the submit-side `refresh()` is the only trigger for a `hardware_result`; `pollShutdownState` also calls `core.refresh()` every 100 ms. `CoreSnapshot` is `Decodable` only, and `drainPublications` assigns the `@Published` snapshot on every refresh, even for an equal revision, so `MenuBarController`'s sink re-runs `camera.apply`, `controls.observe`, `ddc.process` and key routing each time. `CoreBridgeTests` covers refresh-burst coalescing but not "one command, one publish" or "identical refresh does not publish". (The controls handle ignores nothing here: `observe` accepts an equal revision, so a duplicate publish only costs work.)
- **Windows shell.** There is no submit-side `refresh()`: `CoreBridge::submit` only forwards to `tv_submit`, and `onSnapshotReady` coalesces wakes through `revisionPending` and records `latestRevision`. The worker (`runWorker`) fetches and decodes a snapshot on each wake and notifies the sinks without comparing the revision to the one it last published. Two submits that land before the worker reads cause a second wake whose snapshot is the same revision again, so that revision is published twice. `AppController::publish` fans each publish out to `ddc->onSnapshot`, `controls->observe`, the key and laptop logic and `latestSnapshot`.
- **Windows pages.** `KeyboardPage` already skips an unchanged revision. `GeneralPage` and `AutomaticBrightnessPage` do not and should not get the same guard: they run on a poll timer that reads `host_->latestSnapshot()` and also refreshes state that does not come from the snapshot (camera status, laptop availability and calibration view), so skipping on an unchanged revision would leave that state stale. They are left as they are.
- **Removed from this plan.** The first draft also listed `localized_messages()` re-parsing the FTL text on every snapshot and proposed a per-locale cache. That is obsolete: [ADR 0015](../../../adr/0015-native-shells-own-ui-strings.md) removed the FTL catalogs and the snapshot's `messages` field, and `localized_messages` no longer exists.

The core part applies to macOS and Windows in the same change, as the repository requires. The shell parts are per platform.

Status: implemented on branch `core-command-queue` and finalized on 2026-10-18 as [ADR 0031](../../../adr/0031-publish-each-snapshot-revision-once.md). The Windows part is not yet built or run (it needs a Windows machine); see Progress.

## Relevant Files

- `crates/telaviva-core/src/ffi.rs`: `tv_submit` (revision-only path), `tv_snapshot`.
- `crates/telaviva-core/src/core.rs`: `snapshot_value`, and a `revision()` accessor (new, `pub`).
- `native/macos/TelaViva/CoreBridge.swift`: `submit`, `refresh`, the snapshot-ready callback, `refreshAgain` coalescing, `publish`, `drainPublications` and the `@Published` assignment.
- `native/macos/TelaViva/MenuBarController.swift`: snapshot sink that fans out on every publish (read only; no change expected).
- `native/macos/Tests/CoreBridgeTests.swift`: ordering and revision semantics of the bridge.
- `native/windows/Platform/CoreBridge.cpp`: `onSnapshotReady`, `runWorker` and `snapshot()`.
- `native/windows/Platform/RevisionGate.h` (new): the pure "has this revision been published" rule, so it is testable without the core DLL.
- `native/windows/TelaViva/Host/AppController.cpp`: `publish` (read only; no change expected).
- `native/windows/Tests/`: `revision_gate.cpp` (new) for the gate.
- `crates/telaviva-core/tests/ffi_contract.rs`, `crates/telaviva-core/tests/core_contract.rs`: contract tests that must stay green, plus the new revision tests.

## Step by Step Tasks

### 1. Revision-only path in `tv_submit`

Obtain the revision without building a snapshot: add `Core::revision()` returning the field and use it in `tv_submit` instead of `snapshot_value()`. The callback payload, the revision value and the ABI (`ABI_VERSION = 1`, `include/telaviva_core.h`) must not change.

### 2. macOS: drop the duplicate refresh and skip unchanged publishes

In `CoreBridge.swift`:

- After an accepted submit, call `refresh()` only when the bridge is shutting down (`isShuttingDown`), because that is the one case where the core no longer calls the snapshot-ready callback. Otherwise the callback already schedules the refresh. Keep the `refreshAgain` coalescing.
- Skip a snapshot whose revision is not newer than the last one published: `publish` ignores `next <= current` of the pending publication, and `drainPublications` ignores `next <= previous`. The revision is the core's change counter (one increment per accepted command), so an equal revision carries the same state. A snapshot without a revision (the unavailable state) is always accepted.

### 3. Windows: skip an already published revision

In `CoreBridge.cpp` `runWorker`, skip the `tv_snapshot` fetch, the decode and the sink notification when the woken revision (`latestRevision`) is not newer than the last published one, and skip the sink notification when the fetched snapshot's revision is not newer than the last published one (the fetch can already contain a later command than the wake that triggered it). Put the comparison in a small header-only `RevisionGate` (`shouldFetch(woken)`, `shouldPublish(fetched)`, `markPublished(revision)`; the first revision always passes). `snapshot()` and `submit` are unchanged, so a snapshot read after `submit` returns still reflects that command. `GeneralPage` and `AutomaticBrightnessPage` keep their poll logic (see Chore Description).

### 4. Tests

- macOS, `CoreBridgeTests.swift`: one command yields exactly one publish; an identical refresh does not publish; a state change after `submit` is visible without an extra refresh.
- Core: two consecutive snapshots with no state change keep the same revision and identical JSON, and `Core::revision()` equals the snapshot's `revision` before and after a command; the `tv_submit` callback still receives the snapshot revision. `ffi_contract.rs` and `core_contract.rs` run unchanged.
- Windows native tests: `RevisionGate` passes the first revision, passes a newer one, rejects an equal or older one, and a wake with an unchanged revision does not fetch. The worker loop itself needs the core DLL and stays covered by `core_bridge_smoke.cpp`.

### 5. Run Validation Commands

## Validation Commands

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
tools/tests/test-core-bridge.sh
tools/tests/test-controls-bridge.sh
tools/tests/test-camera-lifecycle.sh
scripts/build-macos.sh
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\build-rust-dlls.ps1
$env:TELAVIVA_TEST_FIXTURES = (Resolve-Path native\windows\Tests\fixtures).Path; & native\windows\build\x64\Release\tests.exe
```

The Windows solution must also build with 0 errors (MSBuild on `native\windows\TelaViva.sln`, Release x64).

## Notes

- Measure first if possible: count snapshot builds and publishes per presence report before and after (a debug counter or `os_signpost`; on Windows a counter in the worker), so the saving is a number and not an estimate. This change did not measure: the saving is stated by construction (one snapshot build less per accepted submit; one read less per single-command macOS submit; equal revisions not republished).
- The inference ABI has no version check; do not add one here.
- Function names are used instead of line numbers on purpose, because the files change often.

## Progress

Status 2026-10-18, on branch `core-command-queue`:

- [x] 1. `Core::revision()` added; `tv_submit` reads it instead of building a snapshot. Tests `revision_matches_snapshot_revision` and `consecutive_snapshots_without_a_change_are_identical` in `core_contract.rs`; `ffi_contract.rs` already asserts the callback revision equals the snapshot revision and stays unchanged. `cargo fmt`, `clippy -D warnings` and the workspace tests pass.
- [x] 2. macOS `CoreBridge`: `submit` and `submitJSON` refresh after an accepted command only while shutting down; `publish` and `drainPublications` skip a revision that is not newer than the published one. Tests `oneCommandPublishesOnce`, `identicalRefreshDoesNotPublish` and `stateChangeAfterSubmitIsVisibleWithoutExtraRefresh` in `CoreBridgeTests.swift`; with the old rules the first one fails, so it discriminates. All 16 `tools/tests/test-*.sh` pass on a Mac.
- [x] 3. Windows `CoreBridge::runWorker` skips the fetch and the sink notification for a revision already published, through the header-only `RevisionGate` (`Platform/RevisionGate.h`); tests in `Tests/revision_gate.cpp`. **Not built and not run:** this machine is a Mac. Build the solution and run `tests.exe` on Windows before relying on it.
- Pages: `GeneralPage` and `AutomaticBrightnessPage` keep their poll logic on purpose (see Chore Description).
- Not measured: no counter or signpost was added, so the saving is by construction, not a number.
- Open: the Windows build and test run; the macOS packaging (`scripts/build-macos.sh`) was not run because it downloads the ONNX Runtime.
