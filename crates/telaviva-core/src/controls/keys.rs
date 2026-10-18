//! Key policy for manual controls: step arithmetic, the per-press policy latch, intents, rebase,
//! outstanding and settle tracking, and mute memory. Port of `native/windows/Platform/KeyIntentRouter.cpp`
//! with `VolumeActionTracker` folded into `report_accepted_actions`.

use super::model::{clamped_step, Feature, KeyKind, KeyOwner, KeyPhase, Observation};
use std::collections::{BTreeMap, BTreeSet};

/// Step in whole points used until the first `configure`.
pub(crate) const KEY_DEFAULT_STEP: f64 = 5.0;
/// Smallest accepted key step, in whole points.
pub(crate) const KEY_MIN_STEP: f64 = 1.0;
/// Largest accepted key step, in whole points.
pub(crate) const KEY_MAX_STEP: f64 = 15.0;
/// Amount of a fine (Shift+Option) key press, in points, whatever the step is.
pub(crate) const KEY_FINE_STEP: f64 = 1.0;
/// Readback distance, in percent, that counts as reached (one value for drafts and keys).
pub(crate) const KEY_READBACK_TOLERANCE: f64 = super::manual::READBACK_TOLERANCE;
/// Rebases wait this long after one of our writes completes, in milliseconds.
pub(crate) const KEY_REBASE_SETTLE_MS: i64 = 1000;
/// An accepted write without completion stops blocking after this, in milliseconds.
pub(crate) const KEY_OUTSTANDING_EXPIRY_MS: i64 = 10_000;
/// Bound on intents.
pub(crate) const KEY_MAX_INTENTS: usize = 128;
/// Bound on keys with a write in flight.
pub(crate) const KEY_MAX_IN_FLIGHT: usize = 128;
/// Bound on remembered mute levels.
pub(crate) const KEY_MAX_MUTE_MEMORY: usize = 128;
/// Bound on outstanding acceptances, per key and in total.
pub(crate) const KEY_MAX_OUTSTANDING: usize = 128;
/// Bound on early completions, in total.
pub(crate) const KEY_MAX_EARLY_COMPLETED: usize = 64;
/// Bound on action ids remembered by the action tracker.
pub(crate) const TRACKER_MAX_REMEMBERED: usize = 256;

/// One key event from the shell.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct KeyEvent {
    pub(crate) key: KeyKind,
    pub(crate) phase: KeyPhase,
    pub(crate) is_repeat: bool,
    /// Shift+Option: one point whatever the configured step is.
    pub(crate) fine: bool,
    /// Any other modifier makes the press pass through.
    pub(crate) modifiers_eligible: bool,
}

/// A write to submit as `set_volume` or `set_brightness` with `generation`.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct KeyIntent {
    pub(crate) monitor_id: String,
    pub(crate) generation: u64,
    pub(crate) feature: Feature,
    /// Normalized percent, 0 to 100.
    pub(crate) target: f64,
    pub(crate) revision: u64,
    pub(crate) epoch: u64,
}

/// What the shell last observed for one monitor and feature. `percent` is NaN when unknown.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct DisplayState {
    pub(crate) percent: f64,
    pub(crate) supported: bool,
    pub(crate) awake: bool,
    pub(crate) pending: bool,
    pub(crate) failed: bool,
}

/// Latest display state. Returns `None` unless the monitor id and connection generation match.
pub(crate) trait Displays {
    fn display(&self, monitor_id: &str, generation: u64, feature: Feature) -> Option<DisplayState>;
}

/// Step arithmetic: one point up or down, or the fine step; clamped to 0..=100, not rounded.
pub(crate) fn stepped(value: f64, increase: bool, fine: bool, step: f64) -> f64 {
    let amount = if fine { KEY_FINE_STEP } else { step };
    let delta = if increase { amount } else { -amount };
    (value + delta).clamp(0.0, 100.0)
}

/// Monitor, connection generation and feature that one intent or latch targets.
type Key = (String, u64, Feature);

/// The policy latch of one press. The swallow latch lives in the shells.
#[derive(Debug, Clone)]
enum Latch {
    /// The press was decided to pass through; its repeats and release pass through too.
    PassThrough,
    Handled {
        key: Key,
        increase: bool,
        fine: bool,
        cancelled: bool,
    },
    Mute {
        key: Key,
        cancelled: bool,
    },
}

