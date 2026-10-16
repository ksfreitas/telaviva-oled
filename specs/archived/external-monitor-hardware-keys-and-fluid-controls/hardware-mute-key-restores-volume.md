# Chore: Mute key mutes and restores the monitor volume when it is the sound output

## Chore Description

Physical acceptance of the sound-output volume routing worked with headphones and with the monitor, but the keyboard mute key was not handled for the monitor. Add it: while an external monitor is the Mac's default sound output (same eligibility as the volume keys), the mute key sets that monitor's volume to 0 and remembers the previous level; the next press restores it. With any other output the key passes through to macOS, so headphones keep their normal mute.

Semantics, all under the existing router lock and bounded intent path:

- Eligibility, latching, and pass-through are identical to volume: the target comes from `HardwareKeyPolicy.volumeTarget(defaultOutputUID:displays:)`, the pointer is ignored, the initial decision is latched until key-up, and a handled key-up is consumed. Only the exact no-modifier combination is handled; the repeat events of a held mute key are consumed without toggling again.
- Mute (current level above 0, or no remembered level): remember the current level (latest intent value if one is pending, else the observed value), then reserve a write of 0 through the normal intent queue, so it is coalesced, serialized with DDC, generation-checked, and confirmed by the strict readback.
- Restore (the remembered level exists and the current level is 0): reserve a write of the remembered level and forget it.
- Volume at 0 with nothing remembered: consume the key without a write.
- Volume-up while muted: restore the remembered level, then apply the normal step up from it. Volume-down while muted: consume without a write and keep the remembered level.
- The remembered level is kept only in memory, per monitor ID, connection generation, and feature. Discard it when the volume is no longer 0 at the next key press (changed by the slider, the monitor's own controls, or any other write), whenever the router clears its intents (disable, shutdown), on disconnect, generation change, sleep, or when the monitor stops being eligible.
- The popup slider shows 0 while muted through the existing external-intent path. No new setting, no Rust, ABI, or settings schema change, no new VCP code, no dependency.

## Relevant Files

- `native/macos/TelaViva/HardwareKeyRouter.swift`: event decoding (`NX_KEYTYPE_MUTE` from the public SDK constants), `HardwareKeySequence.process` latching, and the router's intent state and lock.
- `native/macos/Tests/HardwareKeyRouterTests.swift`: deterministic decode, latch, and toggle tests.
- `native/macos/TelaViva/Localization.swift`: the `hardwareKeys.help` strings in both languages currently say volume only.
- `README.md` (keyboard paragraph says mute keeps normal behavior), `api.md` (keyboard intent sentence), `CHANGELOG.md`, `specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys/macos-acceptance.md`, `specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys.md` (the original text says the default scope excludes mute) and `specs/archived/external-monitor-hardware-keys-and-fluid-controls/hardware-volume-keys-follow-sound-output.md`: documentation.

## Step by Step Tasks

### 1. Decode and route the mute key

- Decode `NX_KEYTYPE_MUTE` as a mute action, down and up phases, with the existing modifier rules (no modifiers only; Shift+Option is not a mute combination and passes through).
- Route it through `volumeTarget`, latch the sequence, and consume repeats and key-up as described above.

### 2. Implement the mute memory

- Keep a bounded per-target remembered level in the router, guarded by the existing lock, cleared with the intents and on every ineligibility described above. The event callback must not call CoreAudio, DDC, or the core.

### 3. Tests

- Decode of the mute payload (down, up, repeat; modifiers; ordinary keys unaffected).
- Mute then restore returns the original level; a pending intent value is remembered instead of a stale observed value; volume at 0 with nothing remembered consumes without writing.
- Repeats of a held mute key do not toggle; key-up is consumed; a pass-through mute (headphones, unmatched UID, unsupported volume, sleeping or stale monitor) passes the whole sequence.
- Volume-up while muted restores then steps; volume-down while muted does not write; an external change of the level discards the remembered value.
- Output change mid-hold, disable, and shutdown clear the remembered level; brightness is unaffected.

### 4. Text and documentation

- Update the help strings, README, api.md, CHANGELOG, the acceptance checklist, and the two earlier specs (a short follow-up note; do not rewrite their completed tasks).

### 5. Run Validation Commands

## Validation Commands

```sh
tools/tests/test-hardware-key-router.sh
tools/tests/test-default-audio-output.sh
tools/tests/test-manual-monitor-controls.sh
tools/tests/test-core-bridge.sh
tools/tests/test-ddc-transaction.sh
tools/tests/test-camera-lifecycle.sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
plutil -lint native/macos/TelaViva.xcodeproj/project.pbxproj
./scripts/build-macos.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
lipo 'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' -verify_arch arm64
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
git diff --check
```

## Notes

- This emulates mute with volume 0 and restore instead of the monitor's own mute (VCP 0x8D), which varies between monitors and has not been probed on the connected LG. It works wherever the verified volume path works, but the monitor's on-screen display will show a volume of 0 rather than a mute icon. A native mute could be added later as a separate chore after a read-only capability probe.
- The remembered level is not persisted: after an app restart a monitor left at volume 0 stays at 0 until the user raises it.
- Physical acceptance (person at the keyboard): LG as sound output, mute then restore; mute then volume-up; mute then change the volume on the monitor's own controls, then press mute; headphones as output, where mute must pass through to macOS and not change the monitor; mute held down must toggle once.

### Implementation results (2026-10-16)

- Mute is decoded from `NX_KEYTYPE_MUTE` with no modifiers only, routed through `volumeTarget`, and latched per key sequence. The remembered level is kept per monitor and bounded at 128. It is cleared on disable, on shutdown, when the monitor stops being eligible, when the level is changed externally, and, added after an audit, on any default sound output change (`setDefaultOutput`).
- Audit finding: the existing decode test still asserted that mute decodes to nil, and that assertion aborted the whole hardware-key suite. The test was updated, and `muteBehavior` plus a volume stale-generation case were added to `native/macos/Tests/HardwareKeyRouterTests.swift`.
- The new coverage includes decode; mute and restore; repeats; volume-up and volume-down while muted; zero with nothing remembered; external change; output change; pass-through cases; held-mute cancellation; brightness independence; and disallowed modifiers.
- Automated validation passed on 2026-10-16: the hardware-key router, manual-monitor-controls, core-bridge, ddc-transaction, camera-lifecycle, default-audio-output, and media-key-probe native suites; `./scripts/build-macos.sh`; `--smoke-test` (exit 0); arm64 `lipo` verification; strict `codesign` verification; and `git diff --check`. Cargo checks were not rerun because the Rust workspace did not change in this step.
- Physical acceptance has not been run. Rows 30-34 of `specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys/macos-acceptance.md` remain "Not run".
