use super::*;
use crate::controls::model::*;
use crate::controls::Tokens;
use serde_json::Value;

/// Connection generation of the test monitors.
const CONNECTION_GEN: u64 = 1;
/// Generation stamped on every pending action. It differs from the connection generation on purpose.
const ACTION_GEN: u64 = 9;

fn channel(observed: f64, min: f64, max: f64) -> ChannelObs {
    ChannelObs {
        supported: true,
        observed: Some(observed),
        min,
        max,
    }
}

/// A monitor with brightness 41 and volume 20 on a 0 to 100 range.
fn mon(id: &str, display_state: &str, identity_status: &str) -> MonitorObs {
    MonitorObs {
        id: id.to_owned(),
        generation: CONNECTION_GEN,
        display_state: display_state.to_owned(),
        identity_status: identity_status.to_owned(),
        brightness: channel(41.0, 0.0, 100.0),
        volume: channel(20.0, 0.0, 100.0),
    }
}

fn m1() -> MonitorObs {
    mon("m1", "awake", "stable")
}

fn m2() -> MonitorObs {
    mon("m2", "awake", "stable")
}

fn asleep_m1() -> MonitorObs {
    mon("m1", "asleep", "stable")
}

fn ambiguous_m1() -> MonitorObs {
    mon("m1", "awake", "ambiguous")
}

/// A monitor with the given brightness and volume channels.
fn mon_range(id: &str, brightness: ChannelObs, volume: ChannelObs) -> MonitorObs {
    MonitorObs {
        brightness,
        volume,
        ..mon(id, "awake", "stable")
    }
}

fn act(id: u64, monitor: &str, kind: &str, value: Option<f64>) -> PendingActionObs {
    PendingActionObs {
        id,
        generation: ACTION_GEN,
        monitor_id: monitor.to_owned(),
        kind: kind.to_owned(),
        value,
    }
}

/// One RunAttempt effect, flattened for assertions.
#[derive(Debug, Clone)]
struct Run {
    token: u64,
    monitor: String,
    op: AttemptOp,
    code: u8,
    value: Option<f64>,
}

fn runs(effects: &[Effect]) -> Vec<Run> {
    effects
        .iter()
        .filter_map(|effect| match effect {
            Effect::RunAttempt {
                token,
                monitor_id,
                op,
                code,
                value,
            } => Some(Run {
                token: *token,
                monitor: monitor_id.clone(),
                op: *op,
                code: *code,
                value: *value,
            }),
            _ => None,
        })
        .collect()
}

fn single_run(effects: &[Effect]) -> Run {
    let mut found = runs(effects);
    assert_eq!(found.len(), 1, "expected one RunAttempt in {effects:?}");
    found.remove(0)
}

/// The only SubmitResult in `effects`: its token and parsed result JSON.
fn single_result(effects: &[Effect]) -> (u64, Value) {
    let mut found: Vec<(u64, Value)> = effects
        .iter()
        .filter_map(|effect| match effect {
            Effect::SubmitResult { token, result_json } => Some((
                *token,
                serde_json::from_str::<Value>(result_json).expect("result_json is JSON"),
            )),
            _ => None,
        })
        .collect();
    assert_eq!(found.len(), 1, "expected one SubmitResult in {effects:?}");
    found.remove(0)
}

/// The error name of the only SubmitResult in `effects`.
fn error_of(effects: &[Effect]) -> String {
    let (_, json) = single_result(effects);
    json["error"]
        .as_str()
        .expect("a failed result names its error")
        .to_owned()
}

fn approx(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() < 1e-9,
        "expected {expected}, got {actual}"
    );
}

fn assert_percent(actual: Option<f64>, expected: f64) {
    approx(actual.expect("a percent is reported"), expected);
}

/// Re-sends a rejected result: advances to the retry, checks that nothing is re-run, and
/// checks that the stored result is sent again unchanged.
fn resubmission(rig: &mut Rig, stored: &Value) -> Vec<Effect> {
    let effects = rig.advance_to_deadline();
    assert!(
        runs(&effects).is_empty(),
        "a rejected result is never re-run"
    );
    assert_eq!(single_result(&effects).1, *stored);
    effects
}

/// The machine under test with its tokens and a clock that the tests move by hand.
struct Rig {
    dispatch: Dispatch,
    tokens: Tokens,
    now: f64,
}

