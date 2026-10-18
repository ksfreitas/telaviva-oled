//! Manual slider queue: drafts, latest-wins coalescing and one write in flight per monitor and
//! feature. Port of `native/windows/Platform/ManualControls.cpp`. The observer callbacks and the
//! external-intent flow are gone, because the machine sees every event itself.

use super::keys::{DisplayState, Displays};
use super::model::{Effect, Feature, Observation};
use super::Tokens;
use serde_json::json;
use std::collections::{BTreeMap, BTreeSet};

/// Long enough that a drag coalesces into one submission, short enough to feel immediate.
pub(crate) const DRAIN_DELAY: f64 = 0.020;
/// A completion that finds a queued target sends it without the drag debounce.
pub(crate) const IMMEDIATE_DRAIN: f64 = 0.0;
/// Most drafts held at once. Targets beyond this are dropped, and the UI shows the observed value.
pub(crate) const MAX_DRAFTS: usize = 128;
/// Readback distance, in percent, that counts as the draft having been reached.
pub(crate) const READBACK_TOLERANCE: f64 = 0.51;
/// How long a submitted command may stay in flight without a completion before a snapshot may
/// release it. A completion normally releases it first.
pub(crate) const IN_FLIGHT_SAFETY_SECS: f64 = 3.0;

/// Ready: no unsettled draft. Pending: a draft is queued or in flight. Failed: the core or the
/// hardware rejected the last submission, so the draft follows the observed value.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(crate) enum DraftStatus {
    #[default]
    Ready,
    Pending,
    Failed,
}

impl DraftStatus {
    /// The lowercase name used in JSON.
    pub(crate) fn as_str(&self) -> &'static str {
        match self {
            Self::Ready => "ready",
            Self::Pending => "pending",
            Self::Failed => "failed",
        }
    }
}

/// What the UI shows for one monitor and feature.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct DraftView {
    pub(crate) has_draft: bool,
    pub(crate) draft: f64,
    pub(crate) status: DraftStatus,
}

/// One feature of one monitor, as the latest snapshot described it.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
struct Channel {
    /// Supported with an observed value, awake, stable identity and a non-empty range.
    eligible: bool,
    /// Normalized percent, 0 to 100. Reads 0 when unknown; see `observed_known`.
    observed: f64,
    /// Whether the snapshot carried an observed value.
    observed_known: bool,
    min: f64,
    max: f64,
    /// The latest snapshot lists a pending manual action for this feature.
    action_pending: bool,
}

#[derive(Debug, Clone, Default, PartialEq)]
struct MonitorState {
    generation: u64,
    /// Indexed by `channel_index`.
    channels: [Channel; 2],
}

/// The draft and the submission state of one monitor and feature.
#[derive(Debug, Clone, Default, PartialEq)]
struct Slot {
    has_draft: bool,
    draft: f64,
    status: DraftStatus,
    /// Newest target that has not been submitted yet.
    queued: Option<f64>,
    /// Accepted for submission and not yet released by a completion or the safety window.
    in_flight: bool,
    /// Raw value of the in-flight target, for deduplication.
    in_flight_raw: i64,
    /// Percent value of the in-flight target, reported by `take_accepted`.
    in_flight_value: f64,
    in_flight_generation: u64,
    /// Snapshot revision that was current when the command was submitted.
    submitted_revision: u64,
    submitted_at: f64,
    /// Identifies the in-flight submission; 0 when none.
    submit_token: u64,
}

/// Drafts and the coalescing drain. The core-side port of the Windows `ManualControls`.
#[derive(Default)]
pub(crate) struct Manual {
    monitors: BTreeMap<String, MonitorState>,
    slots: BTreeMap<(String, Feature), Slot>,
    drain_deadline: Option<f64>,
    snapshot_revision: u64,
    stopped: bool,
    /// Slots whose draft changed; flushed into `DraftChanged` effects in key order.
    dirty: BTreeSet<(String, Feature)>,
    /// Accepted submissions not yet taken by `take_accepted`: monitor, feature, connection
    /// generation and submitted percent.
    accepted_log: Vec<(String, Feature, u64, f64)>,
}

