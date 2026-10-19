//! Pending-action dispatch for the controls machine.
//!
//! Port of the action side of `native/windows/Platform/DdcController.cpp` (`DdcController`):
//! takes the core's pending actions from each snapshot, runs at most one per monitor when the
//! monitor may run, and reports every result to the core as a `hardware_result`.

use super::model::{
    AttemptError, AttemptOp, Effect, Feature, Observation, PendingActionObs, MAX_MONITORS,
};
use super::retry::{
    OpKind, Operation, Outcome, Primitive, Profile, Step, BACKOFF_SECS, RESULT_RESUBMITS,
    WAKE_GRACE_SECS, WAKE_READY_WINDOW_SECS,
};
use super::Tokens;
use serde_json::json;
use std::collections::{BTreeMap, BTreeSet};

/// A finished set action, reported to the manual-control machine. Read actions never complete.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Completion {
    pub(crate) monitor_id: String,
    pub(crate) feature: Feature,
    pub(crate) connection_generation: u64,
    pub(crate) action_generation: u64,
    pub(crate) ok: bool,
    pub(crate) observed_percent: Option<f64>,
}

/// One monitor as the latest snapshot described it.
#[derive(Debug, Clone)]
struct MonitorInfo {
    generation: u64,
    display_state: String,
    identity_status: String,
    /// Raw (min, max) per feature: index 0 brightness, index 1 volume.
    ranges: [(f64, f64); 2],
}

/// Where one action is in its life.
enum State {
    /// Waiting for the single DDC execution slot to run this primitive.
    NeedsRun(Primitive),
    /// The primitive is running on the shell; the token answers it.
    Executing(u64),
    /// The operation sleeps until the absolute time.
    Waiting(f64),
    /// The result is ready and must be submitted to the core.
    NeedsSubmit,
    /// The result was handed to the core; the token answers it.
    Submitting(u64),
    /// The core rejected the result; resubmit at the absolute time.
    ResubmitWait(f64),
}

/// One action that the machine is running or has not yet reported.
struct Running {
    action: PendingActionObs,
    feature: Option<Feature>,
    connection_generation: u64,
    range: (f64, f64),
    op: Option<Operation>,
    state: State,
    outcome: Option<Outcome>,
    /// The first primitive of an action is checked against the hold rules before it runs.
    first_attempt: bool,
    resubmits: u32,
}

/// Runs pending DDC actions. Port of `DdcController`: one primitive executes at a time across
/// all monitors, a monitor that is asleep or in wake grace holds its actions, and a finished
/// action is reported once and never re-run.
///
/// The machine reads no clock. Every method takes `now`, the monotonic seconds from the shell.
#[derive(Default)]
pub(crate) struct Dispatch {
    monitors: BTreeMap<String, MonitorInfo>,
    profiles: BTreeMap<String, Profile>,
    latest_actions: Vec<PendingActionObs>,
    completed: BTreeSet<u64>,
    running: Vec<Running>,
    last_revision: u64,
    display_off: bool,
    grace_deadline: Option<f64>,
    /// End of the wake readiness window; an operation that starts before it records it.
    wake_ready_until: Option<f64>,
    /// One DDC primitive executes at a time across every monitor.
    exec_busy: bool,
    completions: Vec<Completion>,
    stopped: bool,
}

impl Dispatch {
    /// Creates an idle dispatcher with no monitors and nothing running.
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Replaces the monitor table from a snapshot, then starts what may run now.
    pub(crate) fn observe(
        &mut self,
        now: f64,
        tokens: &mut Tokens,
        obs: &Observation,
    ) -> Vec<Effect> {
        if self.stopped || obs.revision < self.last_revision {
            return Vec::new();
        }
        self.last_revision = obs.revision;
        self.monitors = obs
            .monitors
            .iter()
            .map(|monitor| {
                let info = MonitorInfo {
                    generation: monitor.generation,
                    display_state: monitor.display_state.clone(),
                    identity_status: monitor.identity_status.clone(),
                    ranges: [
                        (monitor.brightness.min, monitor.brightness.max),
                        (monitor.volume.min, monitor.volume.max),
                    ],
                };
                (monitor.id.clone(), info)
            })
            .collect();
        self.latest_actions = obs.pending_actions.clone();
        // Bound `completed` by dropping ids that the core no longer lists.
        let live: BTreeSet<u64> = obs.pending_actions.iter().map(|action| action.id).collect();
        self.completed.retain(|id| live.contains(id));
        self.take_ready(now);
        self.pump(tokens)
    }