impl Rig {
    fn new() -> Self {
        Self {
            dispatch: Dispatch::new(),
            tokens: Tokens::new(),
            now: 1.0,
        }
    }

    fn observe(
        &mut self,
        revision: u64,
        monitors: Vec<MonitorObs>,
        pending: Vec<PendingActionObs>,
    ) -> Vec<Effect> {
        let observation = Observation {
            revision,
            monitors,
            pending_actions: pending,
        };
        self.dispatch
            .observe(self.now, &mut self.tokens, &observation)
    }

    fn monitor_profile(&mut self, monitor_id: &str, manufacturer: &str) {
        self.dispatch.monitor_profile(monitor_id, manufacturer);
    }

    fn monitor_removed(&mut self, monitor_id: &str) {
        self.dispatch.monitor_removed(monitor_id);
    }

    fn display_power(&mut self, on: bool) -> Vec<Effect> {
        self.dispatch.display_power(self.now, &mut self.tokens, on)
    }

    fn tick(&mut self) -> Vec<Effect> {
        self.dispatch.tick(self.now, &mut self.tokens)
    }

    /// Reports the outcome of the RunAttempt in `effects`.
    fn answer(
        &mut self,
        effects: &[Effect],
        ok: bool,
        error: AttemptError,
        value: Option<f64>,
    ) -> Vec<Effect> {
        let run = single_run(effects);
        self.dispatch
            .on_attempt_done(self.now, &mut self.tokens, run.token, ok, error, value)
    }

    /// Reports whether the core accepted the SubmitResult in `effects`.
    fn respond(&mut self, effects: &[Effect], accepted: bool) -> Vec<Effect> {
        let (token, _) = single_result(effects);
        self.dispatch
            .on_submitted(self.now, &mut self.tokens, token, accepted)
    }

    /// Moves the clock to the next deadline and ticks.
    fn advance_to_deadline(&mut self) -> Vec<Effect> {
        self.now = self.next_deadline().expect("a deadline is armed");
        self.tick()
    }

    fn assert_deadline(&self, expected: f64) {
        approx(self.next_deadline().expect("a deadline is armed"), expected);
    }

    fn next_deadline(&self) -> Option<f64> {
        self.dispatch.next_deadline()
    }

    fn completions(&mut self) -> Vec<Completion> {
        self.dispatch.take_completions()
    }

    fn shutdown(&mut self) {
        self.dispatch.shutdown();
    }

    /// Answers a set write as ok, its readback as `readback`, and accepts the result.
    /// Returns every effect produced after the write, in order.
    fn finish_set(&mut self, write: &[Effect], readback: f64) -> Vec<Effect> {
        let mut effects = self.answer(write, true, AttemptError::None, None);
        let read = self.advance_to_deadline();
        let result = self.answer(&read, true, AttemptError::None, Some(readback));
        effects.extend(read);
        effects.extend(result.clone());
        effects.extend(self.respond(&result, true));
        effects
    }
}

#[test]
fn dispatch_set_brightness_writes_reads_back_and_reports_ok() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    let run = single_run(&write);
    assert_eq!((run.op, run.code), (AttemptOp::Write, 16));
    assert_eq!(run.value, Some(60.0));

    // The write is followed by a 60 ms settle before the readback.
    assert!(rig
        .answer(&write, true, AttemptError::None, None)
        .is_empty());
    rig.assert_deadline(1.06);
    let read = rig.advance_to_deadline();
    let readback = single_run(&read);
    assert_eq!((readback.op, readback.code), (AttemptOp::Read, 16));

    let result = rig.answer(&read, true, AttemptError::None, Some(60.0));
    let (_, json) = single_result(&result);
    assert_eq!(json["type"], "hardware_result");
    assert_eq!(json["action_id"], 7);
    assert_eq!(json["generation"], ACTION_GEN);
    assert_eq!(json["ok"], true);
    assert_eq!(json["value"].as_f64(), Some(60.0));

    rig.respond(&result, true);
    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    let done = &completions[0];
    assert_eq!(done.monitor_id, "m1");
    assert_eq!(done.feature, Feature::Brightness);
    assert_eq!(done.connection_generation, CONNECTION_GEN);
    assert_eq!(done.action_generation, ACTION_GEN);
    assert!(done.ok);
    assert_percent(done.observed_percent, 60.0);
}

