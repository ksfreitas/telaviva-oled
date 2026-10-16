# Chore: Route macOS hardware media keys to the external monitor under the pointer

## Chore Description

Add an opt-in macOS feature that routes physical keyboard brightness-up/down and volume-up/down presses to the supported external monitor under the mouse pointer. It applies only when the pointer hits one unambiguous, connected external display whose corresponding feature is supported and whose identity is stable. Other events and ineligible targets pass through to macOS unchanged. When the feature handles a key, update the monitor through the existing Rust core manual-control commands and native DDC action queue so existing generation, sleep, ownership, and serialization rules remain in force.

The event callback must stay lightweight: resolve an immutable, thread-safe eligibility snapshot and reserve a bounded work slot before accepting a press, without issuing DDC calls or blocking on the core. Latch the initial press decision, monitor ID, connection generation, and key identity until release while the tap remains active. Key repeats for that hold keep the original target even if the pointer moves; the next distinct press resolves the pointer again. A key-up applies no delta, but is consumed when its initial press was handled and the tap is still active. If the initial press was not handled, forward its down, repeats, and up unchanged. If the target disconnects or changes generation mid-hold, cancel future writes for that held key but keep consuming its already-handled repeats and key-up while the tap remains active; never inject a stale event back to macOS or let a held key fall back to the built-in display. If permission is revoked or the tap becomes unavailable, drop queued intents and latches; pass through events the tap can no longer intercept, and never synthesize or reinject keys. Serialize writes with the existing native DDC queue, coalesce pending repeat steps to the latest desired normalized value per monitor and feature, and preserve accumulated increments while a hardware write is in flight. Consume an eligible key at the callback after its target is validated and bounded intent slot reserved, not after hardware acknowledgement. Accept only the no-modifier combination for normal steps and the exact Shift+Option combination for fine steps; pass other modifier combinations through intact so system shortcuts continue to work. Resolve Fn-generated media keys by their media-event type, not by treating ordinary F1/F2/F11/F12 function-key events as media keys. The default scope excludes mute.

Use normalized 0–100 values and a default 6.25 percentage-point step (16 steps across the range); support a finer 1.5625-point step while Shift+Option is held if that modifier combination is reliable for the selected macOS event API. Clamp at each feature's range. At 0 or 100, consume an otherwise eligible owned key event without issuing a redundant write, so macOS does not apply a system-level action to the built-in display. Keep this work macOS-only; Windows implementation and validation remain deferred.

## Relevant Files

- `native/macos/TelaViva/MenuBarController.swift` — owns application startup, settings, and shutdown; install and remove the hardware-key listener with this lifecycle.
- `native/macos/TelaViva/CoreBridge.swift` — decodes settings/monitor snapshots and submits the existing `set_brightness` and `set_volume` core commands; current `submit` discards the core result, so provide an observable accepted/rejected completion for worker-submitted keyboard intent.
- `native/macos/TelaViva/MacDDCController.swift` — also publish a read-only descriptor containing the shared `CGDirectDisplayID`, monitor ID, generation, display bounds, identity/support/sleep state; snapshot/API fields do not currently include the CoreGraphics display ID, so do not guess or correlate by array order/name.
- `crates/telaviva-core/src/core.rs` — `manual_set` validates stable identity, support, awake state, and normalized range, updates brightness ownership where applicable, and enqueues standard hardware actions; `update_settings` parses the new opt-in setting.
- `crates/telaviva-core/src/settings.rs` — settings defaults/serde validation; add a default-false field while retaining schema version 1 and test missing-field compatibility. This preserves existing files and current core settings behavior; it does not guarantee older binaries accept settings serialized with a new field.
- `crates/telaviva-core/tests/core_contract.rs` and `crates/telaviva-core/tests/ffi_contract.rs` — cover default/missing-field loading, settings updates through JSON, persistence, and the FFI snapshot contract.
- `native/macos/TelaViva/SettingsView.swift` — expose the opt-in setting and listener/permission status; do not request event access at first launch, and activate the listener only after the user enables the setting.
- `native/macos/TelaViva/Localization.swift` — add English and Brazilian Portuguese toggle/help/status copy; the two `InfoPlist.strings` files are only needed if a documented system permission-purpose string is required.
- `native/macos/TelaViva.xcodeproj/project.pbxproj` — register any new Swift source and test source in the current explicit source/build phases.
- `packaging/macos/build-app.sh` — add every new Swift production helper to its explicit `SOURCES` array.
- `.github/workflows/macos.yml` — run the focused hardware-key helper suite in the native macOS CI job and retain the existing architecture/signature checks.
- `tools/tests/test-camera-lifecycle.sh` — existing lightweight Swift test runner; use its direct `swiftc` pattern for the focused router and coordinator test suite.
- `native/macos/Tests/CameraLifecycleTests.swift` and `native/macos/Tests/CameraLocalizationTests.swift` — existing examples of standalone native helper tests and localization checks.
- `scripts/build-macos.sh` — run the existing Apple Silicon native build path during validation.
- `specs/archived/external-monitor-control-and-oled-presence-protection/validation/monitor-matrix.md` — existing monitor/connection validation context; hardware-key acceptance should reuse its connection-specific support caveats.