#[derive(Debug, Clone)]
struct Intent {
    target: f64,
    revision: u64,
    epoch: u64,
    /// Target of the last manual slider change that set this intent, if any.
    manual_pending: Option<f64>,
    last_observed: Option<f64>,
    scheduled: bool,
}

/// Key policy state. Port of `KeyIntentRouter` (C++), with `VolumeActionTracker` folded in.
/// The C++ swallow latch stays in the shells; this type keeps only the policy latch.
#[derive(Debug)]
pub(crate) struct Keys {
    enabled: bool,
    stopped: bool,
    volume_owner: Option<String>,
    brightness_step: f64,
    volume_step: f64,
    /// Indexed by `kind_index`. A `Some` slot is a press in progress; `None` is no press.
    latches: [Option<Latch>; 5],
    intents: BTreeMap<Key, Intent>,
    mute_memory: BTreeMap<Key, f64>,
    in_flight: BTreeSet<Key>,
    /// Action generation to acceptance time in ms.
    outstanding: BTreeMap<Key, BTreeMap<u64, i64>>,
    rebase_blocked_until: BTreeMap<Key, i64>,
    early_completed: BTreeMap<Key, BTreeSet<u64>>,
    ready: Vec<KeyIntent>,
    intent_revision: u64,
    /// Action ids already reported by `report_accepted_actions`.
    reported: BTreeSet<u64>,
}

impl Keys {
    /// Creates an empty router with keys disabled.
    pub(crate) fn new() -> Self {
        Self {
            enabled: false,
            stopped: false,
            volume_owner: None,
            brightness_step: KEY_DEFAULT_STEP,
            volume_step: KEY_DEFAULT_STEP,
            latches: Default::default(),
            intents: BTreeMap::new(),
            mute_memory: BTreeMap::new(),
            in_flight: BTreeSet::new(),
            outstanding: BTreeMap::new(),
            rebase_blocked_until: BTreeMap::new(),
            early_completed: BTreeMap::new(),
            ready: Vec::new(),
            intent_revision: 0,
            reported: BTreeSet::new(),
        }
    }

    /// Stores enabled and both key steps, each clamped to whole points in 1..=15. Disabling drops all pending state and the volume owner. Ignored after `shutdown`.
    pub(crate) fn configure(&mut self, enabled: bool, brightness_step: f64, volume_step: f64) {
        if self.stopped {
            return;
        }
        self.enabled = enabled;
        self.brightness_step = clamped_step(brightness_step);
        self.volume_step = clamped_step(volume_step);
        if !enabled {
            self.clear_pending();
            self.volume_owner = None;
        }
    }

    /// Stores the volume owner. A change forgets remembered mute levels, which belong to the
    /// previous sound output. Ignored after `shutdown`.
    pub(crate) fn set_volume_owner(&mut self, owner: Option<&str>) {
        if self.stopped {
            return;
        }
        let next = owner.map(str::to_owned);
        if next != self.volume_owner {
            self.mute_memory.clear();
            self.volume_owner = next;
        }
    }

    /// Stops all work: pending state and the owner are dropped, and later calls are ignored.
    pub(crate) fn shutdown(&mut self) {
        self.enabled = false;
        self.stopped = true;
        self.clear_pending();
        self.volume_owner = None;
    }

    /// Returns true when the key event is consumed, false when it must reach Windows untouched.
    /// Runs `observe` first. The decision is latched per key, so the repeats and the release of
    /// a press get the same answer.
    pub(crate) fn handle_key(
        &mut self,
        now: f64,
        ev: &KeyEvent,
        owner: Option<&KeyOwner>,
        d: &dyn Displays,
    ) -> bool {
        if self.stopped {
            return false;
        }
        self.observe(now, d);
        self.process(ev, owner, d)
    }

    /// Intents to submit now, in the order they were produced. Each one must be reported through
    /// `intent_finished`, including an intent that is not submitted.
    pub(crate) fn take_intents(&mut self) -> Vec<KeyIntent> {
        std::mem::take(&mut self.ready)
    }

