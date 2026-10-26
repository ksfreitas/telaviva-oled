# Contracts: Keep Awake With Presence

## Data Models

### Settings

`keep_awake_with_presence`: Boolean, defaults to false when absent in settings schema 1. Reject non-Booleans. Both native full-settings payloads round-trip it; no other preference is rewritten on enable/disable. ABI remains 1; no C struct layout changes.

### Core Snapshot

```json
{
  "keep_awake": {
    "enabled": true,
    "requested": true,
    "observation_id": 1793020000000,
    "generation": 12,
    "valid_for_ms": 24980,
    "reason": "present"
  }
}
```

- `enabled`: saved preference. `requested`: fresh positive evidence and eligible session, not OS acceptance.
- `observation_id`: accepted positive observation timestamp in epoch milliseconds, zero before evidence. Pair with generation; do not use global snapshot revision as a lease token.
- `generation`: current camera generation; evidence from a different generation cannot request inhibition.
- `valid_for_ms`: nonnegative integer remaining monotonic lease, zero when not requested. Full lease is configured normal sampling interval in milliseconds plus 15000; max 315000. Serialization must not turn positive sub-millisecond time into a new lease.
- `reason`: language-neutral code. At minimum distinguish disabled, session inactive, awaiting presence/unknown, absent, insufficient light, expired and present; implementation may use a smaller grouped unknown code if UI preserves the detection status from camera data.
- Missing additive object decodes to disabled/not-requested. Unknown additive snapshot fields remain tolerated by native readers.

## API Contracts

### `keep_awake_session`

```json
{"type":"keep_awake_session","active":false}
```

Required Boolean `active`, no unknown fields. Native shells report camera-monitoring eligibility: an awake console session with a known unlocked state. Screensaver, display-off and lid states remain separate, stricter native activity-posting vetoes; they do not alone disable local camera monitoring when the OS session can still run. Initial core eligibility is false until the shell reports its current session state. Changed eligibility invalidates evidence and camera generation while this feature is enabled; repeated identical values are idempotent for generation/evidence. Native code updates its local veto before submitting false, so queued snapshots cannot counteract it. True never creates positive presence. App startup/enable must reconcile current native session state before any activity pulse.

### Existing `presence_observation`

Validation remains generation-bound, ordered, not future-dated and at most 60 seconds old. Only an accepted positive sample creates/renews a lease. First accepted absent revokes inhibition independently of brightness misses. Low-light/unavailable/status errors revoke; `usable_light` never grants inhibition. Generation changes invalidate the previous lease. No data enters or leaves the ABI as camera images.

## Integration Contracts

Each native controller accepts the decoded core lease plus current local guards. When an inhibitor-only guard ends without changing the core camera generation, it accepts only evidence whose `observation_id` capture timestamp is newer than that transition, in addition to the monotonic lease and revocation checks. It owns exactly its requests, timer and worker; local denial, cleanup and expiry never depend on a UI refresh or subsequent camera callback. Same-token updates may shorten deadlines but cannot extend them. New tokens replace deadlines using the smaller of reported remaining TTL and the maximum lease. Queued older snapshots never restore a revoked token; retain a token/generation high-water mark or equivalent serialized ordering.

Energy-request and event-posting success are separate native results. Permission or policy failure produces localized unavailable status. An active/requesting label describes requests, not a guarantee about all security policies. Query-only OS APIs must not mutate global state. Deactivation stops pulses before releasing energy requests; cleanup is idempotent, cancels pending work, and joins workers before freeing callback targets.

## State Transitions

- Disabled -- enable --> awaiting fresh presence.
- Awaiting/absent/unknown -- valid present --> requested with finite lease.
- Requested -- valid present, newer token --> requested with replaced lease.
- Requested -- first valid absent --> absent, release immediately.
- Requested -- low light/failure/generation invalidation/expiry --> unknown, release.
- Any -- session becomes ineligible --> suspended, release and invalidate evidence.
- Suspended -- eligible again --> awaiting fresh presence; no automatic wake/unlock.
- Any -- disable/shutdown --> disabled, cancel and release.
- Native denied -- permission/policy becomes available --> await fresh/current valid evidence and guards; no automatic permission prompt.