impl Manual {
    /// Creates an empty queue with nothing scheduled.
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Refreshes the known monitors from a snapshot. A snapshot older than the last applied one
    /// is ignored; an equal revision is accepted.
    pub(crate) fn observe(&mut self, now: f64, obs: &Observation) -> Vec<Effect> {
        if self.stopped || obs.revision < self.snapshot_revision {
            return Vec::new();
        }
        self.snapshot_revision = obs.revision;

        let gone: Vec<String> = self
            .monitors
            .keys()
            .filter(|id| !obs.monitors.iter().any(|monitor| monitor.id == **id))
            .cloned()
            .collect();
        for id in gone {
            self.drop_slots(&id);
            self.monitors.remove(&id);
        }

        for monitor in &obs.monitors {
            if self
                .monitors
                .get(&monitor.id)
                .is_some_and(|state| state.generation != monitor.generation)
            {
                // A new connection generation must not inherit drafts from the old one.
                self.drop_slots(&monitor.id);
            }
            let mut channels = [Channel::default(); 2];
            for feature in [Feature::Brightness, Feature::Volume] {
                let source = match feature {
                    Feature::Brightness => &monitor.brightness,
                    Feature::Volume => &monitor.volume,
                };
                let kind = action_kind(feature);
                let channel = Channel {
                    eligible: source.supported
                        && source.observed.is_some()
                        && monitor.display_state != "asleep"
                        && monitor.identity_status == "stable"
                        && source.max > source.min,
                    observed: source.observed.unwrap_or(0.0),
                    observed_known: source.observed.is_some(),
                    min: source.min,
                    max: source.max,
                    // Matched by monitor and kind only: the action generation is not the
                    // connection generation.
                    action_pending: obs
                        .pending_actions
                        .iter()
                        .any(|action| action.monitor_id == monitor.id && action.kind == kind),
                };
                self.refresh_slot(now, &monitor.id, feature, &channel, obs.revision);
                channels[channel_index(feature)] = channel;
            }
            self.monitors.insert(
                monitor.id.clone(),
                MonitorState {
                    generation: monitor.generation,
                    channels,
                },
            );
        }

        self.clear_drain_if_idle();
        self.flush_dirty()
    }

    /// Stores the draft at once, clamped to 0..=100, and replaces any unsent target with it.
    /// Returns `None` when the target is ignored.
    pub(crate) fn set_target(
        &mut self,
        now: f64,
        monitor_id: &str,
        feature: Feature,
        value: f64,
    ) -> Option<DraftView> {
        if !value.is_finite() || self.stopped {
            return None;
        }
        let target = value.clamp(0.0, 100.0);
        let channel = self.monitors.get(monitor_id)?.channels[channel_index(feature)];
        if !channel.eligible {
            return None;
        }

        let key = (monitor_id.to_owned(), feature);
        let holds_draft = self.slots.get(&key).is_some_and(|slot| slot.has_draft);
        if !holds_draft && self.drafts_count() >= MAX_DRAFTS {
            return None;
        }
        let slot = self.slots.entry(key).or_default();
        slot.has_draft = true;
        slot.draft = target;
        slot.status = DraftStatus::Pending;
        let view = DraftView {
            has_draft: true,
            draft: target,
            status: DraftStatus::Pending,
        };

        if slot.in_flight && raw_target(channel.min, channel.max, target) == slot.in_flight_raw {
            // Same raw value as the in-flight command: nothing new to send, and any unsent
            // target is stale.
            slot.queued = None;
            self.clear_drain_if_idle();
            return Some(view);
        }
        slot.queued = Some(target);
        self.schedule_drain(now, DRAIN_DELAY);
        Some(view)
    }

