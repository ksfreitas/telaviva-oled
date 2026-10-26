//! Monotonic, generation-bound positive presence evidence for keep-awake consumers.

use std::time::{Duration, Instant};

const LEASE_PADDING_MS: u64 = 15_000;
const MAX_LEASE_MS: u64 = 315_000;

#[derive(Clone, Debug)]
struct Observation {
    id: u64,
    generation: u64,
    expires_at: Instant,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct KeepAwakePresence {
    observation: Option<Observation>,
}

impl KeepAwakePresence {
    pub(crate) fn observe_present(
        &mut self,
        id: u64,
        generation: u64,
        sampling_interval_s: f64,
        now: Instant,
    ) {
        let interval_ms = (sampling_interval_s * 1000.0).round() as u64;
        let lease_ms = interval_ms
            .saturating_add(LEASE_PADDING_MS)
            .min(MAX_LEASE_MS);
        self.observation = Some(Observation {
            id,
            generation,
            expires_at: now + Duration::from_millis(lease_ms),
        });
    }

    pub(crate) fn invalidate(&mut self) {
        self.observation = None;
    }

    pub(crate) fn observation_id(&self) -> u64 {
        self.observation
            .as_ref()
            .map(|observation| observation.id)
            .unwrap_or(0)
    }

    pub(crate) fn expired_for(&self, generation: u64, now: Instant) -> bool {
        self.observation.as_ref().is_some_and(|observation| {
            observation.generation == generation && observation.expires_at <= now
        })
    }

    pub(crate) fn valid_for_ms(&self, generation: u64, now: Instant) -> u64 {
        let Some(observation) = self.observation.as_ref() else {
            return 0;
        };
        if observation.generation != generation {
            return 0;
        }
        observation
            .expires_at
            .saturating_duration_since(now)
            .as_millis() as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lease_uses_configured_sampling_interval_plus_padding_and_caps_at_315_seconds() {
        let now = Instant::now();
        let mut evidence = KeepAwakePresence::default();
        evidence.observe_present(17, 3, 10.0, now);
        assert_eq!(evidence.valid_for_ms(3, now), 25_000);

        evidence.observe_present(18, 4, 300.0, now);
        assert_eq!(evidence.valid_for_ms(4, now), 315_000);
    }

    #[test]
    fn generation_mismatch_and_expiry_revoke_without_renewal() {
        let now = Instant::now();
        let mut evidence = KeepAwakePresence::default();
        evidence.observe_present(17, 3, 5.0, now);
        assert_eq!(evidence.valid_for_ms(4, now), 0);
        assert_eq!(evidence.valid_for_ms(3, now + Duration::from_secs(20)), 0);
        assert!(evidence.expired_for(3, now + Duration::from_secs(20)));
    }

    #[test]
    fn invalidation_clears_the_positive_token() {
        let mut evidence = KeepAwakePresence::default();
        evidence.observe_present(17, 3, 10.0, Instant::now());
        evidence.invalidate();
        assert_eq!(evidence.observation_id(), 0);
    }
}
