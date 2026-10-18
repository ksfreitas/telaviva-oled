# ADR-0026: About screen and an informational update check against GitHub releases

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** about-screen-and-update-check
**Spec:** [Archived spec](../specs/archived/about-screen-and-update-check/spec.md)

## Context

Neither shell showed the installed version, the project address or the credits, and neither could tell the user that a newer build exists. Until now the app made no network request of its own, and `README.md` and `packaging/windows/README.md` said so. The version was also defined in several places (`crates/telaviva-core/Cargo.toml`, `native/macos/TelaViva/Info.plist`, a hard-coded constant in `AppController.cpp`, `app.manifest`) with nothing keeping them equal. The repository had no tags, no GitHub releases and no release workflow. The owner also asked that everything implemented from now on be applied to macOS and Windows alike.

## Decision

- Both shells get an About page inside the Settings window (macOS tab, Windows navigation item) and an "About" entry in the menu bar popover (macOS) and the tray right-click menu (Windows). The page shows the icon, the installed version, the project link and the credits, localized in en-US and pt-BR by each shell ([ADR 0015](0015-native-shells-own-ui-strings.md)).
- The update check is one anonymous HTTPS `GET https://api.github.com/repos/ksfreitas/telaviva-oled/releases/latest`. It runs only when the About page is open (result cached in memory for 6 hours; failures and rate limits for 30 seconds) or when the user presses "Check now" (not more than once per 30 seconds). There is no background polling, no check at launch, no setting to turn it off and nothing persisted. It only informs: it shows the remote version and opens the release page in the browser; it never downloads or installs anything. The request carries no identifier: no `Authorization`, no cookies, no query string, only the app version in `User-Agent`.
- The comparison and the parsing of the answer live in the Rust core as two pure, additive exports (`tv_core_version`, `tv_update_evaluate`; ABI version stays 1). The core validates the tag, ignores drafts and pre-releases, and builds the release URL itself as `https://github.com/ksfreitas/telaviva-oled/releases/tag/<tag>`; the server's `html_url` is never used. Each shell only does the transport (`URLSession` on macOS, WinHTTP on Windows, both off the UI thread, bounded timeouts, 1 MiB body cap) and the presentation.
- The version has one source of truth, `crates/telaviva-core/Cargo.toml`. The test `crates/telaviva-core/tests/version_consistency.rs` fails when `crates/telaviva-inference/Cargo.toml`, `Info.plist` (`CFBundleShortVersionString` and `CFBundleVersion`) or `app.manifest` (`X.Y.Z.0`) differ. The Windows hard-coded constant was removed. `AGENTS.md` lists every file to bump and the release procedure (tag `vX.Y.Z`, a published GitHub Release that is neither draft nor pre-release).
- `AGENTS.md` and the ARCHITECTURE.md conventions now state that every new feature is implemented on both platforms in the same change unless the owner scopes it to one.

## Alternatives Considered

- Duplicating semver and JSON handling in Swift and C++: two implementations and two test suites that could drift. Rejected for one tested core implementation.
- A Rust HTTP client in the core: a new dependency, a TLS stack and larger binaries, against the open-source-only and minimal-footprint rules. Rejected for the platform stacks.
- `Windows.Web.Http.HttpClient` instead of WinHTTP: more coroutine and timeout plumbing for a single GET. Rejected.
- Using the server's `html_url`: another path to validate for the same result. Rejected for a fixed-prefix URL built by the core.
- Checking at launch or on a timer, or an opt-out setting: more network use and a new settings field for a check that only informs. Rejected by the owner's choices (check when About opens; no switch).
- Auto-download or auto-install: builds are unsigned and a download path would widen the attack surface. Rejected.
- Persisting the cache: new state with no benefit while the trigger is the About page. Rejected.
- Generating `Info.plist` and the manifest from `Cargo.toml` at build time: touches both build chains and still needs a check. Rejected for validation by a test.

## Consequences

- The "no network requests of its own" statements were rewritten: the update check is now the app's only network request, and `AGENTS.md` forbids other network use without an ADR. Camera frames still never leave the device.
- Two more exports in `telaviva_core.dll` and the static library (12 total); `api.md`, the header and the header-contract tests were updated.
- No new dependency. Windows links `winhttp.lib` through a pragma.
- The Windows `ISettingsHost` interface gained `updateChecker()` and `appVersion()`; the tray menu has three rows, with keyboard navigation generalized.
- The plist and the manifest remain hand-edited copies of the version, guarded by the consistency test. The exe has no VERSIONINFO resource, as before.
- macOS Swift code in this change was reviewed but not compiled on the Windows development machine; its verification is the macOS CI job.

## Non-Obvious Constraints

- No release or tag existed when this was built, so the About page shows "No release has been published yet" until the first release is published with a `vX.Y.Z` tag.
- The unauthenticated GitHub API allows 60 requests per hour per IP, shared behind a NAT; hence the cache, the throttle and a distinct "rate limited" state.
- WinHTTP has no single total deadline, so the Windows timeouts are per phase (resolve 3 s, connect 3 s, send 3 s, receive 5 s); a black-holed network can take about 14 seconds to fail. macOS uses a 10 second total.
- The Windows projects do not compile with `/utf-8`: non-ASCII text in C++ sources must be written as universal character names (the architect's name in `AboutPage.xaml.cpp` is).
- The macOS app is not sandboxed today. If a sandbox is ever introduced it needs `com.apple.security.network.client` for this check.
- Windows enforces a single instance, so the About page can only be inspected on a build while no other `TelaViva.exe` is running.