    /// Runs the drain when its deadline has passed. Calling it early or twice is harmless.
    pub(crate) fn tick(&mut self, now: f64, tokens: &mut Tokens) -> Vec<Effect> {
        if self.stopped {
            return Vec::new();
        }
        let mut effects = Vec::new();
        if self.drain_deadline.is_some_and(|deadline| now >= deadline) {
            self.drain_deadline = None;
            effects = self.drain(now, tokens);
        }
        effects.extend(self.flush_dirty());
        effects
    }

    /// Records the shell's answer to a `submit_command`. Unknown tokens are ignored. A rejection
    /// falls back to the observed value, unless a newer target is already waiting.
    pub(crate) fn on_submitted(
        &mut self,
        now: f64,
        _tokens: &mut Tokens,
        token: u64,
        accepted: bool,
    ) -> Vec<Effect> {
        let Some((key, slot)) = self
            .slots
            .iter_mut()
            .find(|(_, slot)| slot.in_flight && slot.submit_token == token)
        else {
            return Vec::new();
        };
        if accepted {
            self.accepted_log.push((
                key.0.clone(),
                key.1,
                slot.in_flight_generation,
                slot.in_flight_value,
            ));
            return Vec::new();
        }

        slot.in_flight = false;
        slot.submit_token = 0;
        if slot.queued.is_some() {
            self.schedule_drain(now, DRAIN_DELAY);
            return Vec::new();
        }
        let key = key.clone();
        slot.has_draft = true;
        slot.status = DraftStatus::Failed;
        if let Some(monitor) = self.monitors.get(&key.0) {
            let channel = monitor.channels[channel_index(key.1)];
            if channel.eligible {
                slot.draft = channel.observed;
            }
        }
        self.dirty.insert(key);
        self.flush_dirty()
    }

    /// Completion of a hardware action that this machine submitted. A completion from an older
    /// connection generation is ignored, so it can never release a newer command.
    pub(crate) fn on_hardware_completed(
        &mut self,
        now: f64,
        monitor_id: &str,
        feature: Feature,
        connection_generation: u64,
        ok: bool,
        observed_percent: Option<f64>,
    ) -> Vec<Effect> {
        if self.stopped {
            return Vec::new();
        }
        let channel = match self.monitors.get(monitor_id) {
            Some(monitor) => monitor.channels[channel_index(feature)],
            None => return Vec::new(),
        };
        let key = (monitor_id.to_owned(), feature);
        let Some(slot) = self.slots.get_mut(&key) else {
            return Vec::new();
        };
        if !slot.in_flight || slot.in_flight_generation != connection_generation {
            return Vec::new();
        }

        slot.in_flight = false;
        slot.submit_token = 0;
        if slot.queued.is_some() {
            // The newer target gets its own attempt now, whatever this result was.
            self.schedule_drain(now, IMMEDIATE_DRAIN);
            return Vec::new();
        }
        if ok {
            // The readback reaches the UI with the next snapshot; until then the observed value
            // shows.
            slot.has_draft = false;
            slot.status = DraftStatus::Ready;
            slot.draft = 0.0;
        } else {
            slot.has_draft = true;
            slot.status = DraftStatus::Failed;
            slot.draft = observed_percent.unwrap_or(channel.observed);
        }
        self.dirty.insert(key);
        self.flush_dirty()
    }

    /// The value the UI should show: the draft while one exists, else the observed value.
    pub(crate) fn draft(&self, monitor_id: &str, feature: Feature) -> DraftView {
        if let Some(slot) = self.slots.get(&(monitor_id.to_owned(), feature)) {
            if slot.has_draft {
                return DraftView {
                    has_draft: true,
                    draft: slot.draft,
                    status: slot.status,
                };
            }
        }
        let observed = self.monitors.get(monitor_id).map_or(0.0, |monitor| {
            monitor.channels[channel_index(feature)].observed
        });
        DraftView {
            has_draft: false,
            draft: observed,
            status: DraftStatus::Ready,
        }
    }