    /// Submission result of an intent returned by `take_intents`. A rejection with nothing newer
    /// drops the target, so the next press starts from the readback.
    pub(crate) fn intent_finished(
        &mut self,
        _now: f64,
        d: &dyn Displays,
        intent: &KeyIntent,
        accepted: bool,
    ) {
        let key = make_key(&intent.monitor_id, intent.generation, intent.feature);
        self.complete(d, &key, intent.revision, intent.epoch, accepted);
    }

    /// Core progress of one of our writes. An early completion of the same action cancels this
    /// acceptance instead of starting an outstanding one.
    pub(crate) fn action_accepted(
        &mut self,
        now: f64,
        monitor_id: &str,
        generation: u64,
        feature: Feature,
        action_generation: u64,
    ) {
        let key = make_key(monitor_id, generation, feature);
        let consumed = self
            .early_completed
            .get_mut(&key)
            .is_some_and(|set| set.remove(&action_generation));
        if consumed {
            let empty = self
                .early_completed
                .get(&key)
                .is_some_and(|set| set.is_empty());
            if empty {
                self.early_completed.remove(&key);
            }
            return;
        }
        self.register_outstanding(&key, action_generation, now_ms(now));
    }

    /// Core progress of one of our writes. Always starts the rebase settle window.
    pub(crate) fn action_completed(
        &mut self,
        now: f64,
        monitor_id: &str,
        generation: u64,
        feature: Feature,
        action_generation: u64,
    ) {
        let key = make_key(monitor_id, generation, feature);
        let now = now_ms(now);
        let removed = self
            .outstanding
            .get_mut(&key)
            .is_some_and(|actions| actions.remove(&action_generation).is_some());
        if removed {
            let empty = self
                .outstanding
                .get(&key)
                .is_some_and(|actions| actions.is_empty());
            if empty {
                self.outstanding.remove(&key);
            }
        } else {
            // The acceptance has not been registered yet; remember the completion so that it
            // cancels the acceptance.
            let total: usize = self.early_completed.values().map(|set| set.len()).sum();
            if total >= KEY_MAX_EARLY_COMPLETED {
                self.early_completed.clear();
            }
            self.early_completed
                .entry(key.clone())
                .or_default()
                .insert(action_generation);
        }
        self.rebase_blocked_until
            .insert(key, now + KEY_REBASE_SETTLE_MS);
    }

    /// A slider change, reported just before its manual `set_*` is submitted. Replaces the intent
    /// for the key with one whose target is `value`. Does not flush.
    pub(crate) fn manual_control_changed(
        &mut self,
        d: &dyn Displays,
        monitor_id: &str,
        generation: u64,
        feature: Feature,
        value: f64,
    ) {
        if !value.is_finite() || !(0.0..=100.0).contains(&value) {
            return;
        }
        if !self.enabled || self.stopped {
            return;
        }
        let key = make_key(monitor_id, generation, feature);
        self.intent_revision += 1;
        if self.intents.contains_key(&key) || self.intents.len() < KEY_MAX_INTENTS {
            // Replacing the epoch also invalidates a write already handed out.
            let revision = self.intent_revision;
            let observed = observed_value(d, &key);
            self.intents.insert(
                key,
                Intent {
                    target: value,
                    revision,
                    epoch: revision,
                    manual_pending: Some(value),
                    last_observed: observed,
                    scheduled: false,
                },
            );
        }
    }

    /// The submission of a manual change was accepted. Only the exact target of that change
    /// refreshes the observed value.
    pub(crate) fn manual_control_accepted(
        &mut self,
        d: &dyn Displays,
        monitor_id: &str,
        generation: u64,
        feature: Feature,
        value: f64,
    ) {
        let key = make_key(monitor_id, generation, feature);
        let observed = observed_value(d, &key);
        let Some(intent) = self.intents.get_mut(&key) else {
            return;
        };
        if intent.manual_pending != Some(value) || intent.target != value {
            return;
        }
        intent.last_observed = observed;
    }

    /// Reconciles pending intents with the latest displays. Ignored after `shutdown`.
    pub(crate) fn observe(&mut self, now: f64, d: &dyn Displays) {
        if self.stopped {
            return;
        }
        self.reconcile(now_ms(now), d);
    }

