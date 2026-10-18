# Feature Specification: About Screen and Update Check

**Feature Branch**: `about-screen-and-update-check`

## Context and Understanding

TelaViva OLED now has a native macOS shell (SwiftUI/AppKit) and a native Windows shell (WinUI 3), both over the same Rust core. Neither shell tells the user which version is running, where the project lives or who made it, and neither can tell the user that a newer build exists. Today the version is scattered and partly hard-coded:

- `crates/telaviva-core/Cargo.toml` (`0.1.0`) is the only source the Windows packaging reads.
- `native/macos/TelaViva/Info.plist` has its own `CFBundleShortVersionString` and no `CFBundleVersion`.
- `native/windows/TelaViva/Host/AppController.cpp` hard-codes `kAppVersion = "0.1.0"`; the exe has no VERSIONINFO resource and `app.manifest` says `1.0.0.0`.
- No shell reads its own version at runtime for display.

The app has never made an outbound network request ("no telemetry and makes no network requests of its own" in `README.md` and `packaging/windows/README.md`). An update check against GitHub would be the first one, so it is a deliberate, documented exception to that statement, not a side effect.

There are no git tags, no GitHub releases and no release workflow yet, so the update check needs a defined source of truth on GitHub to compare against.

`AGENTS.md` is stale for this work: line 7 still says Windows is deferred (ARCHITECTURE.md says it no longer is) and the project map omits the Windows folders. The user wants the repository rule changed so that anything implemented from now on applies to both platforms.

## Feature Description

An "About" screen, reachable from the Settings window on both platforms, that presents the application with a polished, platform-native look:

- app icon, name and the installed version;
- a link to the project on GitHub (https://github.com/ksfreitas/telaviva-oled), opened in the default browser;
- credits: "Architected by Cássio de Freitas e Silva" and "Coded by Claude and Codex" (pt-BR: "Arquitetado por", "Codificado por");
- an update status: up to date, update available (showing the remote version and a way to open the release page), checking, or could not check.

The update check asks GitHub which version is the latest and compares it with the installed version. It only informs; it never downloads or installs anything. Strings are localized in en-US and pt-BR by each shell (ADR 0015), and the screen follows the Windows 11 look on Windows and the macOS look on macOS.

## Requirements *(mandatory)*

### Proposed solution

- **US-001**: As a user, I open Settings and find an "About" page showing the app icon, name and installed version, so I can tell which build I run (and quote it in a bug report).
- **US-002**: As a user, I click the GitHub address on the About page and it opens the project page in my default browser.
- **US-003**: As a user, I see who designed and who coded the app: "Architected by Cássio de Freitas e Silva" and "Coded by Claude and Codex".
- **US-004**: As a user, I see on the About page whether a newer version exists and, if so, which version it is and a button to open its release page on GitHub.
- **US-005**: As a user, I can ask for the check again, and I understand what happened when it cannot run (offline, GitHub unreachable, rate limited) without errors or dialogs.
- **US-006**: As a user on macOS or Windows, I get the same About content and behaviour, in my language (en-US or pt-BR), in the style of my platform.
- **US-007**: As a maintainer, I find in `AGENTS.md` (and the ARCHITECTURE.md conventions) the rule that every new feature is implemented on both platforms, and the stale Windows statements are corrected.
- **US-009**: As a user, I open "About TelaViva OLED" from the tray or menu bar and land directly on the About page.
- **US-008**: As a privacy-minded user, I find documented exactly what the update check sends and when, and that it is the only network request the app makes.

### Functional Requirements

- **FR-001**: Both shells MUST provide an About page inside the existing Settings window (macOS: a new tab; Windows: a new NavigationView item), with localized label "About" / "Sobre".
- **FR-002**: The About page MUST show the app icon, the product name and the installed version read at runtime from the running build, not from text typed into the screen.
- **FR-003**: The About page MUST show the GitHub URL `https://github.com/ksfreitas/telaviva-oled` as an activatable link that opens in the default browser.
- **FR-004**: The About page MUST show the credits "Architected by: Cássio de Freitas e Silva" and "Coded by: Claude and Codex", with pt-BR labels "Arquitetado por" and "Codificado por"; the names themselves are not translated.
- **FR-005**: The installed version MUST be the same number on macOS and Windows for a given release, with one documented source of truth feeding both shells and the packaging scripts (see Notes).
- **FR-006**: The system MUST check the latest published version on GitHub for `ksfreitas/telaviva-oled` and compare it with the installed version using semantic-version ordering; a remote version greater than the installed one means "update available".
- **FR-007**: When an update is available, the About page MUST show the remote version number and offer an action that opens the release page in the default browser; the app MUST NOT download, replace or run any file.
- **FR-008**: The About page MUST show a distinct, localized state for each of: checking, up to date, update available (with remote version), and could not check. A remote version equal to or lower than the installed one (for example a development build) MUST read as up to date.
- **FR-009**: The check MUST NOT block the UI, MUST have a bounded timeout, MUST NOT retry in a loop, and MUST fail silently into the "could not check" state (no dialog, no log spam, no crash).
- **FR-010**: The check MUST send no identifiers: no installation id, no device, monitor or camera data, no settings, and no user-specific headers beyond what an anonymous HTTPS request carries; the request goes over HTTPS only.
- **FR-011**: The check MUST be rate-conscious: the last result is cached for a bounded time and the unauthenticated GitHub limit is respected (an explicit "check now" may bypass the cache but is throttled).
- **FR-012**: Draft releases and pre-releases MUST be ignored when choosing the latest version.
- **FR-013**: The update check MUST respect the platform efficiency goals: no periodic background polling, no raised timer resolution, no wake of the UI thread while the About page is closed, and on Windows no change to EcoQoS or key-hook behaviour (ADR 0020).
- **FR-014**: All new user-visible text MUST live in each shell's own string catalog for en-US and pt-BR (macOS `Localization.swift`, Windows `Resources.resw`), with the existing key-parity tests extended to cover the new keys.
- **FR-015**: The version comparison and the parsing of the GitHub answer MUST be unit-tested in a way that does not need the network (fake responses), on both platforms.
- **FR-016**: `AGENTS.md` MUST state that everything implemented from now on is applied to both platforms (macOS and Windows) unless the owner explicitly scopes a task to one, correct the "Windows deferred" sentence, and list the Windows folders in the project map; the ARCHITECTURE.md CodeAdvisor Conventions MUST carry the same rule.
- **FR-016a**: `AGENTS.md` MUST list every file in which the application version is defined and has to be incremented on a release (at least `crates/telaviva-core/Cargo.toml`, `native/macos/TelaViva/Info.plist`, and whatever else the design step leaves as a copy: `native/windows/TelaViva/Host/AppController.cpp`, `native/windows/TelaViva/app.manifest`, a Windows VERSIONINFO resource if added, `Cargo.lock`), and say which of them are generated or validated by the build.
- **FR-020**: The Windows tray menu and the macOS menu bar menu MUST each have an "About TelaViva OLED" / "Sobre o TelaViva OLED" item that opens the Settings window already on the About page.
- **FR-021**: The update check MUST run only when the About page is opened or the user presses "Check now"; the check MUST NOT depend on or add any user setting.
- **FR-017**: `README.md`, `packaging/windows/README.md` and any other text that says the app makes no network requests MUST be updated to describe the update check precisely, and the decision MUST be recorded in an ADR.
- **FR-018**: The About page MUST meet the same accessibility bar as the other pages: keyboard reachable, readable by Narrator/VoiceOver (links and the update action have names), correct in light, dark and high-contrast themes, and legible at larger text sizes and display scales.
- **FR-019**: The macOS shell MUST be able to make the outbound HTTPS request under its current signing/entitlements setup (ad-hoc signed, no sandbox today); if a sandbox or entitlement change is ever introduced it must include network client access.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On each platform, a user reaches the About page from the Settings window in one navigation step and sees version, GitHub link and both credits without scrolling at the default window size.
- **SC-002**: The version shown on About equals the version in the release artifact name (`TelaVivaOLED-<version>-win-x64.zip` and the macOS bundle) on both platforms for the same release.
- **SC-003**: With a published release newer than the installed build, the About page shows "update available" with the correct remote version within 10 seconds of opening; with the same or older release it shows "up to date"; with the network disabled it shows "could not check" within the timeout and no dialog appears.
- **SC-004**: The app makes zero network requests unless the About page is open or the user asked for a check, verified by observing traffic during 10 minutes of normal use with About closed.
- **SC-005**: The check request carries no data derived from the user, the device or the settings (verified by inspecting the request in a test and a capture).
- **SC-006**: All new strings exist in en-US and pt-BR on both platforms, and the string-parity checks pass in CI on both platforms.
- **SC-007**: Version comparison tests cover equal, lower, higher, pre-release, malformed tag and a leading `v`, and run offline in CI on both platforms.
- **SC-008**: `AGENTS.md` contains the both-platforms rule and the list of files whose version must be bumped (and a test fails if those files disagree), and no document still claims the app never uses the network or that Windows is deferred.

## Clarification Needed

None open. All questions were answered by the owner (see Decisions).

## Decisions

Answered by the owner on 2026-10-17:

1. **When the check runs**: when the About page opens (with a cache of a few hours) plus a "Check now" button. No background polling and no check at launch.
2. **Source of the latest version**: the GitHub Releases endpoint (`/releases/latest`), ignoring drafts and pre-releases. Releases must be published with a `vX.Y.Z` tag for the check to be meaningful.
3. **"Update available" action**: show the remote version and open the release page in the browser. Nothing is downloaded or installed by the app.
4. **Version**: the same number on both platforms, with `crates/telaviva-core/Cargo.toml` as the single source; build scripts generate or validate the other places and a test checks they agree. `AGENTS.md` MUST list every file where the version has to be bumped.
5. **Menu entry**: add "About TelaViva OLED" / "Sobre o TelaViva OLED" to the Windows tray menu and the macOS menu bar menu, opening Settings directly on the About page.
6. **Opt-out**: no setting to disable the check. It never acts on its own (informs only, runs only when About is open or asked), so it needs no switch and no new `settings.json` field.

## Notes

- **Privacy exception.** The update check is the first outbound request the app would make. `README.md:64`, `packaging/windows/README.md:109`, the camera usage text and the "local only" wording must be reconciled (FR-017). The camera/presence pipeline stays fully local; the request contains no camera or device data.
- **Version sources today.** `crates/telaviva-core/Cargo.toml:3` (read by `build-app.ps1` and `build-windows.ps1`), `native/macos/TelaViva/Info.plist:12`, `native/windows/TelaViva/Host/AppController.cpp:72` (hard-coded, also keys the inference failure marker), `native/windows/TelaViva/app.manifest:3`, no VERSIONINFO. The design step must pick one source and generate or verify the rest; a CI/test check that the sources agree is advisable.
- **No releases yet.** No tags and no release workflow exist. The check is only useful once releases are published with a predictable tag (for example `v0.1.0`); a release workflow or a documented manual release step is a dependency, probably out of scope for this feature but needed to make it meaningful.
- **ABI.** Per ADR 0015 the core returns status codes and the shells own display text. Semver comparison could live in the Rust core as a pure module exposed by an additive C function (ABI stays version 1, `api.md` and header updated, header-drift tests apply), or be duplicated in each shell. Decide in design; sharing avoids two implementations drifting.
- **Networking per shell.** No HTTP client exists in either shell or in `Cargo.lock`. Prefer the platform stacks (`URLSession`, WinRT/WinHTTP) over adding a Rust HTTP dependency, given the open-source-only and minimal-footprint rules in `AGENTS.md`.
- **Windows constraints.** Run the request off the UI thread; no timer resolution changes; `Launcher::LaunchUriAsync` already used for external URIs. The About XAML class must be declared in `Project.idl` and listed in `TelaViva.vcxproj`.
- **macOS constraints.** Settings is an `NSWindow` (640x720, min 560x560) hosting `SettingsView`'s `TabView`; the app icon can be read from `NSApplication.shared.applicationIconImage`; Swift tests are standalone `@main` executables run by `tools/tests/test-*.sh`.
- **Tests.** Network is faked on both platforms; no test may call GitHub. Visual quality ("tela bonitinha") needs a rendered review at the display scales and themes used for the other pages; on Windows the session may be locked, so UI Automation dumps back up screenshots.
- **Docs.** CHANGELOG `Unreleased > Added` entry, ADR for the update check and the both-platforms rule, ARCHITECTURE.md section, `api.md` if an ABI function is added.
- **Hardware/OS validation** of the new page is by rendering and UI automation only; do not claim more than was observed.