### New Files

- `native/macos/TelaViva/HardwareKeyRouter.swift` — isolate SDK-backed event classification, active tap lifecycle, held-key decision latching, display hit-testing, and step calculation from menu UI and DDC transport code.
- `native/macos/Tests/HardwareKeyRouterTests.swift` — meaningful deterministic tests for event classification, normalized stepping/clamping, sequence latching/forwarding/consumption, stale-generation cancellation, rapid-repeat accumulation, manual-control rebasing, and target-selection policy with synthetic inputs and no physical keypresses or DDC writes.
- `tools/tests/test-hardware-key-router.sh` — compile and run the pure/helper-level native tests using the repository's existing `swiftc` test pattern.
- `specs/archived/external-monitor-hardware-keys-and-fluid-controls/external-monitor-hardware-keys/macos-acceptance.md` — physical acceptance checklist, explicitly not run.

## Step by Step Tasks

### 1. [x] Confirm the macOS event and permission contract

Use an active `CGEvent.tapCreate` session event tap (`.cgSessionEventTap`, `.headInsertEventTap`, `.defaultTap`) if SDK/device validation confirms it receives the required events. An `NSEvent` global monitor is insufficient because it cannot modify or suppress delivery. Follow Apple's Accessibility-permission requirement for modifying events; distinguish it from passive input monitoring and do not promise that permission is unnecessary. If permission is absent or the tap cannot be created, preserve the setting and pass every event through. Validate public SDK constants for `NX_SYSDEFINED` / auxiliary-control-button events and `NX_KEYTYPE_SOUND_UP`, `NX_KEYTYPE_SOUND_DOWN`, `NX_KEYTYPE_BRIGHTNESS_UP`, and `NX_KEYTYPE_BRIGHTNESS_DOWN` on real keyboards before relying on their event payload layout. Use SDK constants instead of magic key codes; avoid root/HID taps, event synthesis, and private CoreGraphics APIs. Record the exact permission state the app can observe and the route to System Settings. Treat API accessibility and permissions as a macOS prerequisite, not as a new redistributed dependency. Do not fall back to remapping normal F1/F2/F11/F12 function-key presses.

### 2. [x] Add a backward-compatible opt-in setting

Add a boolean `hardware_keys_enabled` setting with a default of `false` to Rust settings defaults/serde and the Swift `CoreSettings` decoder/encoder. Keep schema version 1 because a defaulted field preserves existing settings files; explicitly avoid claiming compatibility with older binaries, which may reject a newly serialized unknown field. Extend `update_settings` parsing and Rust core/FFI tests. Add a localized toggle/help/status in Settings. Enabling it is the first point at which to request any required event permission; present the operating-system permission path only after the user enables the feature, with no prompt at first launch or separate Codex approval step. If permission is absent or later revoked, show the status and leave the listener inactive; keep the toggle choice so the user can grant access and retry.

### 3. [x] Implement pure event and target decisions

In `HardwareKeyRouter.swift`, separate event decoding and target selection from listener installation. Classify only brightness up/down and volume up/down media-key events; ignore mute, unrelated keys, and ordinary function-key events. Confirm on supported keyboards whether a media event's location reports the current pointer; if not, sample current cursor position with public `CGEvent(source: nil)?.location` on initial key-down. Compare with `CGDisplayBounds` in the same CoreGraphics coordinate space; do not convert through AppKit `NSScreen` coordinates. Resolve to exactly one current external display and retain its shared `CGDirectDisplayID`, stable monitor ID, and connection generation. Return pass-through for no hit, overlap/ambiguity, built-in display, unsupported feature, unstable identity, sleeping display, disconnected/stale generation, inactive app setting, or unavailable permission. Never choose a first/main display as fallback. Latch the initial decision by key identity for the full down/repeat/up sequence: handled key-up is consumed without a delta; an unhandled initial down means the full sequence passes through. Pointer movement during a hold never retargets repeats. If target generation changes mid-hold, cancel further writes but keep consuming the latched handled sequence until key-up.

