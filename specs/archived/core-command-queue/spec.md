# Feature Specification: Shared Manual-Control Command Queue in the Rust Core

**Feature Branch**: `core-command-queue`
**Input**: Owner request to evaluate moving the background command queue for the monitor controls into the Rust core, to stop duplicating fixes and tests across macOS and Windows.

## Context and Understanding

The owner fixed queueing bugs on macOS (sliders and hardware keys), then had to fix the same bugs again on Windows. The cause is structural: the logic that turns user input into DDC writes (draft values, latest-wins coalescing, one write in flight, rebasing after external changes, mute memory, key step arithmetic) is written twice, once in Swift and once in C++, and each copy has its own test suite. The question is whether that queueing belongs in the Rust core.

What discovery found (from reading the code; findings from the exploration pass still need a confirming read during design):

**The logic is a near line-by-line port, not two designs.**

| Concern | macOS | Windows |
| --- | --- | --- |
| Slider draft, coalescing, one-in-flight | `ManualMonitorControls.swift` | `Platform/ManualControls.cpp` |
| Key step, latch, intent coalescing, rebase, mute | `HardwareKeyRouter.swift` (`HardwareKeyPolicy`, `HardwareKeySequence`, router) | `Platform/KeyIntentRouter.cpp`, `KeyRoute.h` |
| DDC transaction framing, retry, pacing | `DDCTransaction.swift` | `DdcTransaction.cpp`, `DdcTimingProfile.cpp` |
| Constants shared by both | step 5 (1 to 15), fine step 1, rebase settle 1000 ms, outstanding expiry 10000 ms, 128-entry bounds | same |

**The copies have already drifted** (each is a candidate for the "which behavior is canonical" decision):

- Slider coalescing: macOS drains on the next main-queue turn with no timer. Windows arms a 20 ms drain timer.
- Releasing a stuck in-flight slot: Windows has a 3 s safety net. No equivalent was found on macOS.
- Readback match: Windows accepts a 0.51 tolerance. macOS requires the observed raw value to equal the raw target.
- A rejected `hardware_result` submission: Windows re-offers the action on the next snapshot. macOS has no equivalent.
- Windows fixed a held volume key bouncing back (`073be42`) and a manual command that was never released on completion (`01bd988`). The owner reports the same classes of bug on macOS.

**There are already three queue layers**, which is part of why bugs recur: (1) UI drafts and per-monitor coalescing in the shell, (2) the key router's per-key intents in the shell, and (3) the core's own bounded `pending_actions` queue. The third already does latest-wins coalescing of non-head entries and generation tagging, but has no notion of "dispatched", so the shell works around that.

**What the core is today**: a synchronous, sans-IO state machine. It owns no threads, timers or clock. It has no DDC port: the shell pulls `pending_actions` from snapshots and pushes `hardware_result` back. ADR 0001 gives "serial DDC execution" to the native adapters. ADR 0004 and ADR 0008 put draft coalescing and debounce in the shell. ADR 0021 (Windows) and ADR 0004 (macOS) say the key hook or event tap never calls the core.

**There is a direct precedent.** ADR 0014 moved the camera session timing (a Swift state machine that already took time as a parameter) into the core behind an opaque handle (`tv_camera_lifecycle_*`). The shell supplies `now`, keeps its timers and device code, and the tests moved to a fake-clock Rust contract test. ADR 0014 lists the `HardwareKeyRouter` latch as a deferred follow-up that needs a new record first. It also rejects routing high-rate events through `tv_submit` because each accepted command bumps the revision and republishes the snapshot (see `specs/archived/snapshot-publish-amplification/plan.md`).

**Short answer to the owner's question**: yes for the policy and state machine, no for the background execution. The decisions (what the draft is, what to send next, when a write is in flight, when to rebase, what the key step does) are pure logic and should live once in Rust. The mechanics (the thread that runs DDC, the timers, the key hook or event tap, the DDC transport I/O) are operating-system code and stay in each shell.

## Feature Description

Introduce a core-owned, clock-injected state machine for manual monitor controls (slider and hardware-key input for brightness and volume). The shells become thin adapters:

- They forward events to it: a slider change, a decoded key press, a hardware completion, a snapshot observation, and "now".
- They execute what it returns: values to show as the draft, actions to dispatch, and the next deadline to arm a timer for.

The behavior is identical on macOS and Windows by construction, and the queueing rules are tested once.

Value to the owner: a queueing bug is fixed once, in one place, with one test; new platform features do not re-port the logic; the two UIs cannot drift apart.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As the owner, I fix a slider or key queueing bug once in Rust and both platforms get the fix, so I stop porting fixes between Swift and C++.
- **US-002**: As the owner, I test coalescing, in-flight tracking, rebase and mute behavior once in the core's contract tests with a fake clock, so the shells only need small adapter tests.
- **US-003**: As a user, dragging a slider or holding a volume or brightness key behaves the same on macOS and Windows: the control follows my input immediately, only the latest target is written, and the value does not jump back.
- **US-004**: As a user, when the monitor sleeps, disconnects, changes connection generation or the app shuts down, pending manual work is dropped and never replayed.
- **US-005**: As a maintainer, adding the third behavior (a new key, a new feature such as contrast) means adding it in the core only.