    /// Takes the submissions the shell accepted since the last call, as (monitor id, feature,
    /// connection generation, submitted percent). Each one is returned once.
    pub(crate) fn take_accepted(&mut self) -> Vec<(String, Feature, u64, f64)> {
        std::mem::take(&mut self.accepted_log)
    }

    /// The display state of one channel for the key policy. None when the monitor is unknown or
    /// its connection generation differs from `generation`.
    pub(crate) fn display_state(
        &self,
        monitor_id: &str,
        generation: u64,
        feature: Feature,
    ) -> Option<DisplayState> {
        let monitor = self.monitors.get(monitor_id)?;
        if monitor.generation != generation {
            return None;
        }
        let channel = monitor.channels[channel_index(feature)];
        let failed = self
            .slots
            .get(&(monitor_id.to_owned(), feature))
            .is_some_and(|slot| slot.has_draft && slot.status == DraftStatus::Failed);
        Some(DisplayState {
            percent: if channel.observed_known {
                channel.observed
            } else {
                f64::NAN
            },
            supported: channel.eligible,
            awake: channel.eligible,
            pending: channel.action_pending,
            failed,
        })
    }

    /// Connection generation of a known monitor, None when the monitor is unknown.
    pub(crate) fn monitor_generation(&self, monitor_id: &str) -> Option<u64> {
        self.monitors
            .get(monitor_id)
            .map(|monitor| monitor.generation)
    }

    /// Absolute deadline of the pending drain, if one is scheduled.
    pub(crate) fn next_deadline(&self) -> Option<f64> {
        self.drain_deadline
    }

    /// Stops all work: drops every draft and queued target, cancels the drain, and turns later
    /// calls into no-ops.
    pub(crate) fn shutdown(&mut self) {
        self.stopped = true;
        self.drain_deadline = None;
        self.slots.clear();
        self.monitors.clear();
        self.dirty.clear();
        self.accepted_log.clear();
    }

    fn drafts_count(&self) -> usize {
        self.slots.values().filter(|slot| slot.has_draft).count()
    }

    /// Cancels the drain deadline when no slot has a queued target, so nothing asks the shell
    /// for a wake-up.
    fn clear_drain_if_idle(&mut self) {
        if !self.slots.values().any(|slot| slot.queued.is_some()) {
            self.drain_deadline = None;
        }
    }

    /// Schedules the drain unless one is already pending. Only the first deadline counts, so a
    /// burst of targets coalesces into the one drain.
    fn schedule_drain(&mut self, now: f64, delay: f64) {
        if self.stopped || self.drain_deadline.is_some() {
            return;
        }
        self.drain_deadline = Some(now + delay);
    }

    /// Submits the queued target of every slot that may send one now.
    fn drain(&mut self, now: f64, tokens: &mut Tokens) -> Vec<Effect> {
        let mut effects = Vec::new();
        let revision = self.snapshot_revision;
        for (key, slot) in self.slots.iter_mut() {
            let Some(value) = slot.queued else {
                continue;
            };
            let Some(monitor) = self.monitors.get(&key.0) else {
                continue;
            };
            let channel = monitor.channels[channel_index(key.1)];
            // One command per channel: wait while this slot is in flight or the core is already
            // running a manual action for the same channel.
            if !channel.eligible || slot.in_flight || channel.action_pending {
                continue;
            }

            slot.queued = None;
            slot.draft = value;
            slot.in_flight = true;
            slot.in_flight_value = value;
            slot.in_flight_raw = raw_target(channel.min, channel.max, value);
            slot.in_flight_generation = monitor.generation;
            slot.submitted_revision = revision;
            slot.submitted_at = now;
            slot.submit_token = tokens.issue();
            effects.push(Effect::SubmitCommand {
                token: slot.submit_token,
                command_json: command_json(key.1, &key.0, value, monitor.generation),
            });
        }
        effects
    }