Apply 6.25 percentage points per no-modifier key-down/repeat and 1.5625 for the exact Shift+Option combination if supported reliably by the selected event API; otherwise retain the normal increment and record the modifier limitation in the setting help. Pass every other modifier combination through intact. Clamp to 0–100. At a bound, consume an otherwise eligible press/repeat without writing, preventing macOS from changing the built-in display. Provide deterministic unit coverage for direction, feature, modifier combinations, boundaries, handled/unhandled key-up and full sequence behavior, pointer movement during a hold, stale-generation cancellation, monitor hit-testing, and ineligible-target pass-through.

### 4. [x] Connect accepted steps to the existing core and DDC queue

Publish an immutable eligibility snapshot from the core snapshot/DDC discovery flow for use by the event callback, with thread-safe replacement and no synchronous core access from that callback. Because `CoreBridge.VcpValue` currently exposes normalized observed value and raw `desired` is not decoded, add an explicit coordinator intent cache or extend the bridge with a well-defined normalized desired value. Hardware acknowledgements must never reset a newer accumulated keyboard intent. When a popup slider or another app control submits a manual value for the same monitor/feature, rebase the keyboard intent cache from that command so the next key starts from the user's latest manual choice. `CoreBridge.submit` currently discards core submission status; expose an accepted/rejected completion to the DDC worker so rejected requests clear or rebase pending intent from a fresh snapshot. The event is already consumed after reservation and must never be reinjected into the global event stream after a submission failure. For an accepted event, reserve one bounded per-target intent slot, calculate the next normalized target from its latest intent, and enqueue it through the existing serialized DDC/core path. Invoke the standard `set_brightness` or `set_volume` command so core-side stable-identity, feature-support, display-sleep, normalized-range, action-generation, and brightness manual-override behavior stays authoritative.

Bound pending keyboard work by monitor and feature, reserving capacity before consuming the initial down event. Coalesce rapid key repeats to the latest accumulated target while a write is in flight, without dropping increments that arrived during that write. Core snapshots and hardware acknowledgements may update observed state but cannot roll back a newer coordinator intent. Revalidate monitor ID and connection generation immediately before submitting/writing; if the device changed, discard pending writes and pass future presses through until a fresh descriptor is available. Already-handled sequences remain consumed through release, even if worker submission fails; clear/rebase failed intent from a fresh snapshot and never synthesize or reinject a keyboard event. Do not run DDC I/O, VCP readback, core calls, or unbounded dispatch work on the event callback. A manual app control to the same feature rebases pending keyboard intent.

### 5. [x] Manage listener lifecycle and permission status

Start the listener only while `hardware_keys_enabled` is true and permission is available. Disabling the setting, losing permission, or shutting down drops pending keyboard intents and clears held-key state. Paired-event consumption is guaranteed only while the active filter remains installed. On tap timeout or user-input disable notifications, let the disabled event pass through and permit only a bounded re-enable attempt while the persisted setting and permission state still authorize routing; do not poll, busy-loop, or install a separate global mouse monitor. Stop it when the setting is disabled, permission is revoked, app shutdown begins, or listener creation fails. Ensure listener teardown is idempotent and does not race callbacks or DDC shutdown; drop queued keyboard intents and held-key latches when tearing down. Expose a concise localized state for disabled, enabled, permission required/revoked, and listener unavailable. Verify the user can use macOS volume/brightness controls normally whenever the feature is off or an event is ineligible.

### 6. [x] Add native tests and physical acceptance checklist

Run meaningful focused helper/coordinator tests without requiring an actual keyboard event tap or monitor. Test key sequence latching while the tap is active, full-sequence pass-through/consumption, modifier pass-through, rapid-repeat accumulation during an in-flight acknowledgement, manual-control rebasing, submission rejection recovery without event reinjection, boundary key consumption with no write, stale-generation cancellation, and intent/latch clearing on teardown. Test brightness-key commands during active presence and screensaver protection so they retain the shared core's established manual-override and ownership policy instead of bypassing it. Register production files in both the Xcode project and packaging script; add the focused test runner to the macOS CI workflow. Build/package the app on Apple Silicon and run the documented smoke executable plus architecture/signature checks. Manually verify on supported physical hardware that a keypress targets the external monitor beneath the pointer across screen edges and mixed-DPI layouts; test supported/unsupported brightness and volume independently, key repeat, fine-step modifier behavior, bounds, a built-in display, pointer gaps, sleep/wake, hot unplug/replug, permission denied/revoked, feature off, app shutdown, ordinary function keys, and pass-through when a destination or modifier combination cannot safely be resolved. Confirm there are no writes to other monitors and no duplicate OS-level action for a consumed key. Exercise brightness keys during presence dimming and screensaver protection to verify the existing manual-override and ownership rules preserve the user adjustment.