#[test]
fn dispatch_failing_readback_reports_io_error_and_no_percent() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    rig.answer(&write, true, AttemptError::None, None);
    let read = rig.advance_to_deadline();

    // The panel still reads 41, not the target 60.
    let result = rig.answer(&read, true, AttemptError::None, Some(41.0));
    let (_, json) = single_result(&result);
    assert_eq!(json["ok"], false);
    assert_eq!(json["error"], "io");
    assert!(matches!(json.get("value"), None | Some(Value::Null)));

    rig.respond(&result, true);
    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    assert!(!completions[0].ok);
    assert_eq!(completions[0].observed_percent, None);
}

#[test]
fn dispatch_read_brightness_reports_raw_value_and_no_completion() {
    let mut rig = Rig::new();
    let read = rig.observe(1, vec![m1()], vec![act(8, "m1", "read_brightness", None)]);
    let run = single_run(&read);
    assert_eq!((run.op, run.code), (AttemptOp::Read, 16));

    let result = rig.answer(&read, true, AttemptError::None, Some(47.0));
    let (_, json) = single_result(&result);
    assert_eq!(json["action_id"], 8);
    assert_eq!(json["ok"], true);
    assert_eq!(json["value"].as_f64(), Some(47.0));

    rig.respond(&result, true);
    assert!(rig.completions().is_empty());
}

#[test]
fn dispatch_same_action_id_in_two_observes_runs_once() {
    let mut rig = Rig::new();
    let pending = vec![act(7, "m1", "set_brightness", Some(60.0))];
    let first = rig.observe(1, vec![m1()], pending.clone());
    assert_eq!(single_run(&first).op, AttemptOp::Write);

    // Still in flight: the same id in a newer snapshot starts nothing.
    assert!(runs(&rig.observe(2, vec![m1()], pending.clone())).is_empty());

    rig.finish_set(&first, 60.0);
    assert_eq!(rig.completions().len(), 1);

    // Completed: a newer snapshot that still lists the id starts nothing.
    assert!(runs(&rig.observe(3, vec![m1()], pending)).is_empty());
}

#[test]
fn dispatch_asleep_monitor_gets_no_hardware_call_and_no_result() {
    let mut rig = Rig::new();
    let effects = rig.observe(
        1,
        vec![asleep_m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    assert!(effects.is_empty());

    rig.now = 10.0;
    assert!(rig.tick().is_empty());
    assert!(rig.completions().is_empty());
}

#[test]
fn dispatch_display_off_holds_actions_and_display_on_holds_for_the_grace_period() {
    let mut rig = Rig::new();
    rig.observe(1, vec![m1()], vec![]);

    rig.display_power(false);
    let held = rig.observe(
        2,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    assert!(runs(&held).is_empty());
    assert!(runs(&rig.tick()).is_empty());

    // Waking holds the action for 2.0 s after the display-on event at t = 1.0.
    rig.display_power(true);
    rig.now = 1.0 + 1.999;
    assert!(runs(&rig.tick()).is_empty());
    rig.assert_deadline(3.0);

    let released = rig.advance_to_deadline();
    assert_eq!(single_run(&released).op, AttemptOp::Write);
}

#[test]
fn dispatch_unknown_monitor_fails_with_disconnected_and_ambiguous_with_ambiguous_and_unknown_kind_with_unsupported(
) {
    let mut rig = Rig::new();

    // A monitor absent from the snapshot: disconnected, with no hardware call.
    let gone = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "gone", "set_brightness", Some(60.0))],
    );
    assert!(runs(&gone).is_empty());
    assert_eq!(error_of(&gone), "disconnected");
    rig.respond(&gone, true);

    // An ambiguous identity is never written or read.
    let ambiguous = rig.observe(
        2,
        vec![ambiguous_m1()],
        vec![act(8, "m1", "set_brightness", Some(60.0))],
    );
    assert!(runs(&ambiguous).is_empty());
    assert_eq!(error_of(&ambiguous), "ambiguous");
    rig.respond(&ambiguous, true);

    // An unknown kind is unsupported.
    let unknown = rig.observe(3, vec![m1()], vec![act(9, "m1", "frobnicate", None)]);
    assert!(runs(&unknown).is_empty());
    assert_eq!(error_of(&unknown), "unsupported");
    rig.respond(&unknown, true);

    // A set without a value is unsupported.
    let no_value = rig.observe(4, vec![m1()], vec![act(10, "m1", "set_volume", None)]);
    assert!(runs(&no_value).is_empty());
    assert_eq!(error_of(&no_value), "unsupported");
}

