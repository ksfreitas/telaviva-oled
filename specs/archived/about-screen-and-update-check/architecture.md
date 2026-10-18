# Architecture: About Screen and Update Check

**Feature**: about-screen-and-update-check **Date**: 2026-10-17 **Branch**: about-screen-and-update-check **Specs**: [spec.md](spec.md), [contracts.md](contracts.md)

## Summary
Each native shell gets an About page (macOS tab in the Settings window, Windows NavigationView page) and a menu entry that opens it. The Rust core gains two pure, additive C functions: one exposes the build's version, the other compares the installed version with a GitHub `releases/latest` answer and returns a status. Each shell performs the one HTTPS GET with its platform stack, off the UI thread, and renders the result from its own string catalog. No setting, no persistence, no background polling.

## Technical Context
**Language/Stack**: Rust (core, `serde_json` already a dependency), Swift/SwiftUI (macOS 14, `URLSession`, Swift concurrency), C++/WinRT + WinUI 3 (WinHTTP in the `Platform` static lib).
**Key Dependencies**: none new. No `url` crate, no Rust HTTP client; platform HTTP stacks only (open-source-only rule and minimal footprint).
**Storage**: none. Update state is in memory per process.
**Testing**: Rust unit and contract tests (semver, evaluate, header contract, version consistency); Swift standalone test executable run by `tools/tests/test-update-checker.sh`; Windows `tests.exe` TEST/CHECK cases with fake HTTP, clock and job runner; strings parity tests on both platforms; rendered review by UI Automation (Windows) and screenshots (macOS) for the visual page.
**Platform**: macOS 14+ Apple Silicon, Windows 11 x64. Both implemented in the same change (new repository rule).

## Technical Decisions

### Decision 1: Version comparison and response parsing live in the Rust core
**What**: New pure module `crates/telaviva-core/src/update.rs`, exposed by `tv_update_evaluate` (inputs: installed version, HTTP status, body; output: UpdateOutcome JSON in a Rust-owned `TvBuffer`). `tv_core_version` exposes `CARGO_PKG_VERSION`.
**Why**: One implementation of semver, tag validation and status mapping for both shells (no drift), fully unit-testable offline, and consistent with ADR 0015 (core returns codes/data, shells own text). The shells keep only transport and presentation.
**Alternatives Considered**: (a) duplicate semver and JSON parsing in Swift and C++ (two implementations, two test suites); (b) a Rust HTTP client in the core (new dependency, TLS stack, larger binaries, energy cost).
**Trade-offs**: Two more exports (ABI stays 1, additive); header contract tests and `api.md` must be updated. Windows has no fake core in tests, so Windows tests call the real core DLL, as `core_bridge_smoke` already does.

### Decision 2: Reuse the existing out-`TvBuffer` convention
**What**: Both functions write a Rust-allocated UTF-8 buffer freed with `tv_buffer_free`, and write `out` only on `TV_OK`.
**Why**: It is the only existing ownership pattern; `header_contract.rs` already supports the needed C types (`uint8_t *`, `size_t`, `uint32_t`, `int32_t`, `TvBuffer *`). A `const char *` return or caller-allocated struct would be new patterns and need test-mapper changes.
**Alternatives Considered**: `const char *tv_core_version(void)` returning a static string (simpler for callers but a new C type in the contract test); caller-allocated fixed struct (new ownership rule).
**Trade-offs**: Callers wrap two calls and free the buffer; small helpers hide that.

### Decision 3: The release URL is built, never trusted
**What**: The core ignores the server's `html_url` and builds `https://github.com/ksfreitas/telaviva-oled/releases/tag/<tag_name>` after validating `tag_name`.
**Why**: The URL is opened in the user's browser; a validated, fixed-prefix URL removes open-redirect/scheme-injection risk from a malformed or spoofed answer.
**Alternatives Considered**: use `html_url` with a prefix check (more code paths for the same result).

