# Third-party license records

`DEPENDENCIES.md` inventories Cargo registry dependencies and their declared open-source licenses. Copies of available crate license/notice files are retained under `rust/`. `inference/` contains YuNet and ONNX Runtime licensing, third-party notices and source provenance. The DDC adapter's m1ddc attribution is retained at `native/macos/DDC-LICENSE`.

The macOS packaging script copies these records and the DDC attribution into `Contents/Resources/ThirdPartyLicenses/`. It verifies the pinned model/runtime SHA-256 hashes before signing the local bundle. The app does not download a model or runtime at execution time. Runtime paths are absolute bundle paths; relative working-directory loading is rejected by the inference ABI.

The positive inference test fixture has its own CC0 provenance record beside the fixture under `crates/telaviva-inference/tests/fixtures/`. It is a development asset and is not copied into the application bundle.

Native Apple system frameworks and development tools are platform facilities, not redistributed dependencies. TelaViva OLED itself is licensed under the MIT License in the repository root. Third-party records above document separate dependencies and assets; the project's license does not replace their notices or terms.
