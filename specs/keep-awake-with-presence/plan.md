# Tasks: Keep the Computer Awake While Presence Is Detected

**Branch**: `feat/keep-awake-with-presence` · **Specs**: [spec.md](spec.md) · **Architecture**: [architecture.md](architecture.md) · **Contracts**: [contracts.md](contracts.md) · **Status**: Shared core and macOS implemented and automatically verified; physical acceptance pending; Windows deferred

Estimates describe engineering time, not guaranteed wall time. Each task is scoped to roughly 15–30 minutes; physical acceptance depends on available hosts. Tests are explicitly required by SC-006 and precede the corresponding implementation. `--ff` authorizes all phases without review stops.

**Owner scope update (2026-10-26)**: Complete shared core and macOS only. T010–T012, T015–T016 and Windows execution in T020–T021 are explicitly deferred, not failed or completed. Early Windows edits were removed; `native/windows/` remains unchanged. See [windows-handoff.md](windows-handoff.md).

## Phase 1: Setup (~1h)

- [x] T001 Resolve discovery decisions in `spec.md`, inspect `AGENTS.md`, and create the feature branch.
- [x] T002 Record native API findings, component boundaries and failure behavior in `architecture.md` and `contracts.md`.

**Notes**: Owner chose C1=A, C2=A, C3=B. No application code changed during discovery/design. No new dependency or network request is needed.

## Phase 2: Foundational (~2–3h)

Blocks integration; native adapters may be developed concurrently against the documented contract.

- [x] T003 Add failing Rust tests in `crates/telaviva-core/tests/keep_awake_presence.rs` for default/migration, strict session command, evidence expiry and generation/token invalidation.
- [x] T004 Add the Boolean and monotonic evidence policy in `crates/telaviva-core/src/settings.rs`, `keep_awake.rs` and `core.rs`; expose the additive snapshot and session command.
- [x] T005 Add failing policy tests for monitoring without dimming, battery/threshold bypass, retained capture, first-absent release and brightness two-miss independence in the core tests.
- [x] T006 Update core acquisition/retry/capture-demand paths without changing brightness eligibility; run the foundational Rust tests.

**Checkpoint**: Core contract ready and tested.

**Notes**: Added the pure monotonic evidence module and 9 integration cases, including a regression preserving brightness-only camera state when keep-awake is off. The completed workspace passes fmt, Clippy and 568 tests, including CPU inference and version consistency.

## Phase 3: Native Safety and Keep-Awake (US-001, US-002, P1) (~5–7h)

**Goal**: Both shells own temporary requests and release on first absence, unknown state, expiry or explicit transition. **Independent Test**: Fake OS backend proves no post-expiry/manual-transition activity and no same-token renewal; actual OS acceptance is recorded separately.

- [x] T007 [US-001] [US-002] Add failing macOS lease/backend tests in `native/macos/Tests/KeepAwakeTests.swift` and runner `tools/tests/test-keep-awake.sh`.
- [x] T008 [US-001] [US-002] Implement `native/macos/TelaViva/KeepAwakeController.swift` with assertion ownership, permission-aware activity backend, local guards, independent deadline and idempotent shutdown.
- [x] T009 [US-002] Wire workspace/session/lid/screensaver/display guards and snapshot handling in `native/macos/TelaViva/MenuBarController.swift`; verify fresh evidence after resume.
- [ ] T010 [US-001] [US-002] Add failing Windows fake-backend tests in `native/windows/Tests/keep_awake.cpp` for equivalent lease, cleanup and guard cases.
- [ ] T011 [US-001] [US-002] Implement `native/windows/Platform/KeepAwakeController.h/.cpp` with dedicated request-owning thread, monotonic watchdog and policy-aware pulse backend; register project/test units.
- [ ] T012 [US-002] Wire `native/windows/TelaViva/Host/AppController.cpp` to session/power/lid/saver events and snapshot reconciliation. Enable saver observation for either consumer.

**Checkpoint**: macOS native ownership and safety tests pass. OS pulse efficacy remains a physical acceptance gate.

**Notes**: macOS owns separate activity and exact-expiry timers, with no recurring timer while disabled. Review added zero-token absence, generation fencing/resume, queue-delay and live-safety checks. Native Windows T010–T012 are deferred by the owner.

## Phase 4: Settings, Independent Monitoring and Restore (US-003, US-004, P1) (~3–4h)