### Decision 4: Single version source, checked by a test
**What**: `crates/telaviva-core/Cargo.toml` is the source. A new Rust integration test (`crates/telaviva-core/tests/version_consistency.rs`) `include_str!`s `telaviva-inference/Cargo.toml`, `Info.plist` and `app.manifest` (precedent: `header_contract.rs` uses `include_str!` with relative paths) and fails if they differ. Windows' hard-coded `kAppVersion` is deleted; `AppController` and the About page use `tv_core_version`. macOS adds `CFBundleVersion` and displays `CFBundleShortVersionString`. `AGENTS.md` lists every file to bump.
**Why**: Runs in both macOS and Windows CI with `cargo test`; no build-script plumbing; the packaging scripts already read the core `Cargo.toml`.
**Alternatives Considered**: generating `Info.plist`/manifest from Cargo.toml at build time (touches both build chains, still needs the check); a shared `workspace.package.version` (would also need a cargo change in both crates and would not cover the plist/manifest).
**Trade-offs**: The plist and manifest remain hand-edited copies, mitigated by the failing test. A Windows VERSIONINFO resource is not added (would be a fourth copy); the Windows About page reads the core, the exe's Details tab stays blank as today.

### Decision 5: HTTP per platform, off the UI thread, stateless
**What**:
- macOS: `UpdateChecker` (`ObservableObject`, main-actor state) with an injectable fetcher protocol; the real fetcher uses an ephemeral `URLSession` (no cookies, no cache, `timeoutIntervalForResource = 10`).
- Windows: `Platform/UpdateChecker` with injected `IHttpGet`, `IClock` and a job-runner `std::function`; the real `WinHttpGet` (`winhttp.lib` via `#pragma comment`) uses automatic proxy detection, cookies disabled, the timeouts from the contract and a 1 MiB cap; the job runs on `ThreadPool::RunAsync`; results reach the page through its `DispatcherQueue` with an alive flag (precedent: `TrayHost` `alive_`).
**Why**: Native stacks honor system proxies and certificate stores, add no dependencies, and keep the shared logic in Rust. Injected seams follow existing patterns (`IClock`, `IScheduler`, `ISleeper`, Swift protocols with fakes).
**Alternatives Considered**: `Windows.Web.Http.HttpClient` (coroutine/timeout plumbing heavier than WinHTTP for one GET); `libcurl`/`reqwest` (new dependency).
**Trade-offs**: Two transport implementations, each tiny; WinHTTP has no single total deadline, so the timeouts are per phase (worst case about 14 s on a black-holed network; SC-003 timing applies to normal failures).

### Decision 6: When it runs, and caching
**What**: The About page calls `checkIfNeeded()` when it appears; `Check now` calls `checkNow()`. Successful outcomes (`upToDate`, `updateAvailable`, `noRelease`) are reused for 6 h; `rateLimited` and `failed` for 30 s; `checkNow` is a no-op within 30 s of the last attempt and while checking. In memory only.
**Why**: Satisfies decisions 1 and 6 of the spec (no polling, no setting, no persistence), stays far under the unauthenticated 60/h limit, and costs nothing while About is closed (EcoQoS and ADR 0020 untouched: no timers, no process-power writes, no UI-thread wake).
**Alternatives Considered**: persisted cache file (new state, no benefit given the trigger).

### Decision 7: Opening About from the menus
**What**: Windows: a third row "About TelaViva OLED" in `TrayMenuWindow` (between Settings and Quit), keyboard navigation generalized from two to N rows, `SetActions` gets an `openAbout` callback, `App::openSettings(page)` and `SettingsWindow::SelectPage` select the About item (also when the window already exists). macOS: the status item opens a popover (no `NSMenu`), so the popover's button row gets an "About" button; `MenuBarController.openSettings(tab:)` writes the requested tab to a shared `SettingsNavigation` observable that `SettingsView` binds to (`TabView(selection:)` with tags on all four tabs), because an existing `NSWindow` keeps its original root view.
**Why**: Matches each platform's existing menu mechanism; the observable also covers the second `SettingsView` instance in the SwiftUI `Settings` scene (it simply keeps its own default instance).
**Alternatives Considered**: re-hosting a new root view per open (loses unsaved drafts); an `NSMenu` on the status item (changes the established popover interaction).

