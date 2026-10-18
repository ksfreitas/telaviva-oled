# Tasks: About Screen and Update Check

**Branch**: about-screen-and-update-check **Specs**: [spec.md](spec.md) **Architecture**: [architecture.md](architecture.md) **Contracts**: [contracts.md](contracts.md) **Status**: Implemented, pending visual check and macOS CI ⏰

Story groups: **G1** core (US-004/005 logic, version), **G2** macOS shell (US-001..006, 009), **G3** Windows shell (US-001..006, 009), **G4** docs and repository rules (US-007, US-008).

Verification note: this machine runs Windows. Rust and Windows work is built and tested here; the macOS Swift code cannot be compiled here, so its verification is the macOS CI job plus review of the diff. Say so in the final report.

## Cross-task interfaces (fixed here so parallel tasks agree)

Core C ABI: exactly as in `contracts.md` (`tv_core_version`, `tv_update_evaluate`).

macOS (`UpdateChecker.swift`, Foundation only):
```swift
enum UpdateState: Equatable { case idle, checking, upToDate(remote: String), updateAvailable(remote: String, url: URL), noRelease, rateLimited, failed }
struct UpdateOutcome: Equatable { let status: String; let remoteVersion: String?; let releaseURL: String? }
protocol UpdateFetching { func fetch(_ request: URLRequest) async throws -> (status: Int, body: Data) }
@MainActor final class UpdateChecker: ObservableObject {
    @Published private(set) var state: UpdateState
    init(installedVersion: String, fetcher: UpdateFetching, evaluate: @escaping (String, Int, Data) -> UpdateOutcome?, now: @escaping () -> Date = Date.init)
    func checkIfNeeded()   // About appeared
    func checkNow()        // button; no-op while checking or < 30 s after the last attempt
    static let endpoint: URL
}
#if TELAVIVA_CORE
enum CoreUpdate { static func version() -> String?; static func evaluate(installed: String, status: Int, body: Data) -> UpdateOutcome? }
#endif
enum SettingsTab: Hashable { case general, keyboard, ambient, about }
final class SettingsNavigation: ObservableObject { @Published var tab: SettingsTab = .general }   // SettingsNavigation.swift
```

Windows (`Platform/UpdateChecker.h`, namespace `telaviva`):
```cpp
enum class UpdateKind { Idle, Checking, UpToDate, UpdateAvailable, NoRelease, RateLimited, Failed };
struct UpdateState { UpdateKind kind = UpdateKind::Idle; std::string remoteVersion; std::string releaseUrl; };
struct HttpResult { bool transportOk = false; int status = 0; std::string body; };
struct IHttpGet { virtual ~IHttpGet() = default; virtual HttpResult get(const std::string& url, const std::string& userAgent) = 0; };
struct UpdateOutcome { std::string status; std::string remoteVersion; std::string releaseUrl; };
using UpdateEvaluator = std::function<std::optional<UpdateOutcome>(const std::string& installed, int status, const std::string& body)>;
using JobRunner = std::function<void(std::function<void()>)>;
class UpdateChecker {   // shared internal state; safe if destroyed while a job runs
public:
    UpdateChecker(std::string installedVersion, std::shared_ptr<IHttpGet> http, UpdateEvaluator evaluate, std::shared_ptr<IClock> clock, JobRunner runner);
    UpdateState state() const;
    void checkIfNeeded();
    void checkNow();
    void setOnChanged(std::function<void()> onChanged);   // called from the worker thread
};
// CoreBridge: static std::string coreVersion(); static std::optional<UpdateOutcome> evaluateUpdate(const std::string&, int, const std::string&);
// Host: ISettingsHost gains `virtual UpdateChecker& updateChecker() = 0;` and `virtual const std::string& appVersion() const = 0;`
// SettingsWindow gains implementation-only `void SelectPage(SettingsPage page)` with `enum class SettingsPage { General, Keyboard, Ambient, About }`; App::openSettings(std::optional<SettingsPage> = std::nullopt)
```

## Phase 1: Core foundation ⏳ (~3h)
⚠️ CRITICAL: blocks the shells (they call the two exports).

