# ADR-0015: Native shells own UI strings

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** core-camera-lifecycle-abi-contract-and-cleanup
**Spec:** [Archived chore plan](../specs/archived/core-camera-lifecycle-abi-contract-and-cleanup/spec.md)

## Context

[ADR 0001](0001-native-shell-and-rust-core.md) states that static locale catalogs cover current messages. The core therefore carried a Fluent catalog for each locale, `locales/en-US.ftl` and `locales/pt-BR.ftl` with 21 messages, and put the parsed messages into every snapshot as `messages`. Swift read only 4 of those keys. The other 116 UI strings lived in `native/macos/TelaViva/Localization.swift`, so the text was split across two places, and 17 of the 21 core messages were carried in every snapshot without being read. The per-snapshot parsing is also one of the costs described in the [snapshot publish amplification spec](../specs/archived/snapshot-publish-amplification/plan.md).

## Decision

- **Each native shell owns its UI strings.** On macOS, `native/macos/TelaViva/Localization.swift` is the catalog. The Windows shell will use its own resource files when it is built.
- **The core keeps the `locale` setting and returns status codes.** The `locale` setting (`auto`, `en-US` or `pt-BR`, default `auto`) stays in the core's settings, and shells read it to choose their catalog. The core returns status codes for the shell to map to text, not display text.
- **The snapshot no longer carries `messages`.** The `messages` key is removed from the snapshot, and the `locales/*.ftl` catalogs are deleted.
- **Partial supersession of ADR 0001.** This record partially supersedes the statement in [ADR 0001](0001-native-shell-and-rust-core.md) that static locale catalogs cover current messages. ADR 0001 remains as history, and the rest of it is unchanged.
- **The `os_locale` option is inert.** The `os_locale` option of `tv_create` is still accepted, so the documented ABI does not change, but it no longer affects snapshot content. Removing it is a follow-up for the next ABI change.

## Alternatives Considered

- **A core-owned catalog served in every snapshot.** Rejected: the catalog would grow every snapshot, which the [snapshot publish amplification spec](../specs/archived/snapshot-publish-amplification/plan.md) already identifies as a cost, and Swift read only 4 of the 21 keys.
- **One catalog fetched once through a new function.** Rejected for now: it is a larger change to the ABI, and WinUI conventionally uses its own resource files, so a shared catalog would not match the Windows shell.

## Consequences

- macOS strings are edited in one place, `Localization.swift`. The core no longer parses FTL text, and the per-snapshot parse described in the snapshot publish amplification spec is removed with the catalog, so that spec's caching step no longer applies.
- The core test that compared the two catalogs' keys is removed. Translation parity between `en-US` and `pt-BR` is now the shells' responsibility, and no core test checks it.
- The full Fluent formatter that ADR 0001 defers has no consumer in the core, because the core no longer holds any text to format.
- Each shell translates its own strings, so the Windows shell will need its own `pt-BR` resources.

## Non-Obvious Constraints

- Do not add display text to core responses or snapshots. A new message belongs in the shell's catalog, and the core returns a status code that the shell maps to it.
- The core still validates the `locale` setting and rejects values other than `auto`, `en-US` and `pt-BR`, so a shell that reads it can rely on those three values.