#[test]
fn dispatch_permanent_port_errors_are_reported_without_retry() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    let result = rig.answer(&write, false, AttemptError::Unsupported, None);
    assert_eq!(error_of(&result), "unsupported");
    rig.respond(&result, true);

    // No retry follows a permanent error.
    rig.now += 5.0;
    assert!(runs(&rig.tick()).is_empty());

    let write = rig.observe(
        2,
        vec![m1()],
        vec![act(8, "m1", "set_brightness", Some(60.0))],
    );
    let result = rig.answer(&write, false, AttemptError::Disconnected, None);
    assert_eq!(error_of(&result), "disconnected");
    rig.respond(&result, true);
    assert!(runs(&rig.tick()).is_empty());
}

#[test]
fn dispatch_transient_write_error_is_retried_after_200ms() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    let first = single_run(&write);

    assert!(rig.answer(&write, false, AttemptError::Io, None).is_empty());
    rig.assert_deadline(1.2);

    let retry = rig.advance_to_deadline();
    let again = single_run(&retry);
    assert_eq!((again.op, again.value), (AttemptOp::Write, Some(60.0)));
    assert_eq!(again.code, first.code);
}

#[test]
fn dispatch_one_primitive_runs_at_a_time_across_monitors() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1(), m2()],
        vec![
            act(7, "m1", "set_brightness", Some(60.0)),
            act(8, "m2", "set_brightness", Some(30.0)),
        ],
    );
    let first = single_run(&write);
    assert_eq!(
        (first.monitor.as_str(), first.op, first.value),
        ("m1", AttemptOp::Write, Some(60.0))
    );

    // m1's write is outstanding, so the slot is busy.
    assert!(runs(&rig.tick()).is_empty());

    // m1 settles for 60 ms, which frees the slot: m2's write starts at once.
    let second_write = rig.answer(&write, true, AttemptError::None, None);
    let second = single_run(&second_write);
    assert_eq!(
        (second.monitor.as_str(), second.op, second.value),
        ("m2", AttemptOp::Write, Some(30.0))
    );

    // m2's write is outstanding when m1's settle ends: the slot stays busy.
    rig.assert_deadline(1.06);
    assert!(runs(&rig.advance_to_deadline()).is_empty());

    // m2 settles, and m1's readback is first in line.
    let readback_effects = rig.answer(&second_write, true, AttemptError::None, None);
    let readback = single_run(&readback_effects);
    assert_eq!(
        (readback.monitor.as_str(), readback.op),
        ("m1", AttemptOp::Read)
    );
}

#[test]
fn dispatch_one_action_per_monitor() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![
            act(7, "m1", "set_brightness", Some(60.0)),
            act(8, "m1", "set_brightness", Some(30.0)),
        ],
    );
    assert_eq!(single_run(&write).value, Some(60.0));

    // The second action of the same monitor waits until the first is accepted.
    assert!(rig
        .answer(&write, true, AttemptError::None, None)
        .is_empty());
    let read = rig.advance_to_deadline();
    assert_eq!(single_run(&read).op, AttemptOp::Read);
    let result = rig.answer(&read, true, AttemptError::None, Some(60.0));
    assert!(runs(&result).is_empty());

    rig.respond(&result, true);
    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    assert_percent(completions[0].observed_percent, 60.0);
}

#[test]
fn dispatch_second_action_starts_after_the_first_is_accepted_without_a_new_observe() {
    let mut rig = Rig::new();
    let read = rig.observe(
        1,
        vec![m1(), m2()],
        vec![
            act(7, "m1", "read_brightness", None),
            act(8, "m2", "set_brightness", Some(30.0)),
        ],
    );
    assert_eq!(single_run(&read).monitor, "m1");

    let result = rig.answer(&read, true, AttemptError::None, Some(41.0));
    let mut later = result.clone();
    later.extend(rig.respond(&result, true));

    // No observe is sent after the first action; the second one starts from the stored list.
    assert!(runs(&later)
        .iter()
        .any(|run| run.monitor == "m2" && run.op == AttemptOp::Write && run.value == Some(30.0)));
}