- [x] T001 [G1] `crates/telaviva-core/src/update.rs` (+ `pub mod update;` in `lib.rs`): strict semver parser (contracts "Semver rules"), `core_version()`, `evaluate(installed, http_status, body) -> Result<String /*json*/, UpdateError>` mapping all statuses in `contracts.md`, tag charset validation, built release URL, body cap 1 MiB; inline unit tests for every row of the contract (equal, lower, higher, `0.10.0` vs `0.9.0`, leading `v`, remote pre-release/draft ignored, installed pre-release below release, malformed tags, missing fields, 404/403/429/500, empty 200 body, bad JSON, illegal tag characters like `../x`).
- [x] T002 [P] [G1] `crates/telaviva-core/tests/version_consistency.rs` and add `CFBundleVersion` (same value) next to `CFBundleShortVersionString` in `native/macos/TelaViva/Info.plist`: test `include_str!`s `../../telaviva-inference/Cargo.toml`, `../../../native/macos/TelaViva/Info.plist`, `../../../native/windows/TelaViva/app.manifest` and compares with `env!("CARGO_PKG_VERSION")` (manifest expects `<version>.0`); failure messages name the file to fix.
- [x] T003 [G1] After T001: `ffi.rs` exports `tv_core_version` and `tv_update_evaluate` (guarded, null/size checks, out written only on `TV_OK`, `invalid_argument` rules from contracts); declarations with comments in `include/telaviva_core.h`; entries in `crates/telaviva-core/tests/header_contract.rs` (typed binding + `Export` row with canonical prototype); new `crates/telaviva-core/tests/update_contract.rs` for the two exports (ownership with `tv_buffer_free`, null `out`, null input with length, oversize, malformed installed, each outcome status).
- [x] T004 [G1] After T003: `api.md` (table rows, "Additive to ABI version 1") and `CHANGELOG.md` `Unreleased > Added` entry.
**Note (done)**: cargo fmt/clippy/test for the workspace pass (core: 19 unit + 3 version + update_contract tests), DLLs rebuilt, 12 core exports match the header (check-dll-exports.ps1). A Haiku worker's tests were run by the orchestrator; 1.0.0  (trailing space) tags are trimmed per the contract, so that case is accepted.
**Checkpoint**: ✋ orchestrator runs `cargo fmt --check`, `cargo clippy -D warnings`, `cargo test --workspace --all-targets --locked`, then `tools\windows\build-rust-dlls.ps1` and `tools\windows\check-dll-exports.ps1`.

## Phase 2: Windows shell (G3) ⏳ 🎯 (~6h)
**Goal**: About page, tray row and update check on Windows. **Independent Test**: `tests.exe` green; UI Automation dump of the About page; offline run shows "Could not check".
- [x] T010 [G3] After Phase 1: `Platform/CoreBridge.h/.cpp` wrappers (`coreVersion`, `evaluateUpdate` via `tv_core_version`/`tv_update_evaluate`, RAII free, tiny JSON read of the three string fields); `Platform/UpdateChecker.h/.cpp` as in the interface block (6 h / 30 s cache, 30 s throttle, in-flight guard, shared state); `Platform/WinHttpGet.h/.cpp` (`#pragma comment(lib,"winhttp.lib")`, automatic proxy, cookies disabled, timeouts 3/3/3/5 s, 1 MiB cap, HTTPS only, headers from contracts); `Tests/Fakes.h` fake HTTP if shared; `Tests/update_checker.cpp` covering: cache hit within 6 h, expiry, failure cache 30 s, throttle, no double job while checking, transport failure -> Failed, each outcome status mapping, unknown status -> Failed, destroy checker while job pending. Warning-clean at /W4 /WX.
- [x] T011 [P] [G3] After Phase 1, localization: add the 16 keys of `contracts.md` (Windows column) to both `Strings/en-US/Resources.resw` and `Strings/pt-BR/Resources.resw`; asset `native/windows/TelaViva/Assets/AppIcon256.png` extracted/rendered from the existing icon (extend `tools/windows/make-app-icon.ps1` or document the one-off command) and wired as Content in `TelaViva.vcxproj`.
- [x] T012 [G3] After T010 and T011: `AboutPage.xaml/.h/.cpp` (Project.idl block, vcxproj Page/ClInclude/ClCompile), layout per architecture decision 8, `SetHost`, `RefreshStrings`, on Loaded -> `checkIfNeeded()` and subscribe to `setOnChanged` with an alive flag and `DispatcherQueue().TryEnqueue`, buttons Check now / Open release page (`Launcher::LaunchUriAsync`), GitHub `HyperlinkButton`, automation names, `{version}` substitution; `Host/SettingsHost.h` + `Host/AppController.*` (create the checker with `WinHttpGet`, `CoreBridge::evaluateUpdate`, `SystemClock`, `ThreadPool::RunAsync` runner; delete `kAppVersion`, use `CoreBridge::coreVersion()` for `acceleration.appVersion` and `appVersion()`).
- [x] T013 [G3] After T012: `SettingsWindow.xaml/.h/.cpp` (AboutItem glyph E946, page member, SelectionChanged branch, RefreshStrings, `SelectPage`), `App.xaml.h/.cpp` (`openSettings(page)`, also selecting on an already open window; tray `SetActions` third callback), `TrayMenuWindow.xaml/.h/.cpp` (About row between Settings and Quit, glyph E946, keyboard navigation for N rows, `applyStrings` name and text).
**Note**: solution builds with 0 errors; `tests.exe` 634 tests, 0 failed (615 + 19 new). Packaging to a temp folder works and the packaged `--smoke-test` exits 0 with `Assets\AppIcon256.png` and `VERSION` 0.1.0. Not yet done: UI Automation dump and rendered review, because the single-instance guard means the owner's running `TelaViva.exe` must be closed first. A Haiku slip was caught in review: `L\"Cássio..."` literal without `/utf-8` would show mojibake; now a universal character name.
**Checkpoint**: ✋ `scripts\build-windows.ps1` passes (all tests); UIA dump shows About page, tray About row, offline state; rendered review recorded in the validation notes.