**Goal**: Save the option on both platforms with clear localized status and preserve other preferences. **Independent Test**: Round-trip full settings and enable/disable without mutating battery/camera/dimming choices.

- [x] T013 [US-003] [US-004] Extend macOS codec tests in `native/macos/Tests/CoreBridgeTests.swift` and localization tests before adding the field/snapshot to `CoreBridge.swift`.
- [x] T014 [US-003] [US-004] Add General toggle, monitoring explanation and permission/status UI in `SettingsView.swift` and `Localization.swift`; wire explicit permission action and safe current state.
- [ ] T015 [US-003] [US-004] Extend Windows `Tests/settings_payload.cpp` and codec tests for the new field and missing-object fallback; then update `Platform/SettingsModel.*` and `JsonCodec.*`.
- [ ] T016 [US-003] [US-004] Add General toggle/status/help in Windows settings XAML, host interfaces and both resource dictionaries; preserve full payloads and preference values.
- [x] T017 [US-004] Add disable/quit/restart and coexistence regression cases to the macOS native tests; fix lifecycle integration until they pass. Windows counterpart deferred.

**Checkpoint**: macOS feature connected; saved versus effective state is explicit.

**Notes**: The General option, permission actions and statuses are localized in both languages. Codec tests cover migration and delayed first lease delivery; the separate new localization table has explicit parity tests. Windows T015–T016 are deferred.

## Phase 5: Validation and Documentation (~3–5h plus hardware availability)

- [x] T018 Run `cargo fmt --all -- --check`, workspace Clippy and Rust tests; fix failures caused by this feature.
- [x] T019 Run the new macOS native tests, related bridge/camera/localization tests and `scripts/build-macos.sh`; inspect build warnings relevant to this change.
- [ ] T020 Run Windows build/tests where a Windows host/SDK exists; otherwise record the exact unexecuted checks in `validation.md`, review all project registrations and retain Windows acceptance as pending.
- [ ] T021 Exercise OS inactivity outcomes, manual saver/lock/sleep/lid, permission denial, elevated desktop and expiry cases on available hardware; record unexecuted cases explicitly in `validation.md` without treating mocks as physical proof.
- [x] T022 Review the inhibition/session/evidence path for security regressions, resource scheduling and callback lifetime; resolve confirmed issues and record review boundaries in `validation.md`.
- [x] T023 Update `README.md`, `docs/user-guide.md`, `api.md`, `ARCHITECTURE.md`, `CHANGELOG.md` and a permanent `adr/` decision with behavior, localization, increased monitoring and physical acceptance limits.
- [x] T024 Review `git diff --check`, scope, version consistency and diff statistics; update this plan phase notes and produce the self-contained delivery report.

**Notes**: Standard macOS package build, Xcode Release build, packaged smoke, signature verification, keep-awake/core-bridge/camera/localization/layout tests passed. The packaging source list and CI include the new controller/suite. Scoped security review fixed stale/queued lease and cached guard issues. `native/windows/` remains unchanged. Exact results and limitations are in [validation.md](validation.md). T020 is deferred; T021 physical acceptance remains unchecked.

**Checkpoint**: Automated evidence is explicit; physical acceptance cannot be checked off unless actually observed. No release/tag/publish or finalize/archive is part of this request.

## Dependencies & Execution Order

Setup → foundational contract → native integration + settings → validation/documentation. macOS and Windows implementations are independent directories and can run in parallel with the core against the fixed contract; each platform's tests precede its implementation. Core changes must pass before integrated builds. Final review waits for all agents. Tasks are not marked `[P]` because neighboring tasks share logical prerequisites; the directory-level parallel opportunities are documented here instead.

## Implementation Strategy

The original MVP estimate included both platforms (roughly 8–11h including setup/foundation). The owner explicitly narrowed this session to the shared core and macOS; Windows remains planned in its handoff. US-003/US-004 complete the usable option before delivery. Total: 24 tasks, five phases, approximately 14–20h of engineering work plus hardware scheduling. Story-specific tasks: US-001=4, US-002=6, US-003=4, US-004=5; shared tasks count for more than one story.

Use Luna agents for core, macOS and Windows with the primary agent coordinating contracts, review and documentation. Do not edit the same files concurrently. Implementation may refine internal type/file boundaries while preserving the contract and recording substantive changes here.

## Progress Tracking

Not Started · In Progress · Completed. Update checkboxes and phase notes as work proceeds. A pending hardware or Windows acceptance row is never reported as a passed check.