#[test]
fn dispatch_rejected_result_is_resubmitted_three_times_with_backoff_and_never_rewrites() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    rig.answer(&write, true, AttemptError::None, None);
    let read = rig.advance_to_deadline();
    let mut result = rig.answer(&read, true, AttemptError::None, Some(60.0));
    let stored = single_result(&result).1;

    // Rejection 1 at t: resubmitted after 0.2 s.
    let t = rig.now;
    assert!(rig.respond(&result, false).is_empty());
    rig.assert_deadline(t + 0.2);
    result = resubmission(&mut rig, &stored);

    // Rejection 2 at t + 0.2: resubmitted after 1.0 s.
    assert!(rig.respond(&result, false).is_empty());
    rig.assert_deadline(t + 1.2);
    result = resubmission(&mut rig, &stored);

    // Rejection 3 at t + 1.2: resubmitted after 1.0 s.
    assert!(rig.respond(&result, false).is_empty());
    rig.assert_deadline(t + 2.2);
    result = resubmission(&mut rig, &stored);

    // Rejection 4 at t + 2.2: the budget is spent, so the action fails.
    assert!(rig.respond(&result, false).is_empty());
    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    assert!(!completions[0].ok);
    assert_eq!(completions[0].observed_percent, None);
}

#[test]
fn dispatch_percent_comes_from_the_observed_range() {
    let mut rig = Rig::new();
    let monitor = mon_range("m1", channel(128.0, 0.0, 255.0), channel(20.0, 0.0, 100.0));
    let write = rig.observe(
        1,
        vec![monitor],
        vec![act(7, "m1", "set_brightness", Some(128.0))],
    );
    assert_eq!(single_run(&write).code, 16);
    rig.finish_set(&write, 128.0);
    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    assert_percent(completions[0].observed_percent, 128.0 / 255.0 * 100.0);

    // Volume reads and writes VCP 98 and uses its own observed range.
    let mut rig = Rig::new();
    let monitor = mon_range("m1", channel(41.0, 0.0, 100.0), channel(128.0, 0.0, 255.0));
    let write = rig.observe(
        1,
        vec![monitor],
        vec![act(9, "m1", "set_volume", Some(128.0))],
    );
    let run = single_run(&write);
    assert_eq!((run.op, run.code), (AttemptOp::Write, 98));
    rig.answer(&write, true, AttemptError::None, None);
    let read = rig.advance_to_deadline();
    assert_eq!(single_run(&read).code, 98);
    let result = rig.answer(&read, true, AttemptError::None, Some(128.0));
    rig.respond(&result, true);
    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    assert_eq!(completions[0].feature, Feature::Volume);
    assert_percent(completions[0].observed_percent, 128.0 / 255.0 * 100.0);
}

#[test]
fn dispatch_gsm_profile_duplicates_the_write() {
    let mut rig = Rig::new();
    rig.monitor_profile("m1", "GSM");
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    assert_eq!(single_run(&write).op, AttemptOp::Write);

    // The duplicate follows 10 ms after the first write.
    assert!(rig
        .answer(&write, true, AttemptError::None, None)
        .is_empty());
    rig.assert_deadline(1.01);
    let duplicate = rig.advance_to_deadline();
    let again = single_run(&duplicate);
    assert_eq!((again.op, again.value), (AttemptOp::Write, Some(60.0)));

    // The readback follows the 60 ms settle after the duplicate.
    assert!(rig
        .answer(&duplicate, true, AttemptError::None, None)
        .is_empty());
    rig.assert_deadline(1.07);
    let read = rig.advance_to_deadline();
    assert_eq!(single_run(&read).op, AttemptOp::Read);
}

#[test]
fn dispatch_monitor_removed_forgets_the_profile() {
    let mut rig = Rig::new();
    rig.monitor_profile("m1", "GSM");
    rig.monitor_removed("m1");
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    assert_eq!(single_run(&write).op, AttemptOp::Write);

    // Without the GSM profile there is no duplicate: the readback follows the 60 ms settle.
    assert!(rig
        .answer(&write, true, AttemptError::None, None)
        .is_empty());
    rig.assert_deadline(1.06);
    let read = rig.advance_to_deadline();
    assert_eq!(single_run(&read).op, AttemptOp::Read);
}

