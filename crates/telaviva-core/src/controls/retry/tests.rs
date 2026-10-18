use super::*;
use crate::controls::model::AttemptError;

// Scripted answer for one attempt: (ok, error, value).
type Answer = (bool, AttemptError, Option<f64>);

// Answers are consumed in order; running out is a test bug.
struct Script {
    answers: Vec<Answer>,
}

impl Script {
    fn next(&mut self) -> Answer {
        assert!(!self.answers.is_empty(), "script ran out of answers");
        self.answers.remove(0)
    }
}

fn wrote() -> Answer {
    (true, AttemptError::None, None)
}

fn read_ok(value: f64) -> Answer {
    (true, AttemptError::None, Some(value))
}

fn fail(error: AttemptError) -> Answer {
    (false, error, None)
}

fn near(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() < 1e-9,
        "expected {expected}, got {actual}"
    );
}

fn expect_done(step: Step) -> Outcome {
    match step {
        Step::Done(outcome) => outcome,
        _ => panic!("expected Step::Done"),
    }
}

fn sleeps(trace: &[String]) -> Vec<&str> {
    trace
        .iter()
        .filter(|event| event.starts_with("sleep "))
        .map(|event| event.as_str())
        .collect()
}

// Drives the machine with a fake clock and records every primitive and wait.
fn run(kind: OpKind, profile: Profile, answers: Vec<Answer>) -> (Outcome, Vec<String>) {
    let mut script = Script { answers };
    let mut op = Operation::new(kind, profile);
    let mut trace = Vec::new();
    let mut now = 0.0;
    let mut step = op.start(now);
    loop {
        match step {
            Step::Run(primitive) => {
                trace.push(match primitive {
                    Primitive::Write { code, value } => format!("write {}={}", code, value),
                    Primitive::Read { code } => format!("read {}", code),
                });
                let (ok, error, value) = script.next();
                step = op.on_attempt_done(now, ok, error, value);
            }
            Step::Wait(target) => {
                trace.push(format!(
                    "sleep {}",
                    ((target - now) * 1000.0).round() as i64
                ));
                now = target;
                step = op.on_timer(now);
            }
            Step::Done(outcome) => return (outcome, trace),
        }
    }
}

// Runs a stuck read whose three attempts all fail with `error`.
fn run_read_failing(error: AttemptError) -> (Outcome, Vec<String>) {
    run(
        OpKind::Read { code: 16 },
        Profile::default(),
        vec![fail(error), fail(error), fail(error)],
    )
}

#[test]
fn retry_set_writes_waits_then_reads_back() {
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![wrote(), read_ok(60.0)],
    );
    assert_eq!(trace, ["write 16=60", "sleep 60", "read 16"]);
    assert!(outcome.ok);
    assert_eq!(outcome.error, None);
    near(outcome.value.expect("readback value"), 60.0);
}

#[test]
fn retry_set_sleeps_exactly_the_settle_delay() {
    let (_, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![wrote(), read_ok(60.0)],
    );
    assert_eq!(sleeps(&trace), ["sleep 60"]);
}

#[test]
fn retry_gsm_profile_duplicates_the_set() {
    let profile = Profile {
        duplicate_set: true,
    };
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        profile,
        vec![wrote(), wrote(), read_ok(60.0)],
    );
    assert_eq!(
        trace,
        [
            "write 16=60",
            "sleep 10",
            "write 16=60",
            "sleep 60",
            "read 16",
        ]
    );
    assert!(outcome.ok);
    near(outcome.value.expect("readback value"), 60.0);

    assert!(Profile::for_manufacturer("GSM").duplicate_set);
    assert!(Profile::for_manufacturer("gsm").duplicate_set);
    assert!(Profile::for_manufacturer(" GSM ").duplicate_set);
    assert!(!Profile::for_manufacturer("DEL").duplicate_set);
    assert!(!Profile::for_manufacturer("").duplicate_set);
}

#[test]
fn retry_readback_mismatch_reports_observed_value_as_io_without_retry() {
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![wrote(), read_ok(61.0)],
    );
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("io"));
    near(outcome.value.expect("observed value"), 61.0);
    assert_eq!(
        trace
            .iter()
            .filter(|event| event.starts_with("read"))
            .count(),
        1
    );
}

#[test]
fn retry_transient_write_failure_is_retried_after_200ms() {
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![fail(AttemptError::Io), wrote(), read_ok(60.0)],
    );
    assert_eq!(
        trace,
        [
            "write 16=60",
            "sleep 200",
            "write 16=60",
            "sleep 60",
            "read 16",
        ]
    );
    assert!(outcome.ok);
}

#[test]
fn retry_write_failures_exhaust_three_attempts_without_readback() {
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![
            fail(AttemptError::Io),
            fail(AttemptError::Io),
            fail(AttemptError::Io),
        ],
    );
    assert_eq!(
        trace,
        [
            "write 16=60",
            "sleep 200",
            "write 16=60",
            "sleep 1000",
            "write 16=60",
        ]
    );
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("io"));
    assert!(outcome.value.is_none());
    assert!(!trace.iter().any(|event| event.starts_with("read")));
}

#[test]
fn retry_write_unsupported_and_disconnected_stop_after_one_attempt() {
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![fail(AttemptError::Unsupported)],
    );
    assert_eq!(trace, ["write 16=60"]);
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("unsupported"));

    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![fail(AttemptError::Disconnected)],
    );
    assert_eq!(trace, ["write 16=60"]);
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("disconnected"));
}