    /// Records the profile of a DDC-capable monitor, from its EDID manufacturer code.
    /// Profiles are bounded by `MAX_MONITORS`. When the map is full, a new monitor id evicts the
    /// first profile (in key order) whose monitor the latest snapshot does not list. If every
    /// stored monitor is listed, the new id is ignored and falls back to the default profile.
    pub(crate) fn monitor_profile(&mut self, monitor_id: &str, manufacturer: &str) {
        if self.profiles.len() >= MAX_MONITORS && !self.profiles.contains_key(monitor_id) {
            let evictable = self
                .profiles
                .keys()
                .find(|id| !self.monitors.contains_key(*id))
                .cloned();
            let Some(evictable) = evictable else {
                return;
            };
            self.profiles.remove(&evictable);
        }
        self.profiles.insert(
            monitor_id.to_owned(),
            Profile::for_manufacturer(manufacturer),
        );
    }

    /// Forgets a monitor and its profile.
    pub(crate) fn monitor_removed(&mut self, monitor_id: &str) {
        self.profiles.remove(monitor_id);
        self.monitors.remove(monitor_id);
    }

    /// Applies the display power state. Off clears grace and the wake readiness window; on arms
    /// the wake grace and opens a readiness window of `WAKE_READY_WINDOW_SECS`. Starts nothing;
    /// a held action runs at the next `observe` or `tick` after grace ends.
    pub(crate) fn display_power(&mut self, now: f64, tokens: &mut Tokens, on: bool) -> Vec<Effect> {
        if self.stopped {
            return Vec::new();
        }
        if on {
            self.display_off = false;
            self.grace_deadline = Some(now + WAKE_GRACE_SECS);
            self.wake_ready_until = Some(now + WAKE_READY_WINDOW_SECS);
        } else {
            self.display_off = true;
            self.grace_deadline = None;
            self.wake_ready_until = None;
        }
        self.pump(tokens)
    }

    /// Records the shell's answer to a `submit_result`. An accepted result completes the action.
    /// A rejected one is resubmitted with backoff, at most `RESULT_RESUBMITS` times; the write is
    /// never re-run.
    pub(crate) fn on_submitted(
        &mut self,
        now: f64,
        tokens: &mut Tokens,
        token: u64,
        accepted: bool,
    ) -> Vec<Effect> {
        if self.stopped {
            return Vec::new();
        }
        let Some(index) = self
            .running
            .iter()
            .position(|entry| matches!(entry.state, State::Submitting(held) if held == token))
        else {
            return Vec::new();
        };
        if !accepted {
            self.running[index].resubmits += 1;
            let resubmits = self.running[index].resubmits;
            if resubmits <= RESULT_RESUBMITS {
                let delay = BACKOFF_SECS[(resubmits as usize - 1).min(BACKOFF_SECS.len() - 1)];
                self.running[index].state = State::ResubmitWait(now + delay);
                return self.pump(tokens);
            }
        }
        self.retire(index, accepted);
        self.take_ready(now);
        self.pump(tokens)
    }

    /// Records the outcome of one primitive that the shell ran. Unknown tokens are ignored.
    pub(crate) fn on_attempt_done(
        &mut self,
        now: f64,
        tokens: &mut Tokens,
        token: u64,
        ok: bool,
        error: AttemptError,
        value: Option<f64>,
    ) -> Vec<Effect> {
        if self.stopped {
            return Vec::new();
        }
        let Some(index) = self
            .running
            .iter()
            .position(|entry| matches!(entry.state, State::Executing(held) if held == token))
        else {
            return Vec::new();
        };
        self.exec_busy = false;
        let step = match self.running[index].op.as_mut() {
            Some(op) => op.on_attempt_done(now, ok, error, value),
            None => lost_operation(),
        };
        apply_step(&mut self.running[index], step);
        self.pump(tokens)
    }

    /// Runs the timers that have passed: the wake grace, the backoff waits and the result
    /// resubmissions. Calling it early or twice is harmless.
    pub(crate) fn tick(&mut self, now: f64, tokens: &mut Tokens) -> Vec<Effect> {
        if self.stopped {
            return Vec::new();
        }
        if self.grace_deadline.is_some_and(|deadline| deadline <= now) {
            self.grace_deadline = None;
            self.take_ready(now);
        }
        for entry in self.running.iter_mut() {
            if let State::Waiting(until) = entry.state {
                if until <= now {
                    let step = match entry.op.as_mut() {
                        Some(op) => op.on_timer(now),
                        None => lost_operation(),
                    };
                    apply_step(entry, step);
                }
            }
            if let State::ResubmitWait(until) = entry.state {
                if until <= now {
                    entry.state = State::NeedsSubmit;
                }
            }
        }
        self.pump(tokens)
    }

