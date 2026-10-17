# Chore: Snapshot publish amplification

## Chore Description

Deferred from [always-on-camera-resource-optimization](archived/always-on-camera-resource-optimization/plan.md) (task 5). Every presence report amplifies into about 3 snapshot builds, 5 JSON serializations, 2 decodes and 2 main-thread publications (`ffi.rs` `tv_submit` builds a full snapshot only to read the revision; `CoreBridge.submit` then calls `refresh()` while the snapshot-ready callback also requests one; `localized_messages()` re-parses the FTL text on every snapshot; `CoreSnapshot` is not `Equatable`, so every publish fires `MenuBarController`'s sink and re-runs `camera.apply`, `manualControls.apply`, `ddc.process` and key routing). This is small per event, but it runs on the main thread about once per 1 to 10 seconds, and once per second while a monitor is dimmed. Reduce it without changing behavior, the snapshot content or the C ABI.

Status: unblocked and not started. The `macbook-brightness-follow` work is merged into main (archived at [macbook-brightness-follow](archived/macbook-brightness-follow/spec.md)); the snapshot already carries the ambient, follow and protection fields, so no rebase onto that branch is needed.

## Relevant Files

- `crates/telaviva-core/src/ffi.rs`: `tv_submit` (revision-only path), `tv_snapshot`.
- `crates/telaviva-core/src/core.rs`: `snapshot_value`, `localized_messages`, a `revision()` accessor if none exists.
- `native/macos/TelaViva/CoreBridge.swift`: `submit`, `refresh`, snapshot-ready callback, `refreshAgain` coalescing, `@Published` assignment.
- `native/macos/TelaViva/MenuBarController.swift`: snapshot sink that fans out on every publish.
- `native/macos/Tests/CoreBridgeTests.swift`: ordering and revision semantics of the bridge.
- `crates/telaviva-core/tests/ffi_contract.rs`, `crates/telaviva-core/tests/core_contract.rs`: contract tests that must stay green.

## Step by Step Tasks

### 1. Revision-only path in `tv_submit`

Obtain the revision without building and serializing a full snapshot (read the counter from `Core`, add a small `Core::revision()` accessor if needed). The callback payload, the revision value and the ABI (`ABI_VERSION = 1`, `include/telaviva_core.h`) must not change.

### 2. Cache localized messages

Cache the parsed FTL messages per locale so `localized_messages()` stops re-parsing on every snapshot. Output must be identical.

### 3. Drop the duplicate refresh and skip unchanged publishes

In `CoreBridge.swift`, remove the redundant `refresh()` after `submit` when the callback already schedules one, keeping the `refreshAgain` coalescing and the post-condition that a snapshot read after `submit` returns reflects that command. Make `CoreSnapshot` `Equatable` (or compare the revision) and skip the `@Published` assignment when nothing changed.

### 4. Tests

Extend `CoreBridgeTests.swift`: one command yields exactly one publish; an identical refresh does not publish; a state change after `submit` is visible immediately. Add a core test that two consecutive snapshots with no state change keep the same revision and identical JSON. `ffi_contract.rs` and `core_contract.rs` run unchanged.

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

## Notes

- Measure first if possible: count snapshot builds and publishes per presence report before and after (a debug counter or `os_signpost`), so the saving is a number and not an estimate.
- The inference ABI has no version check; do not add one here.
