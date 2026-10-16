# Bug: LG UltraFine brightness writes are silently ignored

## Bug Description

At the user's office, TelaViva discovers the connected LG ULTRAFINE and can show its brightness, but moving the brightness slider does not change the panel. DisplayBuddy can control the same connection. Read-only TelaViva diagnostics also intermittently mark brightness or volume unsupported even though the device responds to correctly sequenced DDC transactions.

Expected behavior: apply a manual brightness change and report success only after the monitor confirms the requested raw VCP value. Actual behavior: a single Set VCP transmission returns IOAV success without changing brightness; unreliable Get VCP requests can also prevent the write entirely.

## Problem Statement

Make the existing Apple Silicon IOAV transport reliably transact with this monitor, without changing monitor identity, Rust policy, slider ranges, or the pending keyboard-routing feature.

## Solution Statement

Extract the existing packet construction and bounded retries into a small injectable DDC transaction helper. Send each request twice, waiting 10 ms before each transmission, and allow 60 ms for Get VCP replies. Read exactly the 11-byte response at IOAV read offset zero, retaining both existing request checksum variants. Validate reply framing, status, feature code, checksum, and range. Keep writes serialized on the existing DDC queue and require post-write readback to equal the requested raw value before reporting success.

## Steps to Reproduce

1. Connect the office LG ULTRAFINE (vendor 7789, product 30600) to the Apple Silicon Mac.
2. Launch the original application and attempt to move its brightness slider. The monitor can be listed while the brightness does not change.
3. Run the original executable with `--diagnostics`; the transport is found, but VCP reads can intermittently be unsupported.
4. With a fresh physical brightness baseline of 41/100, send the original single Set VCP command requesting 40. IOAV returns success but physical readback remains 41.
5. Send the identical Set VCP packet twice with 10 ms spacing and read back: brightness becomes 40. Restore 41 using the duplicated sequence and confirm readback 41.

## Root Cause Analysis

The transport sent each Get/Set VCP packet only once. IOAV returning zero confirms submission to the OS transport; it is not an acknowledgment that the monitor applied the command. On the connected LG, a controlled hardware test reproduced `single_write_io=true observed=41` for target 40, followed by `duplicate_write_io=true observed=40`. Restoration returned `restored=true readback=41`. Duplicated Get VCP requests also produced correct alternating brightness/volume responses in all 16 sampled transactions, whereas single requests produced stale, wrong-feature, or invalid replies even with longer waits.

A second bug hid ignored writes: the adapter reported any valid post-write reading as success without comparing it to the requested value. The old read also requested 12 bytes at offset 0x51, despite validating only 11. The maintained upstream MonitorControl implementation uses read offset zero, 11 bytes, and repeated transmissions. Both read offsets worked in the successful duplicated hardware probe, so offset alone is not established as the cause on this LG.

## Relevant Files

- `native/macos/TelaViva/MacDDCController.swift`: existing serialized IOAV adapter, action execution, and hardware-result reporting.
- `packaging/macos/build-app.sh`: explicit native source list.
- `native/macos/TelaViva.xcodeproj/project.pbxproj`: matching Xcode source list.
- `.github/workflows/macos.yml`: deterministic native regression checks.
- `README.md` and `CHANGELOG.md`: current hardware evidence and fix description.

### New Files

- `native/macos/TelaViva/DDCTransaction.swift`: injectable packet sequencing and validation.
- `native/macos/Tests/DDCTransactionTests.swift`: deterministic transaction regressions with no physical writes.
- `tools/tests/test-ddc-transaction.sh`: standalone native test runner.
- `specs/archived/external-monitor-hardware-keys-and-fluid-controls/lg-ultrafine-ddc-brightness-writes.md`: this plan and validation evidence.

## Step by Step Tasks

### 1. Establish the connected monitor failure

- [x] Inspect discovery and Get/Set VCP execution, preserving existing uncommitted work.
- [x] Compare transport sequencing with maintained open-source implementations.
- [x] Reproduce a one-point hardware write failure and successful duplicated write; restore the original brightness.

### 2. Correct transaction sequencing and write acknowledgment

- [x] Extract only framing/retry logic into an injectable helper; preserve existing retry bounds.
- [x] Duplicate requests with 10 ms pre-transmission waits and validate 11-byte replies.
- [x] Use separate IOAV read/write offsets and reject mismatched write readback.
- [x] Wire the helper into packaging and the Xcode target.

### 3. Cover the regression

- [x] Test a fake device that ignores the first write and applies the second.
- [x] Cover wide values, both read checksums, transport failures, stale/wrong-feature replies, checksum/status/range failures, and bounded retries.
- [x] Add the deterministic native test runner to macOS CI.

### 4. Run Validation Commands

- [x] Run the commands below, including application-path write/readback/restoration on the connected LG.
- [x] Build and verify the app bundle, then reopen the corrected application.

## Validation Commands

```sh
tools/tests/test-ddc-transaction.sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-camera-lifecycle.sh
cargo test --workspace --all-targets --locked
plutil -lint native/macos/TelaViva.xcodeproj/project.pbxproj
./scripts/build-macos.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --diagnostics
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
lipo 'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' -verify_arch arm64
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
git diff --check
```

Hardware reproduction uses temporary test harnesses outside the repository. The before/after test must read and retain the original physical brightness, request one raw point of change, confirm it through physical readback, restore the original value even on failure, and verify restoration. The application-path harness compiles the production `CoreBridge`, `MacDDCController`, and `DDCTransaction` sources and submits the same `set_brightness` command used by the slider. Do not test volume or other VCP features as part of this brightness fix.

## Notes

- No new dependency, background timer, or polling loop is introduced. Additional I/O and waits occur only during requested DDC transactions.
- The pending hardware-key implementation is preexisting uncommitted work and must remain intact. No commit or push is part of this bug fix request.
- This confirms only the connected LG/connection; it does not establish universal monitor or dock compatibility.
- Upstream reference: [MonitorControl Apple Silicon DDC transport](https://github.com/MonitorControl/MonitorControl/blob/main/MonitorControl/Support/Arm64DDC.swift).

### Validation results (2026-10-16)

- All 38 Rust tests passed, including the pinned-model CPU inference test.
- DDC transaction, hardware-key router/coordinator, camera lifecycle, and camera localization native suites passed.
- Native release packaging, Xcode project plist validation, arm64 verification, ABI smoke test, strict ad-hoc signature verification, and `git diff --check` passed.
- Corrected packaged diagnostics read brightness 41/100 and volume 0/100 on the LG. Volume was read only; no volume write was tested.
- The temporary harness compiled the production bridge/controller/helper and linked the current release Rust core. It submitted the same manual brightness command as the popup and produced:

```text
app_path original=41.0 target=40.0
app_path write_confirmed=true raw=40.0 status=ready
app_path restored=true raw=41.0
app_path fresh_physical_readback=41.0
```

- Reopened `build/macos/TelaViva OLED.app` and verified its process was running. No commit or push was performed.
- Exact readback equality intentionally rejects values that the monitor clamps or quantizes to a different raw value. Broader monitor compatibility remains unverified.

### Follow-up: test gaps closed (2026-10-16)

- `DDCTransactionTests.staleReplyThenCorrectReplyOnOuterRetry` covers a well-formed reply for the wrong feature code on attempt 0, followed by the correct reply on the outer retry. The scenario takes 3 reads and 6 writes and returns the correct value.
- A reply for the same feature code with a different current or maximum value cannot be distinguished from a real reply, because the read has no expected value. The feature-code and checksum checks are the stale-reply defense. This limit is accepted.