    /// Takes the completions of set actions reported since the last call.
    pub(crate) fn take_completions(&mut self) -> Vec<Completion> {
        std::mem::take(&mut self.completions)
    }

    /// Earliest absolute time at which `tick` has work: the wake grace or a timed wait.
    pub(crate) fn next_deadline(&self) -> Option<f64> {
        self.running
            .iter()
            .fold(self.grace_deadline, |best, entry| {
                let until = match &entry.state {
                    State::Waiting(until) | State::ResubmitWait(until) => *until,
                    _ => return best,
                };
                Some(best.map_or(until, |best| best.min(until)))
            })
    }

    /// Stops all work. Later events are ignored.
    pub(crate) fn shutdown(&mut self) {
        self.stopped = true;
        self.running.clear();
        self.completed.clear();
        self.latest_actions.clear();
        self.grace_deadline = None;
        self.wake_ready_until = None;
    }

    /// Starts the first action of each monitor that may run now and is not completed or running.
    fn take_ready(&mut self, now: f64) {
        let actions = self.latest_actions.clone();
        let mut seen = BTreeSet::new();
        for action in actions {
            // The core exposes at most one action per monitor; only that first one counts.
            if !seen.insert(action.monitor_id.clone()) {
                continue;
            }
            if self.completed.contains(&action.id)
                || self
                    .running
                    .iter()
                    .any(|entry| entry.action.id == action.id)
            {
                continue;
            }
            if !self.may_run(&action.monitor_id) {
                continue;
            }
            self.start(action, now);
        }
    }

    /// False during wake grace or while the display is off, and for a known monitor that is
    /// asleep. An unknown monitor may run, so that it fails with "disconnected".
    fn may_run(&self, monitor_id: &str) -> bool {
        if self.grace_deadline.is_some() || self.display_off {
            return false;
        }
        match self.monitors.get(monitor_id) {
            Some(info) => info.display_state != "asleep",
            None => true,
        }
    }

    /// Creates the running entry for an action. A monitor that is missing, ambiguous or
    /// unsupported fails at once, without a primitive.
    fn start(&mut self, action: PendingActionObs, now: f64) {
        let info = self.monitors.get(&action.monitor_id);
        let generation = info.map_or(0, |info| info.generation);
        let range = info.map_or((0.0, 0.0), |info| {
            info.ranges[usize::from(action.kind == "set_volume")]
        });
        let verdict = match info {
            None => Err("disconnected"),
            Some(info) if info.identity_status == "ambiguous" => Err("ambiguous"),
            Some(_) => op_kind(&action).ok_or("unsupported"),
        };
        let profile = self
            .profiles
            .get(&action.monitor_id)
            .copied()
            .unwrap_or_default();
        let feature = feature_of(&action.kind);
        let mut entry = Running {
            action,
            feature,
            connection_generation: generation,
            range,
            op: None,
            state: State::NeedsSubmit,
            outcome: None,
            first_attempt: true,
            resubmits: 0,
        };
        match verdict {
            Ok(kind) => {
                let mut op = Operation::new(kind, profile)
                    .with_ready_until(self.wake_ready_until.filter(|until| now < *until));
                let step = op.start(now);
                entry.op = Some(op);
                apply_step(&mut entry, step);
            }
            Err(name) => {
                entry.outcome = Some(Outcome {
                    ok: false,
                    error: Some(name),
                    value: None,
                });
            }
        }
        self.running.push(entry);
    }

    /// Submits the results that are ready, and starts the first queued primitive when the
    /// execution slot is free. Returns the effects in order.
    fn pump(&mut self, tokens: &mut Tokens) -> Vec<Effect> {
        let mut effects = Vec::new();
        for entry in self.running.iter_mut() {
            if matches!(entry.state, State::NeedsSubmit) {
                let token = tokens.issue();
                effects.push(Effect::SubmitResult {
                    token,
                    result_json: result_json(&entry.action, entry.outcome.as_ref()),
                });
                entry.state = State::Submitting(token);
            }
        }

        if self.exec_busy {
            return effects;
        }
        let mut index = 0;
        while index < self.running.len() {
            let State::NeedsRun(primitive) = &self.running[index].state else {
                index += 1;
                continue;
            };
            let (op, code, value) = primitive_parts(primitive);
            if self.running[index].first_attempt {
                self.running[index].first_attempt = false;
                let monitor_id = self.running[index].action.monitor_id.clone();
                if !self.may_run(&monitor_id) {
                    // Not completed: the next observe or tick offers it again.
                    self.running.remove(index);
                    continue;
                }
            }
            let token = tokens.issue();
            self.exec_busy = true;
            self.running[index].state = State::Executing(token);
            effects.push(Effect::RunAttempt {
                token,
                monitor_id: self.running[index].action.monitor_id.clone(),
                op,
                code,
                value,
            });
            break;
        }
        effects
    }

