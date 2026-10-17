# ADR-0018: Windows monitor identity from EDID without the connector path

**Status:** Accepted
**Date:** 2026-10-17
**Feature:** windows-version
**Spec:** [Archived spec](../specs/archived/windows-version/spec.md)

## Context

The macOS monitor id embeds the connector location, so per-monitor curves are orphaned on a port change. Guessing between identical monitors could write to the wrong panel. [ADR 0008](0008-core-owned-ambient-follow-and-per-monitor-protection.md) records the macOS id behavior, and [ADR 0013](0013-user-managed-calibration-points.md) stores calibration points under the monitor id.

## Decision

- The id is `win-<manufacturer>-<product>-<serial>-<hex8>`. The manufacturer is the three-letter EDID code. The product and the serial are decimal. `hex8` is the first eight hex digits of the SHA-256 of the 128-byte EDID base block. Example from spike S2: `win-DEL-53688-808858963-a9c404a1`.
- The connector path goes to `connection_path` only and never enters the id.
- `identity_status` is `stable` when the id is unique among the attached monitors. A serial that is not usable (`0`, `1`, `0x01010101`, `0xFFFFFFFF`) does not by itself make a lone monitor ambiguous.
- If two or more attached monitors produce the same id, all of them are discovered as `ambiguous`. The core enqueues no hardware action for them, and the UI shows them read-only with a reason.
- This rule follows [contracts](../specs/archived/windows-version/contracts.md) as implemented on 2026-10-17. It differs from the earlier rule in [architecture](../specs/archived/windows-version/architecture.md) Decision 5, which made a monitor stable only with a usable serial.

## Alternatives Considered

- **Path-based ids.** Orphaned on every port change.
- **Ordinal disambiguation.** Can swap monitors silently.

## Consequences

- Curves, `protection_disabled` and `screensaver_targets` survive docks and port changes.
- A monitor whose EDID is rewritten gets a new id.
- Known limitation: two identical monitors without a usable serial cannot be controlled in the first release. The UI shows them read-only.
- Evidence:
  - [Spike S2](../specs/archived/windows-version/validation/spike-s2-identity.md): hardware row D1 Pass for one DELL S3225QC over HDMI, `identity_status` stable. DDC reads of VCP `0x10` and `0x62` succeeded. DDC writes were not tested.
  - Duplicate-id behavior is covered by unit tests only. Hardware row D9 (two identical monitors, dock, DisplayPort, USB-C) is not run.

## Non-Obvious Constraints

- The id hash uses the 128-byte EDID base block only. The spike's 384-byte EDID is hashed on its first block alone.
- Code that needs the connector location reads `connection_path`. It must not parse the id.
