# TelaViva OLED

## Purpose and status

TelaViva OLED is a planned Rust desktop application for macOS and Windows. It controls external monitor brightness and volume through DDC and provides OLED-oriented brightness protection based on webcam presence and screensaver state.

Implementation is in progress. The repository contains a Cargo workspace, shared Rust policy/C ABI code, local ONNX inference, a native macOS shell, and a native Windows 11 x64 shell (WinUI 3). See the implementation plan and validation reports for completed checks, including the Windows hardware checklist and the macOS validation reports; do not infer physical hardware support or release readiness from code presence.

## Project map

- `ARCHITECTURE.md`: intended architecture, requirements, unresolved design questions, and CodeAdvisor conventions.
- `CHANGELOG.md`: release history, starting with Unreleased.
- `docs/`: user guide with the detailed behavior of each feature; the README is the short entry point.
- `adr/`: permanent architecture decision records.
- `specs/archived/`: the archived record of each completed feature (specification, design, plan and validation reports). Use `ARCHITECTURE.md`, `api.md` and `adr/` for the current design; the initial application's records are in `specs/archived/external-monitor-control-and-oled-presence-protection/`.
- `crates/telaviva-core/`: shared policy, settings and native-shell C ABI; the camera lifecycle and controls handles (`src/controls/`: manual-control queue, key policy and DDC dispatch driven by the shells through `tv_controls_*`).
- `crates/telaviva-inference/`: image quality assessment and local YuNet/ONNX detection.
- `native/macos/`: native SwiftUI/AppKit shell and macOS device adapters.
- `native/windows/`: native WinUI 3 / C++/WinRT shell (`TelaViva/`), the UI-free `Platform/` static library, and `Tests/`.
- `packaging/macos/`: local application build and packaging instructions.
- `packaging/windows/`: per-user zip build and installer scripts for Windows.
- `scripts/`: one-command build scripts for macOS and Windows.
- `.github/workflows/`: macOS and Windows CI.

## Development guidance

- Use Rust and Cargo for a modular monolithic application targeting macOS and Windows.
- Prioritize minimal CPU, memory, camera, and battery usage when evaluating designs.
- Presence means detecting any face, without identifying a specific person or requiring face enrollment.
- Keep intended behavior distinct from implemented and hardware-verified capabilities.
- Determine monitor capabilities, platform limitations, and failure behavior during design; do not assume uniform DDC support.
- Follow the selected design in `ARCHITECTURE.md`, `api.md` and the ADRs in `adr/` (especially `adr/0001-native-shell-and-rust-core.md`): native SwiftUI/AppKit and WinUI 3 shells, one shared Rust core with a versioned C ABI, local face detection through ONNX Runtime (since 2026-10-29 macOS uses RetinaFace MobileNet0.25, see ADR 0042; Windows still uses YuNet until its port), and a selectable inference device with validated acceleration and CPU fallback.
- Use only open-source redistributed dependencies and assets. Initial macOS support targets Apple Silicon; Intel is deferred and DDC compatibility remains connection-specific, including on M1.
- Follow the process conventions in ARCHITECTURE.md. Documentation and commit language default to en-US.
- Everything implemented from now on applies to both platforms (macOS and Windows) in the same change, with equivalent behavior, localized strings (en-US and pt-BR) and tests on each, unless the owner explicitly scopes a task to one platform. The MacBook features are called Laptop (en-US) and Notebook (pt-BR) on Windows UI only.
- The About page's update check is the only network request the app makes: one anonymous HTTPS GET to the GitHub releases API (`ksfreitas/telaviva-oled`, `releases/latest`), made only when the About page is open or "Check now" is pressed. Do not add other network use without an ADR.

## Release version

The application version has one source of truth and several validated copies. Increment all of them together on every release.

- `crates/telaviva-core/Cargo.toml` `version`: source of truth. The Windows zip name is derived from it, and the Windows About page reads it at runtime through the core (`tv_core_version`).
- `crates/telaviva-inference/Cargo.toml` `version`: validated; must equal the core version.
- `native/macos/TelaViva/Info.plist` `CFBundleShortVersionString` and `CFBundleVersion`: validated; must equal the core version. The macOS About page displays `CFBundleShortVersionString`.
- `native/windows/TelaViva/app.manifest` `assemblyIdentity` `version`: validated, written in the four-part form `X.Y.Z.0`.
- `Cargo.lock`: not edited by hand. Cargo updates it after the bump; commit it with the change.
- `CHANGELOG.md`: rename the `Unreleased` heading to the new version heading and open a new empty `Unreleased` section.

`cargo test -p telaviva-core --test version_consistency` fails when the validated copies disagree with the core version.

Release procedure:

1. Bump the versions above and run the tests.
2. Commit the change, then create the tag `vX.Y.Z` on that commit.
3. Publish a GitHub Release for the tag that is neither a draft nor a pre-release. The in-app update check reads `releases/latest` of `ksfreitas/telaviva-oled`, and it ignores drafts and pre-releases, so a release published any other way is not offered as an update.