## Phase 3: macOS shell (G2) ⏳ 🎯 (~5h)
**Goal**: same behaviour on macOS. **Independent Test**: `tools/tests/test-update-checker.sh` and `test-core-bridge.sh`, camera-localization parity, macOS CI green.
- [x] T020 [G2] After Phase 1: `native/macos/TelaViva/UpdateChecker.swift` per interface block (Foundation only; `URLSessionUpdateFetcher` ephemeral, no cookies/cache, `timeoutIntervalForResource = 10`, headers from contracts; `#if TELAVIVA_CORE enum CoreUpdate`), `native/macos/Tests/UpdateCheckerTests.swift` (same case list as the Windows tests, with a fake fetcher and clock), `tools/tests/test-update-checker.sh` (pattern of `test-popup-layout.sh`), `.github/workflows/macos.yml` step, `README.md` list; extend `CoreBridgeTests.swift` + `tools/tests/test-core-bridge.sh` to exercise `CoreUpdate.evaluate` and `CoreUpdate.version` against the real core.
- [x] T021 [P] [G2] After Phase 1: `Localization.swift` keys (macOS column of `contracts.md`) in both locale dictionaries (single-line dictionaries; keep format); `SettingsNavigation.swift`; `AboutView.swift` (layout per decision 8, takes `core`-independent `installedVersion`, `UpdateChecker`, locale string; `.task`/`onAppear` -> `checkIfNeeded()`; `Link` for GitHub; Open release page via `NSWorkspace.shared.open`; accessibility labels).
- [x] T022 [G2] After T020 and T021: `SettingsView.swift` (`TabView(selection:)` with `.tag` on four tabs, About tab, `@ObservedObject navigation`, `updateChecker` parameter with default so `App.swift` still compiles), `MenuBarController.swift` (own `SettingsNavigation` and `UpdateChecker`, installed version from `Bundle.main` `CFBundleShortVersionString`, `openSettings(tab: SettingsTab? = nil)`, pass an `openAbout` closure), `MonitorPopup.swift` (About button in the button row), `App.swift` if the signature requires it, `project.pbxproj` (3 new Swift files: file ref, build file, group, sources), `packaging/macos/build-app.sh` SOURCES.
**Checkpoint**: ✋ diff review against briefings; Swift cannot be compiled on this machine, so confirm that the macOS CI job (not run locally) is the verification, and say so.

## Phase 4: Docs and repository rules (G4) ⏳ (~1.5h)
- [x] T030 [P] [G4] `AGENTS.md`: rule that everything implemented from now on applies to macOS and Windows unless the owner scopes it; correct "Windows deferred"; project map with `native/windows/`, `packaging/windows/`, `scripts/`, `.github/workflows/`; a "Release version" list of every file to bump (`crates/telaviva-core/Cargo.toml` source of truth, `crates/telaviva-inference/Cargo.toml`, `native/macos/TelaViva/Info.plist` both keys, `native/windows/TelaViva/app.manifest` as `X.Y.Z.0`, `Cargo.lock`, `CHANGELOG.md` heading) with which are validated by `cargo test -p telaviva-core --test version_consistency`, and the release procedure (tag `vX.Y.Z`, published non-draft non-pre-release GitHub Release); privacy line that the update check is the only network request. `ARCHITECTURE.md`: same both-platforms rule in CodeAdvisor Conventions and a section for this feature.
- [x] T031 [P] [G4] `README.md` line 64 and `packaging/windows/README.md` line 109: replace "makes no network requests of its own" with the precise statement (one anonymous HTTPS GET to the GitHub releases API, only when About is open or "Check now" is pressed; sends nothing but the app version in `User-Agent`; camera and presence stay local). Check macOS README "does not download runtime files when it runs" still holds (it does: the check downloads no files).
**Checkpoint**: ✋ grep shows no remaining "makes no network requests" or "Windows deferred" claims.

## Final Phase: Verify and polish ⏳ (~1.5h)
- [ ] T040 Orchestrator: full Windows build + tests, Rust checks, packaged `--smoke-test`, UIA/offline run of the About page, `/codeadvisor-dev:fix-idea-diagnostics` on changed files, then commit; no push without the owner asking.

## Dependencies & Execution Order
Phase 1 -> Phases 2 and 3 in parallel (disjoint files) -> Phase 4 (parallel with 2 and 3, touches only docs) -> Final.
Inside Phase 1: T001 and T002 in parallel, then T003, then T004. Inside Phase 2: T010 and T011 in parallel, then T012, then T013. Inside Phase 3: T020 and T021 in parallel, then T022.

## Implementation Strategy
Core first, shells next, each shell built to the fixed interfaces above so both can proceed at once. Windows first to verify (only platform runnable here). Haiku workers apply the briefings; the orchestrator runs every build and test and reviews every diff.

## Progress Tracking
⏳ Not Started · ⏰ In Progress · ✅ Completed — update as you go, including the Notes sections per phase.