### Functional Requirements

- **FR-001**: The system MUST hold the manual-control state (draft, queued target, in-flight tracking, rebase state, mute memory, key step policy) in the Rust core, not in the Swift or C++ shells.
- **FR-002**: The core MUST take time only as an injected monotonic `now`, read no clock, and own no threads or timers (consistent with ADR 0014).
- **FR-003**: The core MUST report the next deadline it needs (the coalescing delay, the DDC settle, backoff and resubmission waits, and the wake grace), and the shell MUST arm its own timer for it. The in-flight safety release, the rebase settle window and the outstanding-write expiry are not reported deadlines: they are evaluated on the next observe or key event.
- **FR-004**: Slider input MUST show an immediate draft, MUST send only the latest target per (monitor, feature), and MUST allow at most one write in flight per (monitor, feature).
- **FR-005**: Key input (already decoded and latched by the shell) MUST produce intents that share the slider's queue, with held-key repeats accumulating into the pending target and not bouncing back on a lagging readback.
- **FR-006**: Rebase against external changes, the settle window, outstanding-write tracking, mute memory, step clamping and fine-step behavior MUST behave identically on both platforms, with one set of constants defined once. Status: met in the Rust code; the 0.51 tolerance (`manual::READBACK_TOLERANCE`) and the default step (`keys::KEY_DEFAULT_STEP`) each have one definition that the other modules reuse.
- **FR-007**: Sleep, wake, disconnect, connection-generation change and shutdown MUST drop pending and queued manual work with no replay after wake (a core action that is already listed in pending_actions is held through sleep and runs after the wake grace).
- **FR-008**: The new events MUST NOT go through `tv_submit` per slider tick or key repeat, so they do not bump the snapshot revision or amplify snapshot publishing.
- **FR-009**: The shell MUST keep the operating-system side: the key hook and event tap, the swallow-or-pass-through decision on the hook thread, the single DDC attempt (frame, write, read, and its I/O errors), the worker thread or queue that runs DDC calls, the timers, and the UI binding. The retry decision and backoff timing are not in this list; see FR-015.
- **FR-015**: The core MUST own the DDC retry policy (how many attempts, which errors are permanent, the backoff schedule, the pacing profile) as part of the same state machine, driven by injected `now`. The shell executes one attempt at a time on request and reports its outcome. The system MUST define one retry budget that covers both per-operation attempts and the core's restore retries (ADR 0006), and an ADR MUST record how that budget relates to the "three total attempts" rule in the archived `contracts.md`. See Decisions, item 1. Status: recorded in [ADR 0029](../../../adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md); ADRs 0004, 0005 and 0008 are amended when the macOS adapter lands.
- **FR-010**: The change MUST be additive to C ABI version 1 and covered by the header contract test, unless the design shows it cannot be.
- **FR-011**: The change MUST ship for macOS and Windows in the same change with localized strings unchanged and tests on each (project rule), unless the owner scopes it to one platform.

  Status 2026-10-18: Windows done (the replaced files and their tests are removed); macOS pending (Decision 8).
- **FR-012**: A decision record (ADR) MUST supersede or amend the parts of ADR 0001, 0004, 0005, 0008 and 0021 that place coalescing and the key router in the shells. Status: recorded in [ADR 0029](../../../adr/0029-core-owned-manual-control-queue-and-ddc-dispatch.md); ADRs 0004, 0005 and 0008 are amended when the macOS adapter lands.
- **FR-013**: Where the two shells currently differ, the system MUST adopt one canonical behavior per difference, recorded as an explicit decision. See Clarification Needed 2.
- **FR-014**: The existing observable behavior covered by today's tests (the 34 Windows `manual_controls` cases, the 34 key router cases and the 5 tracker cases, the key router cases on both platforms, and the macOS `ManualMonitorControls` cases) MUST be preserved or intentionally changed with a recorded reason, and ported to the Rust contract tests before the shell copies are deleted.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The slider and key queueing policy exists in one implementation. The Swift and C++ copies are reduced to adapters, with no coalescing, rebase or step arithmetic left in either shell.

  Status 2026-10-18: Windows done (the replaced files and their tests are removed); macOS pending (Decision 8).
- **SC-002**: A scripted scenario (drag, hold key, external change, sleep and wake, disconnect) produces the same sequence of dispatched writes and the same draft values on both platforms, asserted by one shared set of core tests.
- **SC-003**: A queueing bug reproduced as a core test is fixed once and no platform-specific code change is needed.
- **SC-004**: No regression in input latency: the hook-thread swallow decision stays lock-free in the shell, and slider and key drafts update without waiting on a DDC round trip.
- **SC-005**: No increase in idle CPU, memory or wakeups (the core owns no timers; shells arm at most one timer per pending deadline), and no extra snapshot publications per slider tick or key repeat.
- **SC-006**: The duplicated shell test suites for the moved logic shrink to adapter-level tests, and the removed cases each map to a core test.