### Decision 8: Page look
**What**: macOS: centered `ScrollView` stack, app icon from `NSApplication.shared.applicationIconImage` (96 pt), name in `.title2.bold()`, version in secondary `.caption`, the project link, a rounded card (`.quaternary.opacity(0.45)`, radius 10, as in `MonitorPopup`) holding the two credit rows, and an update row with SF Symbols (`checkmark.circle.fill`, `arrow.down.circle.fill`, `exclamationmark.triangle`, `ProgressView`). Windows: `ScrollViewer` (Padding 24,8,24,24) with `PageTitle` (TitleTextBlockStyle), a header block with the 256 px app icon (PNG derived from the existing icon, copied to output, referenced as `ms-appx:///Assets/AppIcon256.png`), name and version, then cards built with `MakeSettingRow`/`MakeSectionHeader` helpers for the project link (`HyperlinkButton`), credits and updates (FontIcon glyphs, `ProgressRing`, `Button` for Check now/Open release). All text through the string catalogs; `AutomationProperties` names on links and buttons; theme brushes only (light, dark, high contrast).
**Why**: Reuses each platform's settings-page vocabulary so the page looks native and consistent.
**Alternatives Considered**: a separate About window (extra window management, focus/EcoQoS visibility rules).

## Architecture Overview
Before: shells show no version and make no network request. After: `tv_core_version` and `tv_update_evaluate` are stateless core exports; each shell owns an `UpdateChecker` that is created with the app, holds the in-memory state and cache, and is observed only by the About page. Opening About (or "Check now") starts one job: GET to GitHub, then `tv_update_evaluate`, then state update, then UI refresh on the UI thread. Closing the page abandons nothing (the job finishes harmlessly into the checker, guarded by shared state/alive flags).

### Component Structure
**New Files/Modules:**
- `crates/telaviva-core/src/update.rs` - semver parsing, tag validation, outcome mapping, JSON output; unit tests inline.
- `crates/telaviva-core/tests/version_consistency.rs` - fails when Cargo/plist/manifest versions differ.
- `crates/telaviva-core/tests/update_contract.rs` - FFI-level tests for the two exports (null/size/invalid cases, ownership).
- `native/macos/TelaViva/UpdateChecker.swift` - state machine, cache, fetcher protocol, URLSession fetcher; no SwiftUI/AppKit imports.
- `native/macos/TelaViva/AboutView.swift` - SwiftUI About page.
- `native/macos/TelaViva/SettingsNavigation.swift` - `SettingsTab` enum and `SettingsNavigation` observable.
- `native/macos/Tests/UpdateCheckerTests.swift`, `tools/tests/test-update-checker.sh`.
- `native/windows/Platform/UpdateChecker.h/.cpp` - state machine and cache (no XAML, /W4 /WX).
- `native/windows/Platform/WinHttpGet.h/.cpp` - `IHttpGet` interface and WinHTTP implementation.
- `native/windows/TelaViva/AboutPage.xaml/.h/.cpp` - the page (declared in `Project.idl`, listed in `TelaViva.vcxproj`).
- `native/windows/TelaViva/Assets/AppIcon256.png` - icon for the page.
- `native/windows/Tests/update_checker.cpp` - fake HTTP/clock/job-runner tests.

**Modified Files:**
- `crates/telaviva-core/src/lib.rs`, `src/ffi.rs`, `include/telaviva_core.h`, `tests/header_contract.rs`, `api.md`, `CHANGELOG.md` - two additive exports.
- `native/macos/TelaViva/Info.plist` - add `CFBundleVersion`.
- `native/macos/TelaViva/SettingsView.swift` - selection binding, tags, About tab, checker/navigation parameters.
- `native/macos/TelaViva/MenuBarController.swift`, `MonitorPopup.swift`, `App.swift` - About entry, `openSettings(tab:)`, owning checker and navigation.
- `native/macos/TelaViva/Localization.swift` - new keys in both locales.
- `native/macos/TelaViva.xcodeproj/project.pbxproj`, `packaging/macos/build-app.sh` (SOURCES), `.github/workflows/macos.yml`, `README.md` - register files and the new test.
- `native/windows/Platform/CoreBridge.h/.cpp` - `coreVersion()` and `evaluateUpdate()` wrappers.
- `native/windows/TelaViva/Host/AppController.cpp/.h`, `Host/SettingsHost.h` - own the checker, remove `kAppVersion`, expose checker and version to the About page.
- `native/windows/TelaViva/SettingsWindow.xaml/.h/.cpp`, `App.xaml.cpp/.h`, `TrayMenuWindow.xaml/.h/.cpp`, `Project.idl`, `TelaViva.vcxproj`, `Strings/en-US/Resources.resw`, `Strings/pt-BR/Resources.resw`.
- `AGENTS.md`, `ARCHITECTURE.md`, `README.md`, `packaging/windows/README.md` - rules, privacy wording, version list.