    /// Folds in the snapshot's set_volume and set_brightness actions, as the C++
    /// `VolumeActionTracker::onSnapshot` did for volume. Each newly listed action is reported
    /// once, in pending order, as (monitor id, connection generation, feature, action generation),
    /// and is passed on to `action_accepted`.
    pub(crate) fn report_accepted_actions(
        &mut self,
        now: f64,
        obs: &Observation,
    ) -> Vec<(String, u64, Feature, u64)> {
        let listed: BTreeSet<u64> = obs.pending_actions.iter().map(|action| action.id).collect();
        self.reported.retain(|id| listed.contains(id));

        let mut accepted = Vec::new();
        for action in &obs.pending_actions {
            let feature = match action.kind.as_str() {
                "set_volume" => Feature::Volume,
                "set_brightness" => Feature::Brightness,
                _ => continue,
            };
            if self.reported.contains(&action.id) {
                continue;
            }
            let Some(monitor) = obs.monitors.iter().find(|m| m.id == action.monitor_id) else {
                continue;
            };
            if self.reported.len() >= TRACKER_MAX_REMEMBERED {
                break;
            }
            self.reported.insert(action.id);
            accepted.push((
                action.monitor_id.clone(),
                monitor.generation,
                feature,
                action.generation,
            ));
            self.action_accepted(
                now,
                &action.monitor_id,
                monitor.generation,
                feature,
                action.generation,
            );
        }
        accepted
    }

    fn process(&mut self, ev: &KeyEvent, owner: Option<&KeyOwner>, d: &dyn Displays) -> bool {
        let index = kind_index(ev.key);
        if ev.phase == KeyPhase::Up {
            let held = self.latches[index].take();
            return matches!(held, Some(Latch::Handled { .. }) | Some(Latch::Mute { .. }));
        }
        if !ev.is_repeat {
            self.latches[index] = None;
        }
        if let Some(mut held) = self.latches[index].take() {
            let consumed = self.repeat_press(&mut held, owner, d);
            self.latches[index] = Some(held);
            return consumed;
        }
        self.fresh_press(ev, owner, d)
    }

    /// A repeat of a press already decided: keep the decision for the whole sequence.
    fn repeat_press(
        &mut self,
        held: &mut Latch,
        owner: Option<&KeyOwner>,
        d: &dyn Displays,
    ) -> bool {
        match held {
            Latch::PassThrough => false,
            Latch::Mute { key, cancelled } => {
                // Repeats of a held mute are consumed without toggling again.
                if !*cancelled
                    && live_owner_display(owner, key, d)
                    && self.value_for(key, d).is_some()
                {
                    return true;
                }
                *cancelled = true;
                true
            }
            Latch::Handled {
                key,
                increase,
                fine,
                cancelled,
            } => {
                if !*cancelled {
                    let value = if live_owner_display(owner, key, d) {
                        self.value_for(key, d)
                    } else {
                        None
                    };
                    if let Some(value) = value {
                        self.reserve_step(d, key, *increase, *fine, value);
                        return true;
                    }
                }
                *cancelled = true;
                true
            }
        }
    }

    /// A fresh press: decide once and latch the decision for its repeats and release.
    fn fresh_press(&mut self, ev: &KeyEvent, owner: Option<&KeyOwner>, d: &dyn Displays) -> bool {
        let index = kind_index(ev.key);
        if ev.is_repeat || !self.enabled || !ev.modifiers_eligible {
            self.latches[index] = Some(Latch::PassThrough);
            return false;
        }
        let is_mute = ev.key == KeyKind::Mute;
        let increase = matches!(ev.key, KeyKind::VolumeUp | KeyKind::BrightnessUp);
        let feature = feature_of(ev.key);
        let Some(key) = owner_key(owner, feature, d) else {
            self.latches[index] = Some(Latch::PassThrough);
            return false;
        };
        let reserved = match self.value_for(&key, d) {
            Some(value) if is_mute => self.reserve_mute(d, &key, value),
            Some(value) => self.reserve_step(d, &key, increase, ev.fine, value),
            None => false,
        };
        if !reserved {
            self.latches[index] = Some(Latch::PassThrough);
            return false;
        }
        self.latches[index] = Some(if is_mute {
            Latch::Mute {
                key,
                cancelled: false,
            }
        } else {
            Latch::Handled {
                key,
                increase,
                fine: ev.fine,
                cancelled: false,
            }
        });
        true
    }