## Decisions

Recorded from the owner's answers (2026-10-18):

1. **Scope: B (confirmed).** The core takes the manual-control state machine and also the DDC retry and pacing policy. The owner confirmed this after the ADR 0006 explanation (see Notes, "The retry-budget tension"), accepting the single retry budget and the hardware verification it requires. FR-015 stays.
2. **Canonical behavior: A.** Where the shells differ, adopt the Windows behavior: 20 ms coalescing delay, 3 s in-flight safety release, 0.51 readback tolerance, re-offer of a rejected `hardware_result`. Each difference gets a core test. Refined by architecture Decision 7: a rejected `hardware_result` is resubmitted at most three times and the write is never re-run; the 3 s safety release is snapshot-driven (evaluated when a snapshot is observed), as in the Windows original.
3. **Interface: A.** A new opaque handle in the style of `tv_camera_lifecycle_*`. The shell supplies `now` and receives the draft values, the actions to dispatch and the next deadline. Events do not go through `tv_submit`.
4. **`pending_actions`: A.** The core's own queue stays as the channel to the executor in the first cut. The new machine decides when to send.
5. **Key latch: A.** The per-key swallow-or-pass-through latch stays in the shell, lock-free on the hook thread. Step, intent, rebase and mute policy move to the core. Exception: on macOS the event tap calls the handle once, synchronously, for the consume decision (architecture Decision 4; ADR 0004 needs an amendment). On Windows the swallow decision stays on the hook thread and the returned `consume` is only traced.
6. **Sequencing: A.** Finish and commit `inference-device-selection` first, then start this feature.
7. **Branch: A, refined.** No branch is created during discovery or design. The branch `core-command-queue` is created at implementation time, after item 6 is done.
8. **macOS adapter: deferred (confirmed 2026-10-18).** The owner will implement the macOS adapter on a Mac; the core handle and the Windows adapter are done first. FR-011 (same change on both platforms) is therefore not met yet and the feature must not be released before the macOS adapter lands, unless the owner scopes it otherwise.

## Resolved questions (original wording)

1. **Scope of what moves.** The queue spans three layers plus DDC transport. Which parts move in this feature?
2. **Canonical behavior where the shells differ** (coalescing delay, safety release, readback tolerance, rejected-result re-offer).
3. **Where the new state machine lives and how shells talk to it**: a new opaque handle like the camera lifecycle, or extend `Core` and `tv_submit`.
4. **Fate of the core's own `pending_actions` queue** once the manual state machine owns dispatching: keep as is, or fold manual actions into the new machine.
5. **Key router boundary.** Should the keystroke latch (the per-key swallow decision) stay in the shell, with only intent and rebase policy moving? ADR 0014 left this open and Windows needs a lock-free hook-thread decision.
6. **Sequencing against in-flight work.** The working tree has uncommitted changes to `core.rs`, `settings.rs` and Windows `Platform/` files (branch `inference-device-selection`). This feature touches the same core and platform files.
7. **Branch.** The skill wants a branch named `core-command-queue`, but creating it now would carry those uncommitted changes onto it.

## Notes

- Exploration gaps: no test exists for `MacDDCController` (process, perform, sleep handling), for the Windows `ThreadExecutor`, or for the Windows protection trailing timer. Moving logic into the core is a chance to close part of that gap, but those adapters stay in the shells.
- The macOS git history has no commit with "fix" in the subject for queueing or keys, so the owner's macOS fixes may be uncommitted or under other subjects. Worth confirming which bugs motivated this, to turn each into a core regression test.
- **The retry-budget tension (ADR 0006).** Retries exist at two layers today. The native layer retries one DDC operation up to 3 attempts, sleeping 200 ms then 1000 ms between them (about 1.7 s per failed read). The core, per ADR 0006, retries the wake restore up to `MAX_RESTORE_FAILURES` = 3 times after a failed readback or restore write, and every core retry is a new operation with its own 3 native attempts. So one stuck monitor can cost up to 4 failed readbacks (about 7 s) and 12 attempts, while the archived `contracts.md` says "at most three total attempts". ADR 0006 admits a strict reading forbids this and leaves `contracts.md` unchanged. With scope B both layers land in the same module, so the design must pick one budget and amend or supersede `contracts.md`. Moving the retry also removes the blocking `sleep` from the single DDC worker (Windows runs one worker for all monitors, so a failing monitor delays the others today); the core would instead return "retry at T" and the shell would arm a timer. That is a behavior change to verify on hardware.
- `specs/archived/snapshot-publish-amplification/plan.md` is related: the core's per-accepted-command snapshot build is the reason a separate handle is preferred over `tv_submit` for high-rate events.
- Project rules that apply: everything lands on both platforms together, the update check stays the only network use, and intended behavior is kept distinct from hardware-verified behavior. Hardware verification of the moved behavior on real monitors is still required and cannot be inferred from the tests.