### Component Relationships
Core (`update.rs`) <- C ABI <- `CoreBridge` (Windows) / a small Swift wrapper (macOS) <- `UpdateChecker` <- About page. `UpdateChecker` depends only on injected seams (HTTP, clock, evaluator, job runner), so it is tested without network, UI or window system.

### Data Flow
Open About -> `checkIfNeeded()` -> fresh cache? show it : state `checking` -> worker: GET -> `(status, body)` -> `tv_update_evaluate(installed, status, body)` -> UpdateOutcome -> `UpdateState` stored with timestamp -> UI thread refresh -> page renders text from catalog, `{version}` substituted; "Open release page" launches the built URL in the default browser.

## Implementation Approach

### User Story Mapping
**US1/US3 (page, version, credits)**: `AboutView.swift`, `AboutPage.*`, catalogs; tested by strings parity and rendered review.
**US2 (GitHub link)**: `Link`/`openURL` (macOS), `HyperlinkButton` + `Launcher::LaunchUriAsync` (Windows); constant URL.
**US4/US5 (update status, check now, failure states)**: core `update.rs`, `UpdateChecker` on both platforms; offline tests with fakes.
**US6 (parity)**: same keys, same states, same cache rules, same contract; both platforms land in one change.
**US7 (AGENTS.md rule and version list)**: `AGENTS.md`, `ARCHITECTURE.md` conventions, `version_consistency.rs`.
**US8 (privacy)**: README, packaging README, ADR written at finalize, request contract in `contracts.md`.
**US9 (menu entry)**: `TrayMenuWindow`, `App::openSettings(page)`, `MonitorPopup`, `MenuBarController`, `SettingsNavigation`.

## Integration Points
Core C ABI (`include/telaviva_core.h`, `api.md`), existing `tv_buffer_free`, Windows `CoreBridge`, Windows `ISettingsHost`, macOS `CoreBridge` bridging header, `Localization.swift`, `Resources.resw`, CI workflows (`macos.yml` new test step; Windows `tests.exe` picks up new `*.cpp` through the glob).

## Technical Constraints
- ABI version stays 1; exports defined in `telaviva-core` itself (ADR 0017, MSVC export reliability); header drift tests must pass.
- Windows keys follow the `strings_keys` rules: no key may also be a scope prefix of another (`settings.tab.about` and `about.*` are fine; no bare `about` key), identical key sets in both resw files, no blank values, no "Mac" wording.
- New macOS Swift files must be registered by hand in `project.pbxproj` (file ref, build file, group, sources phase), `build-app.sh` SOURCES and in any test script compiling them.
- Windows XAML classes must be in `Project.idl` and `TelaViva.vcxproj`; Platform stays free of XAML and warning-clean (/W4 /WX).
- No timer-resolution changes, no process EcoQoS writes (ADR 0020).
- macOS is not sandboxed (no entitlements today); networking works without changes. Introducing a sandbox later requires `com.apple.security.network.client`.

## Risks & Mitigations
| Risk | Impact | Mitigation |
|---|---|---|
| No releases or tags exist yet | About shows "No release has been published yet" until the first release | Distinct `noRelease` state; AGENTS.md documents publishing a non-draft, non-prerelease GitHub Release tagged `vX.Y.Z` |
| Version copies drift | Wrong version shown or false update prompt | `version_consistency.rs` in both CI jobs; list in AGENTS.md |
| Spoofed/malformed GitHub answer | Wrong prompt or unsafe URL | Core validates tag charset, ignores `html_url`, builds a fixed-prefix HTTPS URL, caps body at 1 MiB |
| Unauthenticated rate limit (60/h/IP, shared NAT) | Check fails for some users | `rateLimited` state with its own message, 6 h cache, 30 s throttle |
| Page closed or app quits mid-request | Dangling references | Job captures shared state / alive flag; checker owned by the app, outlives windows |
| Slow network blocks UI | Frozen About page | Request only on worker thread/async; bounded timeouts |
| "No network requests" promise now false | Trust | README/packaging README/ADR rewritten to describe exactly one anonymous request, only when About is open or asked; request carries only the app version in `User-Agent` |
| Windows icon PNG not resolved by unpackaged `ms-appx` | Missing icon | Verify rendering by UIA/screenshot; fall back to a FontIcon only if needed |

## Open Questions
None.

## Next Steps
1. Review this architecture.
2. Run `/codeadvisor-dev:plan`.