    fn reserve_step(
        &mut self,
        d: &dyn Displays,
        key: &Key,
        increase: bool,
        fine: bool,
        value: f64,
    ) -> bool {
        // The mute restore steps up the volume, so it is keyed by volume and uses the volume step.
        let step = match key.2 {
            Feature::Brightness => self.brightness_step,
            Feature::Volume => self.volume_step,
        };
        if let Some(level) = self.mute_memory.get(key).copied() {
            if value != 0.0 {
                self.mute_memory.remove(key); // Changed elsewhere since the mute.
            } else if !increase {
                return true; // Still muted: consume without a write and keep the level.
            } else {
                // Restore the remembered level, then step up from it, as one write.
                let next = stepped(level, increase, fine, step);
                if !self.reserve_write(d, key, value, next) {
                    return false;
                }
                self.mute_memory.remove(key);
                return true;
            }
        }
        let next = stepped(value, increase, fine, step);
        self.reserve_write(d, key, value, next)
    }

    /// A mute press remembers the level and writes 0; the next mute press restores it.
    fn reserve_mute(&mut self, d: &dyn Displays, key: &Key, value: f64) -> bool {
        if value != 0.0 {
            self.mute_memory.remove(key); // Stale memory when the level changed elsewhere.
        }
        if value > 0.0 {
            if self.mute_memory.len() >= KEY_MAX_MUTE_MEMORY
                || !self.reserve_write(d, key, value, 0.0)
            {
                return false;
            }
            self.mute_memory.insert(key.clone(), value);
            return true;
        }
        let Some(level) = self.mute_memory.get(key).copied() else {
            return true; // At zero with nothing to restore.
        };
        if !self.reserve_write(d, key, value, level) {
            return false;
        }
        self.mute_memory.remove(key);
        true
    }

    fn reserve_write(&mut self, d: &dyn Displays, key: &Key, value: f64, next: f64) -> bool {
        if !self.intents.contains_key(key) && self.intents.len() >= KEY_MAX_INTENTS {
            return false;
        }
        if !self.in_flight.contains(key) && self.in_flight.len() >= KEY_MAX_IN_FLIGHT {
            return false;
        }
        if next == value {
            return true;
        }
        self.intent_revision += 1;
        let revision = self.intent_revision;
        let observed = observed_value(d, key);
        let intent = self.intents.entry(key.clone()).or_insert_with(|| Intent {
            target: value,
            revision,
            epoch: revision,
            manual_pending: None,
            last_observed: observed,
            scheduled: false,
        });
        intent.target = next;
        intent.revision = revision;
        let needs_worker = !intent.scheduled && !self.in_flight.contains(key);
        intent.scheduled = true;
        if needs_worker {
            self.flush(d, key);
        }
        true
    }

    /// Hands the newest target of a key to the caller, unless a write for that key is already in
    /// flight. Erases the intent when the key cannot take a write now, even if one is in flight.
    fn flush(&mut self, d: &dyn Displays, key: &Key) {
        let live = self.enabled
            && self.intents.contains_key(key)
            && display_for(d, key).is_some_and(|s| s.supported && s.awake);
        if !live {
            self.intents.remove(key);
            return;
        }
        if self.in_flight.contains(key) || self.in_flight.len() >= KEY_MAX_IN_FLIGHT {
            return;
        }
        let Some(intent) = self.intents.get(key) else {
            return;
        };
        self.in_flight.insert(key.clone());
        self.ready.push(KeyIntent {
            monitor_id: key.0.clone(),
            generation: key.1,
            feature: key.2,
            target: intent.target,
            revision: intent.revision,
            epoch: intent.epoch,
        });
    }

