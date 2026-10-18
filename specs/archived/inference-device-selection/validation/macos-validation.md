# macOS implementation validation

- Date: 2026-10-18
- Machine: Apple M4 Pro MacBook Pro, 48 GB RAM
- OS build: macOS 27.0.1 (26A434)
- Driver: Apple system CoreML; no separate driver version applies
- Build: working tree based on `7247693`, application version 0.1.1
- Scope: macOS completion requested by the owner; the existing Windows implementation was preserved

## Automated checks

| Command/check | Result |
|---|---|
| `cargo test --workspace --locked` | 239 tests passed, including version consistency, settings/C ABI contracts, inference mapping and the bundled runtime/model smoke test |
| `cargo clippy --workspace --all-targets --locked -- -D warnings` | passed |
| `cargo fmt --all -- --check` | passed |
| All 15 `tools/tests/*.sh` scripts | passed; changed CoreBridge, localization and inference scripts rerun after final edits |
| `scripts/build-macos.sh` | passed; built and ad-hoc signed the Apple Silicon app with ORT 1.23.0, the model and TVSF self-test frame |
| Packaged executable `--smoke-test` | exit 0 |
| `codesign --verify --deep --strict` on the app | passed |
| `lipo` arm64 verification | passed |
| `plutil -lint native/macos/TelaViva.xcodeproj/project.pbxproj` | passed |
| `cargo build --workspace --locked`, then Xcode Debug build | passed, including the self-test frame generation phase |
| `git diff --check` | passed |

The first direct Xcode Debug attempts linked old local Rust archives and failed with missing symbols. Rebuilding those archives with Cargo resolved the link failures. The one-command build uses the explicit ARM64 release libraries and passed independently. The SDK emits an existing Swift concurrency warning for `MenuBarController`'s settings-window close observer, and Xcode reports the existing traditional headermap warning; neither is a new inference failure.

## CoreML and fallback evidence

- [Runtime spike](macos-ort-spike.md): diagnosed ORT 1.22.0's exit-time mutex abort on CPU and CoreML; official 1.23.0 loads with API 20 and exits cleanly using the unchanged bindings.
- [Production C ABI probe](macos-coreml-device-probe.md): CPU reference and ALL/Neural Engine/GPU candidates return `present` on the packaged-source frame; each candidate attaches provider 1 and answers within 125 ms.
- [Production detector host](macos-coreml.md): release-library correctness/latency, forced candidate failure, same-frame CPU retry after a runtime error, and retained CPU fallback through idle recreation. Automatic was enabled after review.
- Durable Swift tests use the same injected detector host as `PresenceCamera` and cover selection changes, lazy creation, session failure, registration mismatch, self-test mismatch/slow failure, runtime retry, retained successful validation and retained CPU fallback.

## UI checks

An isolated AppKit/SwiftUI harness rendered the production Advanced view without instantiating the app's controllers, core, camera or network checker. All six combinations of en-US/pt-BR and normal/failed/unavailable states were rendered and inspected. The picker retains a foreign-platform GPU id as an unavailable selection, accelerator entries are labeled preferences, the active label is localized and the notices wrap within the card. The final copy reflects validated CoreML Automatic with CPU fallback.

Transient render artifacts: `/tmp/telaviva-advanced-ui.5aX7sa/screenshots/`. These renders verify the view's layout and status presentation, not interaction with a live camera or the running user's settings.

## Acceptance limits

- No live webcam, camera-on timing, battery, energy, thermal or idle-resource comparison was performed. Existing camera scheduling was preserved; no measured resource/energy improvement is claimed.
- CoreML compute units are preferences. Provider registration and correct output do not prove individual operator placement on the GPU or Neural Engine.
- Hardware results cover this M4 Pro and OS build only. Each accelerated preference session validates locally and falls back to CPU on failure.
- Windows builds/tests and UI Automation were not rerun in this macOS-only completion. The pre-existing T030 UI acceptance and gaps listed in `windows-auto-igpu.md` remain open.
- No tag, GitHub Release, distribution signing or notarization was performed.

## Post-finalization diagnostics and version audit

The requested `fix-idea-diagnostics` pass covered all 26 modified/added non-Markdown, non-SQL files. IntelliJ inspections were queried with weak warnings enabled, without opening editor tabs or changing focus. Six unnecessary `std::ffi::` prefixes were removed from the header contract test, using its existing parent-module import. A UTF-8 BOM made the PowerShell runtime fetcher's encoding explicit and cleared IntelliJ's encoding error. Both edited files returned no findings on recheck.

Two weak duplicate-fragment warnings remain in `crates/telaviva-inference/src/lib.rs` (production interpolation and the independent test reference). The test deliberately preserves the earlier algorithm as a differential oracle; sharing implementation merely to remove the warning would weaken that verification. They were neither suppressed nor reported as fixed.

Headless SonarLint completed successfully and reported the macOS workflow YAML clean; it skipped the other 25 files because Rust, Swift, C headers, TOML, pbxproj, shell, Python, PowerShell and notice text are unsupported by this helper. The TypeScript service completed with zero applicable TS/JS findings. Both headless sources were repeated on the corrected files, which they skip by language. Targeted inference tests, strict Clippy, formatting and whitespace checks passed after the Rust cleanup.

The application version audit confirmed core and inference Cargo packages `0.1.1`, both macOS bundle version keys `0.1.1`, Windows manifest `0.1.1.0` and both application lockfile entries `0.1.1`. `cargo test -p telaviva-core --test version_consistency --locked` passed all three checks. The source version does not establish that a tag or GitHub Release was published.

The documentation audit updated README status to source version 0.1.1, separated Windows iGPU and macOS CoreML behavior, clarified the bundled-frame correctness limit, and corrected the architecture summary. The macOS feature/runtime notes were consolidated under the 0.1.1 changelog entry with an empty Unreleased section, removing the obsolete claim that macOS lacked device selection. Local documentation links were checked.