#[test]
fn dispatch_stale_observe_is_ignored() {
    let mut rig = Rig::new();
    rig.observe(5, vec![asleep_m1()], vec![]);

    // The lower revision would wake the monitor and start the write; it is ignored.
    let stale = rig.observe(
        4,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    assert!(runs(&stale).is_empty());
}

#[test]
fn dispatch_equal_revision_is_applied() {
    let mut rig = Rig::new();
    rig.observe(5, vec![asleep_m1()], vec![]);

    // The same revision wakes the monitor, so the listed action runs.
    let applied = rig.observe(
        5,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    assert_eq!(single_run(&applied).op, AttemptOp::Write);
}

#[test]
fn dispatch_shutdown_makes_everything_inert() {
    // A late attempt result and a late observe do nothing.
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    rig.shutdown();
    assert!(rig
        .answer(&write, true, AttemptError::None, None)
        .is_empty());
    rig.now = 5.0;
    assert!(rig.tick().is_empty());
    assert!(rig
        .observe(
            2,
            vec![m1()],
            vec![act(8, "m1", "set_brightness", Some(30.0))],
        )
        .is_empty());
    assert_eq!(rig.next_deadline(), None);

    // A result submitted before shutdown is not completed afterwards.
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    rig.answer(&write, true, AttemptError::None, None);
    let read = rig.advance_to_deadline();
    let result = rig.answer(&read, true, AttemptError::None, Some(60.0));
    rig.shutdown();
    assert!(rig.respond(&result, true).is_empty());
    assert!(rig.completions().is_empty());
}

#[test]
fn dispatch_completed_ids_are_pruned_when_no_longer_listed() {
    let mut rig = Rig::new();
    let pending = vec![act(7, "m1", "set_brightness", Some(60.0))];
    let write = rig.observe(1, vec![m1()], pending.clone());
    rig.finish_set(&write, 60.0);
    assert_eq!(rig.completions().len(), 1);

    // The core stops listing the action, so its completed record is dropped.
    assert!(runs(&rig.observe(2, vec![m1()], vec![])).is_empty());

    // Listed again, the same id is run again.
    let again = rig.observe(3, vec![m1()], pending);
    assert_eq!(single_run(&again).op, AttemptOp::Write);
}

#[test]
fn dispatch_failed_set_reports_completion_without_percent() {
    let mut rig = Rig::new();
    let write = rig.observe(
        1,
        vec![m1()],
        vec![act(7, "m1", "set_brightness", Some(60.0))],
    );
    let result = rig.answer(&write, false, AttemptError::Unsupported, None);
    assert_eq!(error_of(&result), "unsupported");
    rig.respond(&result, true);

    let completions = rig.completions();
    assert_eq!(completions.len(), 1);
    assert_eq!(completions[0].feature, Feature::Brightness);
    assert!(!completions[0].ok);
    assert_eq!(completions[0].observed_percent, None);
}

#[test]
fn dispatch_profile_map_is_bounded() {
    let mut d = Dispatch::new();
    for i in 0..(MAX_MONITORS + 10) {
        d.monitor_profile(&format!("m{i}"), "GSM");
    }
    assert_eq!(
        d.profiles.len(),
        MAX_MONITORS,
        "the profile map stops at the cap"
    );

    // With no snapshot every stored monitor is evictable: the first key in order goes, and the
    // newest id is stored.
    assert!(!d.profiles.contains_key("m0"));
    assert!(d.profiles.contains_key("m73"));

    // An id that is already stored can still change its profile at the cap.
    d.monitor_profile("m73", "DEL");
    assert!(!d.profiles["m73"].duplicate_set);
    assert_eq!(d.profiles.len(), MAX_MONITORS);
}

#[test]
fn dispatch_profile_eviction_keeps_profiles_of_monitors_in_the_snapshot() {
    let mut rig = Rig::new();
    let monitors: Vec<MonitorObs> = (0..MAX_MONITORS)
        .map(|i| mon(&format!("keep-{i}"), "awake", "stable"))
        .collect();
    rig.observe(1, monitors, vec![]);
    for i in 0..MAX_MONITORS {
        rig.monitor_profile(&format!("keep-{i}"), "GSM");
    }

    // Every stored monitor is listed by the snapshot, so the new id finds nothing to evict.
    rig.monitor_profile("extra", "GSM");
    for i in 0..MAX_MONITORS {
        let id = format!("keep-{i}");
        assert!(
            rig.dispatch.profiles.contains_key(&id),
            "{id} keeps its profile"
        );
    }
    assert!(!rig.dispatch.profiles.contains_key("extra"));
}
