//! Core-owned manual-control queue and DDC dispatch (see specs/archived/core-command-queue/architecture.md).

mod dispatch;
mod keys;
mod manual;
mod model;
mod retry;

use dispatch::Dispatch;
use keys::{KeyEvent, Keys};
use manual::Manual;
use model::{clamped_step, parse_event, AttemptError, Effect, Event, StepOutput};
use serde_json::{json, Value};

pub use crate::strict_json::StepError;
pub use model::MAX_EVENT_BYTES;

/// Rounds of key-intent hand-off per event; a guard against an endless loop.
const MAX_HANDOFF_ROUNDS: usize = 256;

/// Issues the tokens that tie an effect to the event that answers it. The counter starts at 1,
/// so 0 is never a token.
pub(crate) struct Tokens {
    next: u64,
}

impl Tokens {
    pub(crate) fn new() -> Self {
        Self { next: 1 }
    }

    pub(crate) fn issue(&mut self) -> u64 {
        let token = self.next;
        self.next += 1;
        token
    }
}

/// The manual-control machine. One instance backs one handle; the caller serializes calls.
pub struct Controls {
    last_now: f64,
    tokens: Tokens,
    stopped: bool,
    brightness_step: f64,
    volume_step: f64,
    keys_enabled: bool,
    volume_owner: Option<String>,
    /// Revision of the newest snapshot the key policy has seen.
    last_revision: u64,
    manual: Manual,
    keys: Keys,
    dispatch: Dispatch,
}

impl Default for Controls {
    fn default() -> Self {
        Self::new()
    }
}

impl Controls {
    /// Creates an idle machine: key steps 5, keys disabled, no volume owner, not shut down.
    pub fn new() -> Self {
        Self {
            last_now: 0.0,
            tokens: Tokens::new(),
            stopped: false,
            brightness_step: model::DEFAULT_STEP,
            volume_step: model::DEFAULT_STEP,
            keys_enabled: false,
            volume_owner: None,
            last_revision: 0,
            manual: Manual::new(),
            keys: Keys::new(),
            dispatch: Dispatch::new(),
        }
    }

    /// Brightness key step in force, in whole points.
    #[doc(hidden)]
    pub fn brightness_step(&self) -> f64 {
        self.brightness_step
    }

    /// Volume key step in force, in whole points.
    #[doc(hidden)]
    pub fn volume_step(&self) -> f64 {
        self.volume_step
    }

    /// Monitor that owns volume keys, if any.
    #[doc(hidden)]
    pub fn volume_owner(&self) -> Option<&str> {
        self.volume_owner.as_deref()
    }

    /// True when the shell routes keys to the machine.
    #[doc(hidden)]
    pub fn keys_enabled(&self) -> bool {
        self.keys_enabled
    }