    fn complete(&mut self, d: &dyn Displays, key: &Key, revision: u64, epoch: u64, accepted: bool) {
        self.in_flight.remove(key);
        let Some((current_epoch, current_revision, scheduled)) = self
            .intents
            .get(key)
            .map(|intent| (intent.epoch, intent.revision, intent.scheduled))
        else {
            return;
        };
        if current_epoch != epoch {
            // Superseded by a manual change: send the newest target if one is waiting.
            if self.enabled && !self.stopped && scheduled {
                self.flush(d, key);
            }
            return;
        }
        if !accepted && current_revision == revision {
            // Rejected with nothing newer: drop the target, so the next press starts from the readback.
            self.intents.remove(key);
            return;
        }
        if current_revision == revision {
            if let Some(intent) = self.intents.get_mut(key) {
                intent.scheduled = false;
            }
            return;
        }
        if let Some(intent) = self.intents.get_mut(key) {
            intent.scheduled = true;
        }
        self.flush(d, key);
    }

    /// Runs before every observation and key event. Rebases a pending intent to the readback when
    /// the readback moved away from the last one seen and no write of ours is outstanding or
    /// settling.
    fn reconcile(&mut self, now: i64, d: &dyn Displays) {
        self.prune_rebase_state(now, d);

        self.mute_memory.retain(|key, _| {
            display_for(d, key)
                .is_some_and(|s| s.supported && s.awake && !s.failed && s.percent.is_finite())
        });

        let keys: Vec<Key> = self.intents.keys().cloned().collect();
        for key in keys {
            let usable = display_for(d, &key)
                .filter(|s| s.supported && s.awake && !s.failed && s.percent.is_finite());
            let Some(display) = usable else {
                self.intents.remove(&key);
                self.forget_rebase_state(&key);
                continue;
            };
            // While our own write is outstanding or settling, the readback is not an external change.
            let blocked = self
                .outstanding
                .get(&key)
                .is_some_and(|actions| !actions.is_empty())
                || self
                    .rebase_blocked_until
                    .get(&key)
                    .is_some_and(|until| now < *until);
            let Some(intent) = self.intents.get_mut(&key) else {
                continue;
            };
            if intent.scheduled || display.pending || blocked {
                continue;
            }
            let observed = display.percent;
            let rebase = intent.last_observed.is_some_and(|last| observed != last)
                && (intent.target - observed).abs() > KEY_READBACK_TOLERANCE;
            if rebase {
                self.intent_revision += 1;
                let revision = self.intent_revision;
                *intent = Intent {
                    target: observed,
                    revision,
                    epoch: revision,
                    manual_pending: None,
                    last_observed: Some(observed),
                    scheduled: false,
                };
            } else {
                intent.last_observed = Some(observed);
            }
        }
    }

    /// Drops acceptances older than the expiry, settle windows that have ended, and rebase state
    /// of keys that are no longer observed.
    fn prune_rebase_state(&mut self, now: i64, d: &dyn Displays) {
        self.outstanding.retain(|_, actions| {
            actions.retain(|_, accepted| now <= *accepted + KEY_OUTSTANDING_EXPIRY_MS);
            !actions.is_empty()
        });
        self.rebase_blocked_until.retain(|_, until| *until > now);

        let mut keys: BTreeSet<Key> = BTreeSet::new();
        keys.extend(self.outstanding.keys().cloned());
        keys.extend(self.rebase_blocked_until.keys().cloned());
        keys.extend(self.early_completed.keys().cloned());
        for key in keys {
            if display_for(d, &key).is_none() {
                self.forget_rebase_state(&key);
            }
        }
    }

    fn forget_rebase_state(&mut self, key: &Key) {
        self.outstanding.remove(key);
        self.rebase_blocked_until.remove(key);
        self.early_completed.remove(key);
    }

    /// Records an acceptance. At the bound the oldest acceptance is dropped: first for this key,
    /// then overall.
    fn register_outstanding(&mut self, key: &Key, action_generation: u64, now: i64) {
        let per_key = self.outstanding.get(key).map_or(0, |actions| actions.len());
        if per_key >= KEY_MAX_OUTSTANDING {
            self.drop_oldest_outstanding(Some(key));
        }
        let total: usize = self.outstanding.values().map(|actions| actions.len()).sum();
        if total >= KEY_MAX_OUTSTANDING {
            self.drop_oldest_outstanding(None);
        }
        self.outstanding
            .entry(key.clone())
            .or_default()
            .insert(action_generation, now);
    }