### 7. [x] Run Validation Commands

Run formatting, focused Swift helper tests, Rust workspace tests/lint covering settings/core/FFI compatibility, the existing camera lifecycle tests, and the Apple Silicon app build. Run the documented smoke executable, `lipo` architecture check, and `codesign` verification. Record physical acceptance separately from automated results; automated tests and a successful build do not establish hardware compatibility.

## Validation Commands

```sh
tools/tests/test-hardware-key-router.sh
cargo fmt --all -- --check
cargo test --workspace --all-targets --locked -- --skip bundled_runtime_executes_pinned_yunet_model_on_cpu
cargo clippy --workspace --all-targets --locked -- -D warnings
tools/tests/test-camera-lifecycle.sh
./scripts/build-macos.sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
lipo 'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' -verify_arch arm64
codesign --verify --deep --strict 'build/macos/TelaViva OLED.app'
```

The ONNX Runtime must already be present for `./scripts/build-macos.sh` to avoid its first-build fetch path. The smoke test is documented in `packaging/macos/README.md` and runs without opening the UI or camera. On a physical Apple Silicon Mac, perform the manual acceptance matrix from Step 6 and record OS version, keyboard type, monitor model, connection path, permission status, and outcomes. Do not use `curl` for validation.

## Notes

Implementation follows this standalone fast-chore plan in place. Foundation completed: Rust settings preserve schema-1 defaults, optional connection-generation guards reject stale manual commands, and bounded-queue rejection leaves manual intent untouched. Core implementation and native integration are complete. Local automated validation and Apple Silicon packaging passed. Physical key/TCC/DDC acceptance remains pending and is recorded separately.

