# Chore: Snapshot publish amplification

## Chore Description

Deferred from [always-on-camera-resource-optimization](archived/always-on-camera-resource-optimization/plan.md) (task 5). Every accepted core command repeats work that only needs to happen once. Reduce it without changing behavior, the snapshot content or the C ABI.

Re-verified against `main` on 2026-10-18:

- **Core (both platforms).** `ffi.rs` `tv_submit` builds a full snapshot (`snapshot_value()`, including one `serde_json::to_vec`) only to read `revision` for the snapshot-ready callback, and `Core` has no revision accessor (the `revision` field is private). That is 1 snapshot build and 1 serialization per accepted submit with a callback registered, not the "about 3 and 5" the first draft counted.
- **macOS shell.** `CoreBridge.submit` calls `refresh()` after an accepted submit while the snapshot-ready callback also calls `requestSnapshotRefresh()`, so one command can cause two snapshot reads (the `refreshAgain` coalescing bounds bursts, not the single-command double). `CoreSnapshot` is `Decodable` only, so `drainPublications` assigns the `@Published` snapshot on every refresh, even for an equal revision, and `MenuBarController`'s sink then re-runs `camera.apply`, `manualControls.apply`, `ddc.process` and key routing each time. `CoreBridgeTests` covers refresh-burst coalescing but not "one command, one publish" or "identical refresh does not publish".
- **Windows shell.** There is no submit-side `refresh()`: `CoreBridge::submit` only forwards to `tv_submit`, and `onSnapshotReady` coalesces wakes through `revisionPending`. The worker still fetches and decodes a snapshot on each wake and `AppController` publishes it to its sinks without comparing the revision to the one it last published. `KeyboardPage` already skips an unchanged revision; whether `GeneralPage` and `AutomaticBrightnessPage` do is not confirmed.
- **Removed from this plan.** The first draft also listed `localized_messages()` re-parsing the FTL text on every snapshot and proposed a per-locale cache. That is obsolete: [ADR 0015](../adr/0015-native-shells-own-ui-strings.md) removed the FTL catalogs and the snapshot's `messages` field, and `localized_messages` no longer exists.

The core part applies to macOS and Windows in the same change, as the repository requires. The shell parts are per platform.

Status: unblocked and not started. The `macbook-brightness-follow` work is merged into main (archived at [macbook-brightness-follow](archived/macbook-brightness-follow/spec.md)). Not implemented, so not finalized or archived.

## Relevant Files

- `crates/telaviva-core/src/ffi.rs`: `tv_submit` (revision-only path), `tv_snapshot`.
- `crates/telaviva-core/src/core.rs`: `snapshot_value`, and a `revision()` accessor (new, `pub(crate)` or `pub`).
- `native/macos/TelaViva/CoreBridge.swift`: `submit`, `refresh`, the snapshot-ready callback, `refreshAgain` coalescing, `drainPublications` and the `@Published` assignment.
- `native/macos/TelaViva/MenuBarController.swift`: snapshot sink that fans out on every publish.
- `native/macos/Tests/CoreBridgeTests.swift`: ordering and revision semantics of the bridge.
- `native/windows/Platform/CoreBridge.cpp`: `onSnapshotReady`, the worker loop and `snapshot()`.
- `native/windows/TelaViva/Host/AppController.cpp`: `publish`, which stores `latestSnapshot` and notifies the sinks.
- `native/windows/TelaViva/GeneralPage.xaml.cpp`, `AutomaticBrightnessPage.xaml.cpp`, `KeyboardPage.xaml.cpp`: per-page revision guards (read to see which pages lack one).
- `native/windows/Tests/`: native tests for the bridge or host, where a test for the unchanged-revision skip belongs.
- `crates/telaviva-core/tests/ffi_contract.rs`, `crates/telaviva-core/tests/core_contract.rs`: contract tests that must stay green.

## Step by Step Tasks

### 1. Revision-only path in `tv_submit`

Obtain the revision without building and serializing a full snapshot: add a small `Core::revision()` accessor and use it in `tv_submit`. The callback payload, the revision value and the ABI (`ABI_VERSION = 1`, `include/telaviva_core.h`) must not change.

### 2. macOS: drop the duplicate refresh and skip unchanged publishes

In `CoreBridge.swift`, remove the redundant `refresh()` after `submit` when the callback already schedules one, keeping the `refreshAgain` coalescing and the post-condition that a snapshot read after `submit` returns reflects that command. Make `CoreSnapshot` `Equatable` (or compare the revision) and skip the `@Published` assignment when nothing changed.

### 3. Windows: skip an unchanged revision before decoding and publishing

Read the worker loop in `CoreBridge.cpp` and `publish` in `AppController.cpp`, then skip the `tv_snapshot` fetch, the decode and the sink notification when the woken revision equals the last published one, keeping the guarantee that a snapshot read after `submit` returns reflects that command. Decide per page, after reading them, whether `GeneralPage` and `AutomaticBrightnessPage` need the same guard that `KeyboardPage` has; change only what the measurement shows.

### 4. Tests

- macOS, `CoreBridgeTests.swift`: one command yields exactly one publish; an identical refresh does not publish; a state change after `submit` is visible immediately.
- Core: two consecutive snapshots with no state change keep the same revision and identical JSON, and `Core::revision()` equals the snapshot's `revision`. `ffi_contract.rs` and `core_contract.rs` run unchanged.
- Windows native tests: a wake with an unchanged revision does not notify the sinks, and a wake with a new revision does.

### 5. Run Validation Commands

## Validation Commands

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
tools/tests/test-core-bridge.sh
tools/tests/test-camera-lifecycle.sh
scripts/build-macos.sh
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\build-rust-dlls.ps1
$env:TELAVIVA_TEST_FIXTURES = (Resolve-Path native\windows\Tests\fixtures).Path; & native\windows\build\x64\Release\tests.exe
```

The Windows solution must also build with 0 errors (MSBuild on `native\windows\TelaViva.sln`, Release x64).

## Notes

- Measure first if possible: count snapshot builds and publishes per presence report before and after (a debug counter or `os_signpost`; on Windows a counter in the worker), so the saving is a number and not an estimate.
- The inference ABI has no version check; do not add one here.
- Function names are used instead of line numbers on purpose, because the files change often.