    fn drop_oldest_outstanding(&mut self, only: Option<&Key>) {
        let mut oldest: Option<(Key, u64, i64)> = None;
        for (key, actions) in &self.outstanding {
            if only.is_some_and(|wanted| wanted != key) {
                continue;
            }
            for (action, accepted) in actions {
                if oldest.as_ref().map_or(true, |(_, _, at)| accepted < at) {
                    oldest = Some((key.clone(), *action, *accepted));
                }
            }
        }
        let Some((key, action, _)) = oldest else {
            return;
        };
        let empty = match self.outstanding.get_mut(&key) {
            Some(actions) => {
                actions.remove(&action);
                actions.is_empty()
            }
            None => false,
        };
        if empty {
            self.outstanding.remove(&key);
        }
    }

    /// Drops every pending target, latch and rebase record. In-flight keys are kept: their
    /// `intent_finished` still arrives.
    fn clear_pending(&mut self) {
        for slot in self.latches.iter_mut() {
            *slot = None;
        }
        self.intents.clear();
        self.outstanding.clear();
        self.rebase_blocked_until.clear();
        self.early_completed.clear();
        self.mute_memory.clear();
    }

    /// Current value of a key: the pending target wins over the readback, so a held key keeps
    /// stepping from what it already wrote.
    fn value_for(&self, key: &Key, d: &dyn Displays) -> Option<f64> {
        match self.intents.get(key) {
            Some(intent) => Some(intent.target),
            None => observed_value(d, key),
        }
    }
}

impl Default for Keys {
    fn default() -> Self {
        Self::new()
    }
}

fn make_key(monitor_id: &str, generation: u64, feature: Feature) -> Key {
    (monitor_id.to_owned(), generation, feature)
}

/// Milliseconds from the shell's monotonic seconds. Non-positive and NaN times map to zero.
fn now_ms(now: f64) -> i64 {
    if now <= 0.0 {
        0
    } else {
        (now * 1000.0).round() as i64
    }
}

fn kind_index(key: KeyKind) -> usize {
    match key {
        KeyKind::VolumeUp => 0,
        KeyKind::VolumeDown => 1,
        KeyKind::Mute => 2,
        KeyKind::BrightnessUp => 3,
        KeyKind::BrightnessDown => 4,
    }
}

/// Mute is a volume key; the brightness keys target brightness.
fn feature_of(key: KeyKind) -> Feature {
    match key {
        KeyKind::VolumeUp | KeyKind::VolumeDown | KeyKind::Mute => Feature::Volume,
        KeyKind::BrightnessUp | KeyKind::BrightnessDown => Feature::Brightness,
    }
}

fn display_for(d: &dyn Displays, key: &Key) -> Option<DisplayState> {
    d.display(&key.0, key.1, key.2)
}

fn observed_value(d: &dyn Displays, key: &Key) -> Option<f64> {
    display_for(d, key)
        .map(|s| s.percent)
        .filter(|percent| percent.is_finite())
}

/// True when the owner names the key's monitor, the monitor reports the key's feature, and that
/// display is supported and awake.
fn live_owner_display(owner: Option<&KeyOwner>, key: &Key, d: &dyn Displays) -> bool {
    owner.is_some_and(|owner| owner.monitor_id == key.0)
        && display_for(d, key).is_some_and(|s| s.supported && s.awake)
}

/// The key of the owner's feature, when the owner's display is supported, awake and reports a
/// percent within 0..=100.
fn owner_key(owner: Option<&KeyOwner>, feature: Feature, d: &dyn Displays) -> Option<Key> {
    let owner = owner?;
    let key = make_key(&owner.monitor_id, owner.generation, feature);
    let display = display_for(d, &key)?;
    let live = display.supported
        && display.awake
        && display.percent.is_finite()
        && (0.0..=100.0).contains(&display.percent);
    live.then_some(key)
}

#[cfg(test)]
mod test_support;

#[cfg(test)]
mod tests;

#[cfg(test)]
mod brightness_tests;

#[cfg(test)]
mod quirk_tests;
