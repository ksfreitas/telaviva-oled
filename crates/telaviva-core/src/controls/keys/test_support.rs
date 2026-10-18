//! Shared fake display source for the key tests.

use super::*;
use crate::controls::model::Feature;
use std::collections::BTreeMap;

/// Latest display states keyed by monitor, connection generation and feature.
#[derive(Default)]
pub(super) struct FakeDisplays {
    states: BTreeMap<(String, u64, Feature), DisplayState>,
}

impl FakeDisplays {
    /// Stores `state` as the latest observation of `monitor` and `feature`. Entries of the same
    /// monitor and feature with another generation are dropped.
    pub(super) fn set(
        &mut self,
        monitor: &str,
        generation: u64,
        feature: Feature,
        state: DisplayState,
    ) {
        self.states.retain(|(id, old, kept), _| {
            !(id.as_str() == monitor && *kept == feature && *old != generation)
        });
        self.states
            .insert((monitor.to_owned(), generation, feature), state);
    }
}

impl Displays for FakeDisplays {
    fn display(&self, monitor_id: &str, generation: u64, feature: Feature) -> Option<DisplayState> {
        self.states
            .get(&(monitor_id.to_owned(), generation, feature))
            .copied()
    }
}
