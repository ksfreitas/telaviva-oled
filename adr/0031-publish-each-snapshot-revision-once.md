# ADR-0031: Publish each snapshot revision once

**Status:** Accepted for the core and macOS; the Windows change is written and reviewed but not yet built or run (it needs a Windows machine)
**Date:** 2026-10-18
**Feature:** snapshot-publish-amplification
**Spec:** [Archived chore plan](../specs/archived/snapshot-publish-amplification/plan.md)

## Context

Every accepted core command repeated work that only needs to happen once:

- `tv_submit` built a full snapshot (`snapshot_value()`, a JSON tree) only to read the `revision` it passes to the snapshot-ready callback. `Core` had no revision accessor.
- The macOS `CoreBridge.submit` called `refresh()` after an accepted submit while the snapshot-ready callback also scheduled a refresh, so one command could cause two snapshot reads. The bridge also assigned the `@Published` snapshot on every refresh, even for a revision it had already published, so the menu-bar sink re-ran camera, controls, DDC and key routing each time.
- The Windows bridge worker fetched and decoded a snapshot on each wake and notified its sinks without comparing the revision to the last one it published. Two submits that landed before the worker read produced a second wake for a revision that was already published.

The snapshot content and the C ABI (`ABI_VERSION = 1`) had to stay unchanged, and a snapshot read after `submit` returns had to keep reflecting that command.

## Decision

- **Core:** add `Core::revision()` and use it in `tv_submit`. `snapshot_value` is pure and `revision` changes only at the end of `submit_json` (one increment per accepted command), so the accessor returns exactly the value the snapshot would carry. The callback payload is unchanged.
- **macOS:** the revision is the core's change counter, so a snapshot whose revision is not newer than the last published one carries the same state and is skipped (`publish` and `drainPublications` compare with `<=`; a snapshot without a revision, the unavailable state, is still accepted). After an accepted submit the bridge calls `refresh()` only while it is shutting down.
- **Windows:** `runWorker` skips the fetch and the sink notification for a revision that was already published, through a small header-only `RevisionGate` (`shouldFetch` for the woken revision, `shouldPublish` for the fetched one, `markPublished`; the first revision always passes). `snapshot()` and `submit` are unchanged.
- **Windows pages stay as they are.** `KeyboardPage` already skips an unchanged revision. `GeneralPage` and `AutomaticBrightnessPage` run on a poll timer and also refresh state that does not come from the snapshot (camera status, laptop availability, calibration view), so a revision guard would leave that state stale.

## Alternatives Considered

- **Make `CoreSnapshot` `Equatable` and compare content.** Rejected for the revision comparison: it is cheaper and the revision already means "state changed".
- **Remove the macOS submit-side `refresh()` altogether.** Rejected: the callback is skipped once the core is stopped (`tv_submit` after `tv_shutdown`), so during shutdown restoration the submit-side refresh is the only trigger for a `hardware_result`.
- **A revision guard on every Windows page.** Rejected for the two poll-driven pages (above).
- **A per-locale cache for `localized_messages()`.** Obsolete: [ADR 0015](0015-native-shells-own-ui-strings.md) removed the FTL catalogs and the snapshot's `messages` field.

## Consequences

- One snapshot build less per accepted submit with a callback registered; one snapshot read less per single-command macOS submit; an already published revision is not republished on either shell.
- A revision is now published at most once, so anything that must refresh the UI without a revision change needs another trigger. The shells already use timers and notifications for that.
- The saving is justified by construction and was **not measured**: no counter or signpost was added.
- The Windows part is **unverified**: it was written on a Mac and reviewed by reading. Build `native\windows\TelaViva.sln` (Release x64) and run `tests.exe` (`revision_gate.cpp` covers the gate; the worker loop needs the core DLL and stays covered by `core_bridge_smoke.cpp`) before relying on it.
- The macOS packaging (`scripts/build-macos.sh`, which downloads the ONNX Runtime) was not run for this change; the Swift scripts and the Rust workspace checks pass.

## Non-Obvious Constraints

- The snapshot-ready callback does not run after `tv_shutdown`; shutdown restoration depends on the shell refreshing by itself.
- `observe` in the controls handle accepts an equal revision ([ADR 0029](0029-core-owned-manual-control-queue-and-ddc-dispatch.md)), so a duplicate publish only cost work, not correctness; skipping it is safe.
- The inference ABI has no version check; this change did not add one.