    /// Moves a finished action out of the running list and records its completion. Only set
    /// actions produce a completion. A rejected result is reported as failed, with no percent.
    fn retire(&mut self, index: usize, accepted: bool) {
        let entry = self.running.remove(index);
        self.completed.insert(entry.action.id);
        let Some(feature) = entry.feature else {
            return;
        };
        let outcome = entry.outcome.as_ref();
        let (ok, observed_percent) = if accepted {
            let ok = outcome.is_some_and(|outcome| outcome.ok);
            let (min, max) = entry.range;
            let percent = match outcome.filter(|outcome| outcome.ok).and_then(|o| o.value) {
                Some(value) if max > min => {
                    Some(((value - min) / (max - min) * 100.0).clamp(0.0, 100.0))
                }
                _ => None,
            };
            (ok, percent)
        } else {
            (false, None)
        };
        self.completions.push(Completion {
            monitor_id: entry.action.monitor_id.clone(),
            feature,
            connection_generation: entry.connection_generation,
            action_generation: entry.action.generation,
            ok,
            observed_percent,
        });
    }
}

/// Stores the next step of an operation on its running entry.
fn apply_step(entry: &mut Running, step: Step) {
    match step {
        Step::Run(primitive) => entry.state = State::NeedsRun(primitive),
        Step::Wait(until) => entry.state = State::Waiting(until),
        Step::Done(outcome) => {
            entry.outcome = Some(outcome);
            entry.state = State::NeedsSubmit;
        }
    }
}

/// Fallback for an entry without an operation. Unreachable: every running entry that reaches
/// a primitive or a timer has one.
fn lost_operation() -> Step {
    Step::Done(Outcome {
        ok: false,
        error: Some("io"),
        value: None,
    })
}

/// The feature that a set action changes. Read actions and unknown kinds have none.
fn feature_of(kind: &str) -> Option<Feature> {
    match kind {
        "set_brightness" => Some(Feature::Brightness),
        "set_volume" => Some(Feature::Volume),
        _ => None,
    }
}

/// The operation for an action. A set action needs a value; any other kind is unsupported.
fn op_kind(action: &PendingActionObs) -> Option<OpKind> {
    match (action.kind.as_str(), action.value) {
        ("set_brightness", Some(value)) => Some(OpKind::SetVerify {
            code: 16,
            target: raw_target(value),
        }),
        ("set_volume", Some(value)) => Some(OpKind::SetVerify {
            code: 98,
            target: raw_target(value),
        }),
        ("read_brightness", _) => Some(OpKind::Read { code: 16 }),
        _ => None,
    }
}

/// Rounds a requested raw value to the nearest integer, clamped to the 32-bit range.
fn raw_target(value: f64) -> i64 {
    value.round().clamp(0.0, u32::MAX as f64) as i64
}

/// The primitive as the shell runs it: the attempt kind, the VCP code and the raw value for a
/// write.
fn primitive_parts(primitive: &Primitive) -> (AttemptOp, u8, Option<f64>) {
    match primitive {
        Primitive::Write { code, value } => (AttemptOp::Write, *code, Some(*value as f64)),
        Primitive::Read { code } => (AttemptOp::Read, *code, None),
    }
}

/// The `hardware_result` that the core accepts. Failures carry no value.
fn result_json(action: &PendingActionObs, outcome: Option<&Outcome>) -> String {
    match outcome.filter(|outcome| outcome.ok) {
        Some(outcome) => json!({
            "type": "hardware_result",
            "action_id": action.id,
            "generation": action.generation,
            "ok": true,
            "value": outcome.value,
        })
        .to_string(),
        None => json!({
            "type": "hardware_result",
            "action_id": action.id,
            "generation": action.generation,
            "ok": false,
            "error": outcome.and_then(|outcome| outcome.error).unwrap_or("io"),
        })
        .to_string(),
    }
}

#[cfg(test)]
mod tests;