- Observe only the required media-event mask. Never retain or log ordinary keys, typed text, or full event payloads.
- The feature is opt-in and defaults off. This avoids requesting event-monitoring access before the user deliberately enables hardware-key routing.
- Default scope excludes mute; add mute only if requested separately.
- Do not add an on-screen display as part of this chore. Only no-modifier and exact Shift+Option combinations are handled; leave Command, Option-only, Shift-only, and other combinations to macOS.
- Shift+Option fine stepping is a proposed usability detail and must be confirmed against the chosen event API; if unavailable, retain the standard step and state that clearly.
- The 6.25-point normal step corresponds to sixteen increments from minimum to maximum. Actual DDC values may be quantized by monitor-specific VCP ranges; use the existing normalized core mapping and report hardware limitations without promising exact physical increments.
- Validate whether the media event's `CGEvent.location` is the current pointer location on supported keyboards. If not, sample the current cursor through the public `CGEvent(source: nil)?.location` API on the initial key-down and compare directly with `CGDisplayBounds` in the same CoreGraphics coordinate space. Do not add a global mouse monitor or guess AppKit coordinate flips. Validate displays left or above the primary display and mixed scaling; ambiguous or no-hit geometry must pass the key sequence through.
- Do not add an event-tap, hotkey, or other redistributed package. Capture API choice, TCC scope, and permission UX must follow the official macOS documentation confirmed during implementation.
- Apple documents that a global AppKit event monitor cannot suppress delivery in [`NSEvent.addGlobalMonitorForEvents`](https://developer.apple.com/documentation/appkit/nsevent/addglobalmonitorforevents(matching:handler:)); Apple's [WWDC 2019 event-handling session](https://developer.apple.com/videos/play/wwdc2019/701/) states that modifying events requires Accessibility permission and distinguishes passive monitoring. Use the public macOS SDK headers for the `NX_SYSDEFINED`, auxiliary-control-button, and `NX_KEYTYPE_*` constants. Verify real keyboard media-event payloads because hardware and event delivery may vary.
- Do not call a successful synthetic test or build proof of keyboard capture, TCC behavior, DDC physical writes, or broad monitor support. Windows work remains deferred.

### Implementation phase results

- [x] Foundation: default-false settings, strict optional connection generations, queue-full transactional behavior, and compatibility/persistence tests.
- [x] Core implementation: public SDK media-key decoder, latched key sequences, normalized stepping, bounded intent/submit queues, and generation/authorization checks. Ordinary manual controls invalidate older keyboard handoffs before their final main-thread submission.
- [x] Native integration: localized opt-in controls and Accessibility status, all-display geometry from the existing adapter mapping, explicit source/CI wiring, and permanent shutdown guards. Event callbacks hold only a short local state lock; no DDC or Rust call holds that lock.
- [x] Automated validation: 38 Rust tests (29 core, 4 FFI, 4 inference unit, 1 pinned-model CPU integration), strict clippy, formatting, hardware-key/coordinator tests, camera lifecycle/localization tests, release packaging, arm64 verification, signature check, and executable smoke test.
- [ ] Physical keyboard/TCC/DDC acceptance: [macOS checklist](external-monitor-hardware-keys/macos-acceptance.md). SDK payload decoding and synthetic tests cannot establish actual system interception or monitor support. (user-reported working 2026-10-16; per-row results not recorded)

The adapter uses the current cursor sampled through `CGEvent(source: nil)`; an unavailable position passes through instead of guessing. Per-target authorization epochs and one outstanding handoff per target/feature prevent stale manual writes and unbounded queues across repeated manual rebases. No new redistributed dependency or key-event logging was added. The privacy/input review found no high-confidence vulnerability in the changed paths. Windows remains deferred.

### Follow-up

Volume key routing was later changed to follow the Mac's default sound output instead of the pointer; see [hardware volume keys follow sound output](hardware-volume-keys-follow-sound-output.md). The completed tasks above describe the original pointer-based behavior for both features.

Mute was originally out of scope; it is now handled for the sound-output monitor, see [mute key restores volume](hardware-mute-key-restores-volume.md).

### Follow-up: test gaps closed (2026-10-16)

- `CoreBridgeTests.hardwareBrightnessKeyDuringProtections` now runs the real `HardwareKeyRouter`, `CoreBridge`, and Rust core together. With presence protection dimmed and the screensaver active, a brightness-up key is handled, accepted by the core, and produces a pending `set_brightness` action with the expected target. `presence_dimmed` stays true.
- This closes the native-level gap for Step 6 (manual-override policy during protections). The Rust-level test `generation_checked_manual_command_keeps_existing_behavior_during_active_protections` remains the policy contract.
- `MediaKeyProbe.swift` listens to key down and key up for function-key codes as well as the media-event mask. This is wider than the note in Notes that says to observe only the media-event mask. It is intentional for the probe: the probe is listen-only and records only media and function-key codes.

### Follow-up: brightness keys are plain key events (2026-10-16)

- Probe evidence: a key probe (`--probe-media-keys`) on the user's real keyboard, with Accessibility and Input Monitoring granted and the cursor over the LG ULTRAFINE, showed that brightness-up arrives as ordinary `keyDown` and `keyUp` events with key code 144, and brightness-down with key code 145, both with the `fn` flag set. Neither arrived as an `NX_SYSDEFINED` media event.
- Volume arrived as `NX_SYSDEFINED` (`subtype=8 key=0` for volume up). That explains the first physical result: volume and mute worked, while brightness keys did nothing, because the router decoded brightness only from `NX_SYSDEFINED` events.
- Fix: the event tap also listens to `keyDown` and `keyUp`, and decodes only key codes 144 (brightness up) and 145 (brightness down) as brightness keys. The rules are unchanged: the single external display under the pointer is the target; normal step 6.25 points; Shift+Option fine step 1.5625 points; Command, Control and other modifiers pass through; the `fn` flag does not disqualify the key; key sequences stay latched; the key passes through over the built-in display or an unsupported control. Volume and mute routing are unchanged.
- Privacy: the tap now receives every keystroke. The handler reads only the key code, the autorepeat flag and the modifier flags, returns immediately for every other key code, never reads characters, and never stores or logs anything. This widens the event mask beyond the media-event-only mask recorded in the original notes.
- Status: the user confirmed on 2026-10-16 that volume and mute work with the monitor as the sound output. The brightness fix is awaiting physical confirmation; see the [macOS acceptance checklist](external-monitor-hardware-keys/macos-acceptance.md).

### Closure (2026-10-16)

- The user reported on 2026-10-16 that brightness, volume and mute keys all work on the connected LG ULTRAFINE with the rebuilt app (brightness after the key-code 144/145 fix).
- The individual rows of the macOS acceptance checklist were not recorded one by one and remain `Not run`.
- Windows remains deferred.
- The spec is archived by `/codeadvisor-dev:finalize`; its permanent rationale is recorded in [ADR 0004](../../../adr/0004-external-monitor-hardware-keys-and-fluid-controls.md).