#[test]
fn retry_read_returns_current_value_without_sleeping() {
    let (outcome, trace) = run(
        OpKind::Read { code: 16 },
        Profile::default(),
        vec![read_ok(41.0)],
    );
    assert_eq!(trace, ["read 16"]);
    assert!(outcome.ok);
    assert_eq!(outcome.error, None);
    near(outcome.value.expect("current value"), 41.0);
}

#[test]
fn retry_read_retries_after_200ms_then_succeeds() {
    let (outcome, trace) = run(
        OpKind::Read { code: 16 },
        Profile::default(),
        vec![fail(AttemptError::Io), read_ok(41.0)],
    );
    assert_eq!(trace, ["read 16", "sleep 200", "read 16"]);
    assert!(outcome.ok);
    near(outcome.value.expect("current value"), 41.0);
}

#[test]
fn retry_two_read_failures_back_off_200_then_1000_and_report_last_error() {
    let (outcome, trace) = run_read_failing(AttemptError::Io);
    assert_eq!(
        trace,
        ["read 16", "sleep 200", "read 16", "sleep 1000", "read 16"]
    );
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("io"));
    assert!(outcome.value.is_none());
}

#[test]
fn retry_read_unsupported_and_disconnected_are_not_retried() {
    let (outcome, trace) = run(
        OpKind::Read { code: 16 },
        Profile::default(),
        vec![fail(AttemptError::Unsupported)],
    );
    assert_eq!(trace, ["read 16"]);
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("unsupported"));

    let (outcome, trace) = run(
        OpKind::Read { code: 16 },
        Profile::default(),
        vec![fail(AttemptError::Disconnected)],
    );
    assert_eq!(trace, ["read 16"]);
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("disconnected"));
}

#[test]
fn retry_error_none_is_treated_as_a_transient_io_failure() {
    let (outcome, trace) = run_read_failing(AttemptError::None);
    assert_eq!(
        trace,
        ["read 16", "sleep 200", "read 16", "sleep 1000", "read 16"]
    );
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("io"));
}

#[test]
fn retry_ok_read_without_a_value_is_a_transient_failure() {
    let (outcome, trace) = run(
        OpKind::Read { code: 16 },
        Profile::default(),
        vec![(true, AttemptError::None, None), read_ok(41.0)],
    );
    assert_eq!(trace, ["read 16", "sleep 200", "read 16"]);
    assert!(outcome.ok);
    near(outcome.value.expect("current value"), 41.0);
}

#[test]
fn retry_read_failure_inside_set_verify_reports_the_error_without_value() {
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        Profile::default(),
        vec![
            wrote(),
            fail(AttemptError::Io),
            fail(AttemptError::Io),
            fail(AttemptError::Io),
        ],
    );
    assert_eq!(
        trace,
        [
            "write 16=60",
            "sleep 60",
            "read 16",
            "sleep 200",
            "read 16",
            "sleep 1000",
            "read 16",
        ]
    );
    assert!(!outcome.ok);
    assert_eq!(outcome.error, Some("io"));
    assert!(outcome.value.is_none());
}

#[test]
fn retry_budget_is_per_primitive() {
    let profile = Profile {
        duplicate_set: true,
    };
    let (outcome, trace) = run(
        OpKind::SetVerify {
            code: 16,
            target: 60,
        },
        profile,
        vec![
            fail(AttemptError::Io),
            wrote(),
            fail(AttemptError::Io),
            wrote(),
            fail(AttemptError::Io),
            read_ok(60.0),
        ],
    );
    assert_eq!(
        trace,
        [
            "write 16=60",
            "sleep 200",
            "write 16=60",
            "sleep 10",
            "write 16=60",
            "sleep 200",
            "write 16=60",
            "sleep 60",
            "read 16",
            "sleep 200",
            "read 16",
        ]
    );
    assert!(outcome.ok);
    near(outcome.value.expect("readback value"), 60.0);
}

#[test]
fn retry_default_profile_matches_the_documented_timing() {
    near(SETTLE_SECS, 0.06);
    near(DUPLICATE_GAP_SECS, 0.01);
    assert_eq!(BACKOFF_SECS.len(), 2);
    near(BACKOFF_SECS[0], 0.2);
    near(BACKOFF_SECS[1], 1.0);
    near(WAKE_GRACE_SECS, 2.0);
    assert_eq!(RESULT_RESUBMITS, 3);
    assert_eq!(ATTEMPTS_PER_PRIMITIVE, 3);
    assert!(!Profile::default().duplicate_set);
}

#[test]
fn retry_worst_case_wait_of_a_stuck_read_is_1_2_seconds() {
    let (outcome, trace) = run_read_failing(AttemptError::Io);
    let total_ms: i64 = sleeps(&trace)
        .iter()
        .map(|event| {
            event
                .strip_prefix("sleep ")
                .unwrap()
                .parse::<i64>()
                .unwrap()
        })
        .sum();
    assert_eq!(total_ms, 1200);
    near(total_ms as f64 / 1000.0, 1.2);
    assert!(!outcome.ok);
}

#[test]
fn retry_misuse_does_not_panic() {
    // on_timer with no wait pending.
    let mut op = Operation::new(OpKind::Read { code: 16 }, Profile::default());
    let step = op.start(0.0);
    assert!(matches!(step, Step::Run(_)));
    let outcome = expect_done(op.on_timer(0.0));
    assert!(!outcome.ok);

    // on_attempt_done after the operation already finished.
    let mut op = Operation::new(OpKind::Read { code: 16 }, Profile::default());
    let _ = op.start(0.0);
    let outcome = expect_done(op.on_attempt_done(0.0, false, AttemptError::Unsupported, None));
    assert!(!outcome.ok);
    let outcome = expect_done(op.on_attempt_done(0.0, true, AttemptError::None, Some(1.0)));
    assert!(!outcome.ok);
}