    /// Applies one channel's current state to its slot: drops the slot when the channel is
    /// ineligible, releases a stale in-flight command, clears a draft that reached the readback
    /// or completed, and reschedules a drain for a waiting target.
    fn refresh_slot(
        &mut self,
        now: f64,
        monitor_id: &str,
        feature: Feature,
        channel: &Channel,
        revision: u64,
    ) {
        let key = (monitor_id.to_owned(), feature);
        let Some(slot) = self.slots.get_mut(&key) else {
            return;
        };
        if !channel.eligible {
            // Sleep, unstable identity or an unsupported channel ends the draft and any in-flight
            // state. Nothing replays on wake.
            if slot.has_draft {
                self.dirty.insert(key);
            }
            *slot = Slot::default();
            return;
        }

        // Safety net only: the completion normally releases the slot. A snapshot may release it
        // once it has outlived the window, so a lost completion cannot wedge the channel.
        if slot.in_flight
            && now - slot.submitted_at >= IN_FLIGHT_SAFETY_SECS
            && revision > slot.submitted_revision
            && !channel.action_pending
        {
            slot.in_flight = false;
        }

        if slot.has_draft && slot.status == DraftStatus::Pending && slot.queued.is_none() {
            let readback = (channel.observed - slot.draft).abs() <= READBACK_TOLERANCE;
            let completed = !slot.in_flight;
            if readback || completed {
                slot.has_draft = false;
                slot.status = DraftStatus::Ready;
                slot.draft = 0.0;
                self.dirty.insert(key.clone());
            }
        }
        // A failed draft follows the observed value; it never retries on its own.
        if slot.has_draft && slot.status == DraftStatus::Failed {
            slot.draft = channel.observed;
        }
        let waiting = slot.queued.is_some() && !slot.in_flight;
        if waiting {
            self.schedule_drain(now, DRAIN_DELAY);
        }
    }

    /// Removes every slot of one monitor. A slot that held a draft is reported as changed.
    fn drop_slots(&mut self, monitor_id: &str) {
        let dirty = &mut self.dirty;
        self.slots.retain(|key, slot| {
            if key.0 != monitor_id {
                return true;
            }
            if slot.has_draft {
                dirty.insert(key.clone());
            }
            false
        });
    }

    /// Turns the dirty set into `DraftChanged` effects, in key order.
    fn flush_dirty(&mut self) -> Vec<Effect> {
        std::mem::take(&mut self.dirty)
            .into_iter()
            .map(|(monitor_id, feature)| Effect::DraftChanged {
                monitor_id,
                feature,
            })
            .collect()
    }
}

impl Displays for Manual {
    fn display(&self, monitor_id: &str, generation: u64, feature: Feature) -> Option<DisplayState> {
        self.display_state(monitor_id, generation, feature)
    }
}

fn channel_index(feature: Feature) -> usize {
    match feature {
        Feature::Brightness => 0,
        Feature::Volume => 1,
    }
}

/// The core action kind that a manual command for this feature creates.
fn action_kind(feature: Feature) -> &'static str {
    match feature {
        Feature::Brightness => "set_brightness",
        Feature::Volume => "set_volume",
    }
}

/// Raw device value for a normalized percent. Rounds half away from zero, the same rule as the
/// macOS shell. Narrow ranges can map two percents to one raw value, which deduplication relies
/// on. Only called for eligible channels, where `max > min`.
fn raw_target(min: f64, max: f64, percent: f64) -> i64 {
    let raw = (min + (max - min) * percent / 100.0).round();
    raw.clamp(min, max) as i64
}

/// The exact JSON that `tv_submit` accepts for a manual command. The core takes a normalized
/// percent and rejects the command when `connection_generation` is not the monitor's current one.
fn command_json(feature: Feature, monitor_id: &str, value: f64, generation: u64) -> String {
    json!({
        "type": action_kind(feature),
        "monitor_id": monitor_id,
        "value": value,
        "connection_generation": generation,
    })
    .to_string()
}

#[cfg(test)]
mod tests;