    /// Parses one JSON event, applies it and returns the rendered result.
    ///
    /// The event is validated completely before any field changes, so a rejected event leaves
    /// the machine exactly as it was, including `last_now`.
    pub fn step(&mut self, event_json: &[u8]) -> Result<String, StepError> {
        let (now, event) = parse_event(event_json)?;
        // A mis-ordered shell clock must not move a deadline backward.
        self.last_now = now.max(self.last_now);
        let now = self.last_now;
        let mut value = Value::Null;
        let mut effects = Vec::new();
        if !self.stopped {
            match event {
                Event::Configure {
                    brightness_step,
                    volume_step,
                    keys_enabled,
                    volume_owner,
                } => {
                    self.brightness_step = clamped_step(brightness_step);
                    self.volume_step = clamped_step(volume_step);
                    self.keys_enabled = keys_enabled;
                    self.keys
                        .configure(keys_enabled, brightness_step, volume_step);
                    if !keys_enabled {
                        // The key policy drops its owner when keys are disabled; mirror it.
                        self.volume_owner = None;
                    }
                    // An owner in an event that disables keys is ignored: the owner stays cleared.
                    if let Some(owner) = volume_owner.filter(|_| keys_enabled) {
                        self.keys.set_volume_owner(owner.as_deref());
                        self.volume_owner = owner;
                    }
                }
                Event::Observe(observation) => {
                    effects = self.manual.observe(now, &observation);
                    // The manual queue and the dispatcher ignore a snapshot older than the last
                    // one they applied; the key policy must not register its actions either.
                    if observation.revision >= self.last_revision {
                        self.last_revision = observation.revision;
                        self.keys.report_accepted_actions(now, &observation);
                        self.keys.observe(now, &self.manual);
                    }
                    effects.extend(self.dispatch.observe(now, &mut self.tokens, &observation));
                    effects.extend(self.process_completions(now));
                    effects.extend(self.hand_off_key_intents(now));
                }
                Event::MonitorProfile {
                    monitor_id,
                    manufacturer,
                } => {
                    self.dispatch.monitor_profile(&monitor_id, &manufacturer);
                }
                Event::MonitorRemoved { monitor_id } => {
                    self.dispatch.monitor_removed(&monitor_id);
                }
                Event::DisplayPower { on } => {
                    effects = self.dispatch.display_power(now, &mut self.tokens, on);
                }
                Event::Slider {
                    monitor_id,
                    feature,
                    value: target,
                } => {
                    let result = self.manual.set_target(now, &monitor_id, feature, target);
                    if let Some(view) = result {
                        if let Some(generation) = self.manual.monitor_generation(&monitor_id) {
                            self.keys.manual_control_changed(
                                &self.manual,
                                &monitor_id,
                                generation,
                                feature,
                                view.draft,
                            );
                        }
                    }
                    value = result.map_or(
                        Value::Null,
                        |view| json!({"draft": view.draft, "status": view.status.as_str()}),
                    );
                }
                Event::Key {
                    key,
                    phase,
                    repeat,
                    fine,
                    owner,
                } => {
                    let event = KeyEvent {
                        key,
                        phase,
                        is_repeat: repeat,
                        fine,
                        modifiers_eligible: true,
                    };
                    let consume = self
                        .keys
                        .handle_key(now, &event, owner.as_ref(), &self.manual);
                    effects = self.hand_off_key_intents(now);
                    value = json!({"consume": consume});
                }
                Event::QueryDraft {
                    monitor_id,
                    feature,
                } => {
                    let view = self.manual.draft(&monitor_id, feature);
                    value = json!({
                        "has_draft": view.has_draft,
                        "draft": view.draft,
                        "status": view.status.as_str(),
                    });
                }
                Event::Submitted { token, accepted } => {
                    effects = self
                        .manual
                        .on_submitted(now, &mut self.tokens, token, accepted);
                    for (monitor_id, feature, generation, percent) in self.manual.take_accepted() {
                        self.keys.manual_control_accepted(
                            &self.manual,
                            &monitor_id,
                            generation,
                            feature,
                            percent,
                        );
                    }
                    let more = self
                        .dispatch
                        .on_submitted(now, &mut self.tokens, token, accepted);
                    effects.extend(more);
                    effects.extend(self.process_completions(now));
                }
                Event::AttemptDone {
                    token,
                    ok,
                    error,
                    value: raw,
                } => {
                    effects = self.dispatch.on_attempt_done(
                        now,
                        &mut self.tokens,
                        token,
                        ok,
                        error.unwrap_or(AttemptError::None),
                        raw,
                    );
                    effects.extend(self.process_completions(now));
                }
                Event::Tick => {
                    effects = self.manual.tick(now, &mut self.tokens);
                    effects.extend(self.dispatch.tick(now, &mut self.tokens));
                    effects.extend(self.process_completions(now));
                }
                Event::Shutdown => {
                    self.stopped = true;
                    self.keys.shutdown();
                    self.manual.shutdown();
                    self.dispatch.shutdown();
                }
            }
        }
        let next_deadline = match (self.manual.next_deadline(), self.dispatch.next_deadline()) {
            (Some(manual), Some(dispatch)) => Some(manual.min(dispatch)),
            (manual, dispatch) => manual.or(dispatch),
        };
        Ok(StepOutput {
            value,
            effects,
            next_deadline,
        }
        .render())
    }

    /// Reports the set actions that the dispatcher finished to the manual and key machines, then
    /// submits the key intents that this may have produced.
    fn process_completions(&mut self, now: f64) -> Vec<Effect> {
        let mut effects = Vec::new();
        for completion in self.dispatch.take_completions() {
            effects.extend(self.manual.on_hardware_completed(
                now,
                &completion.monitor_id,
                completion.feature,
                completion.connection_generation,
                completion.ok,
                completion.observed_percent,
            ));
            self.keys.action_completed(
                now,
                &completion.monitor_id,
                completion.connection_generation,
                completion.feature,
                completion.action_generation,
            );
        }
        effects.extend(self.hand_off_key_intents(now));
        effects
    }

    /// Submits every key intent the key policy produced, in rounds, and reports each outcome
    /// back to it. Returns a `DraftChanged` effect per accepted draft, once per slot.
    fn hand_off_key_intents(&mut self, now: f64) -> Vec<Effect> {
        let mut effects: Vec<Effect> = Vec::new();
        for _ in 0..MAX_HANDOFF_ROUNDS {
            let intents = self.keys.take_intents();
            if intents.is_empty() {
                break;
            }
            for intent in intents {
                let accepted = self
                    .manual
                    .set_target(now, &intent.monitor_id, intent.feature, intent.target)
                    .is_some();
                self.keys
                    .intent_finished(now, &self.manual, &intent, accepted);
                if !accepted {
                    continue;
                }
                let draft = Effect::DraftChanged {
                    monitor_id: intent.monitor_id.clone(),
                    feature: intent.feature,
                };
                if !effects.contains(&draft) {
                    effects.push(draft);
                }
            }
        }
        effects
    }
}
