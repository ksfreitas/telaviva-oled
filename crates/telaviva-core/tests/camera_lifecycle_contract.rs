//! Contract tests for `telaviva_core::camera_lifecycle`.
//!
//! The fake-clock tests reproduce `native/macos/Tests/CameraLifecycleTests.swift` lines 26-175.
//! Each test starts from its own lifecycle and advances a plain `f64` clock itself, because the
//! lifecycle never reads time. The JSON tests drive the same behavior through `step`.

use serde_json::{json, Value};
use telaviva_core::camera_lifecycle::{CameraLifecycle, FrameDecision, StepError, MAX_EVENT_BYTES};
use telaviva_core::ffi::{self, TvBuffer, TvCameraLifecycle};

/// Mirrors the Swift `FakeSession`: a start or stop is counted only when it changes the state.
#[derive(Default)]
struct FakeSession {
    starts: u32,
    stops: u32,
    running: bool,
}

impl FakeSession {
    fn start(&mut self) {
        if !self.running {
            self.running = true;
            self.starts += 1;
        }
    }

    fn stop(&mut self) {
        if self.running {
            self.running = false;
            self.stops += 1;
        }
    }
}

/// Every member of the `state` object in a step result, and nothing else.
const STATE_FIELDS: [&str; 12] = [
    "generation",
    "camera_id",
    "mode",
    "capture_mode",
    "always_on",
    "eligible",
    "acquisition_pending",
    "should_keep_session",
    "settling_until",
    "assessment_deadline",
    "acquisition_deadline",
    "assessment_token",
];

/// Builds a `reconcile` event.
fn reconcile_event(
    mode: &str,
    capture_mode: &str,
    generation: u64,
    camera_id: Option<&str>,
    always_on: bool,
) -> Value {
    json!({
        "type": "reconcile",
        "mode": mode,
        "capture_mode": capture_mode,
        "generation": generation,
        "camera_id": camera_id,
        "always_on": always_on,
    })
}

/// Sends one event through the JSON adapter and returns the parsed result object.
fn step_json(lifecycle: &mut CameraLifecycle, event: &Value) -> Value {
    let bytes = serde_json::to_vec(event).unwrap();
    let result = lifecycle.step(&bytes).unwrap();
    serde_json::from_str(&result).unwrap()
}

/// Returns the `value` member of a step result.
fn value_of(result: &Value) -> &Value {
    &result["value"]
}

/// Checks that a result holds exactly `value` and `state`, and that `state` holds exactly the
/// contract fields.
fn assert_result_shape(result: &Value) {
    let mut top: Vec<&str> = result
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect();
    top.sort_unstable();
    assert_eq!(
        top,
        ["state", "value"],
        "result must hold only value and state"
    );

    let mut fields: Vec<&str> = result["state"]
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect();
    fields.sort_unstable();
    let mut expected = STATE_FIELDS.to_vec();
    expected.sort_unstable();
    assert_eq!(
        fields, expected,
        "state must hold exactly the contract fields"
    );
}

/// Runs a script of events on a fresh lifecycle and returns every raw result and the final state.
fn run_script(script: &[Value]) -> (Vec<String>, CameraLifecycle) {
    let mut lifecycle = CameraLifecycle::new();
    let mut results = Vec::new();
    for event in script {
        let bytes = serde_json::to_vec(event).unwrap();
        results.push(lifecycle.step(&bytes).unwrap());
    }
    (results, lifecycle)
}

/// Asserts that `input` is rejected and that the lifecycle is left equal to its earlier state.
fn expect_rejection(lifecycle: &mut CameraLifecycle, input: &[u8]) -> StepError {
    let before = lifecycle.clone();
    let error = lifecycle.step(input).expect_err("input should be rejected");
    assert_eq!(
        *lifecycle, before,
        "a rejected event must not change the state"
    );
    error
}

#[test]
fn same_camera_transitions_reuse_session_and_reject_stale_generations() {
    let mut policy = CameraLifecycle::new();
    let mut session = FakeSession::default();

    let stop = policy.reconcile("face", "continuous_monitoring", 1, Some("camera-a"), true);
    assert!(!stop, "inactive policy should not require a stop");
    assert!(
        policy.should_keep_session(),
        "eligible always-on mode should retain capture"
    );
    session.start();

    let stop = policy.reconcile(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    assert!(
        !stop && policy.should_keep_session(),
        "eligible transitions must keep the session"
    );
    assert_eq!(
        session.starts, 1,
        "eligible transitions must reuse the session"
    );
    assert!(
        policy.accepts_callback(2),
        "new generation should be accepted"
    );
    assert!(
        !policy.accepts_callback(1),
        "old generation callback must be stale"
    );

    let first_token = policy.assessment_token();
    policy.begin_light_assessment(0.0);
    assert!(
        !policy.is_current_assessment(first_token),
        "superseded timer must be stale"
    );
    let latest_token = policy.assessment_token();
    assert!(
        policy.is_current_assessment(latest_token),
        "latest assessment timer is current"
    );
}

#[test]
fn warmup_frames_are_discarded_and_post_settle_frame_assesses_light() {
    let mut policy = CameraLifecycle::new();
    policy.reconcile(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    let mut now = 0.0_f64;

    policy.begin_acquisition(now);
    policy.begin_light_assessment(now);
    let canceled_token = policy.assessment_token();
    now += 4.9;
    assert_eq!(
        policy.frame(now),
        FrameDecision::DiscardWarmup,
        "warmup frames are discarded"
    );
    policy.acquired_frame();
    now += 0.1;
    assert!(
        !policy.timed_out(now),
        "warmup must not extend the acquisition watchdog"
    );
    now += 4.9;
    assert_eq!(
        policy.frame(now),
        FrameDecision::AssessLight,
        "post-settle frame assesses light"
    );
    assert!(
        !policy.timed_out(now),
        "a successful assessment clears the post-settle watchdog"
    );
    assert!(
        !policy.is_current_assessment(canceled_token),
        "completed assessment ends its timer"
    );
}

#[test]
fn each_light_assessment_gets_a_fresh_settle_window() {
    let mut policy = CameraLifecycle::new();
    policy.reconcile(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    let mut now = 10.0_f64;

    policy.begin_light_assessment(now);
    now += 5.0;
    assert_eq!(
        policy.frame(now),
        FrameDecision::AssessLight,
        "first usable-light opportunity"
    );

    now = 20.0;
    policy.begin_light_assessment(now);
    assert_eq!(
        policy.settling_until(),
        Some(25.0),
        "a new assessment settles for five seconds"
    );
    now += 5.0;
    assert_eq!(
        policy.frame(now),
        FrameDecision::AssessLight,
        "second usable-light opportunity"
    );
}

#[test]
fn frozen_post_warmup_stream_times_out_and_acquisition_keeps_five_seconds() {
    let mut policy = CameraLifecycle::new();
    policy.reconcile(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    let mut now = 10.0_f64;

    // Settles until 15 and expects a post-settle frame by 20.
    policy.begin_light_assessment(now);
    now += 5.0;
    assert!(
        !policy.timed_out(now),
        "no timeout before the post-settle deadline"
    );
    now += 5.0;
    assert!(
        policy.timed_out(now),
        "frozen post-warmup stream must time out"
    );

    policy.begin_acquisition(now);
    now += 5.0;
    assert!(
        policy.timed_out(now),
        "initial acquisition keeps its independent five-second limit"
    );
}

#[test]
fn delayed_pre_settle_frame_cannot_cancel_the_fresh_post_settle_watchdog() {
    let mut frozen = CameraLifecycle::new();
    frozen.reconcile(
        "light_only",
        "continuous_monitoring",
        20,
        Some("camera-f"),
        true,
    );
    frozen.begin_acquisition(0.0);
    frozen.begin_light_assessment(5.0);
    frozen.acquired_frame();
    assert_eq!(
        frozen.frame(9.9),
        FrameDecision::DiscardWarmup,
        "late warmup frame is discarded"
    );
    assert!(
        frozen.timed_out(15.0),
        "a discarded warmup frame leaves the watchdog armed"
    );

    frozen.begin_light_assessment(20.0);
    assert_eq!(
        frozen.frame(30.0),
        FrameDecision::AssessmentExpired,
        "late frame must fail"
    );
    let invalidated_token = frozen.assessment_token();
    frozen.cancel_deadlines();
    assert!(
        !frozen.is_current_assessment(invalidated_token),
        "cancelled timer cannot be current"
    );
}

#[test]
fn suspension_stops_the_session_and_cancels_pending_timers() {
    let mut policy = CameraLifecycle::new();
    let mut session = FakeSession::default();
    policy.reconcile("face", "continuous_monitoring", 2, Some("camera-a"), true);
    session.start();
    policy.reconcile("face", "continuous_monitoring", 3, Some("camera-a"), true);
    session.start();
    policy.begin_light_assessment(0.0);

    let stopped = policy.reconcile("suspended", "stopped", 4, Some("camera-a"), true);
    if stopped {
        session.stop();
    }
    assert!(
        stopped && !session.running && session.stops == 1,
        "suspension stops the session"
    );
    assert!(
        policy.settling_until().is_none(),
        "suspension cancels the settle window"
    );
    assert!(
        policy.assessment_deadline().is_none(),
        "suspension cancels the assessment deadline"
    );
    assert!(
        !policy.accepts_callback(4) && !policy.accepts_callback(3),
        "suspended callbacks"
    );
}

#[test]
fn camera_replacement_stops_the_prior_session() {
    let mut policy = CameraLifecycle::new();
    let mut session = FakeSession::default();
    policy.reconcile("face", "continuous_monitoring", 5, Some("camera-a"), true);
    session.start();

    let replaced = policy.reconcile("face", "continuous_monitoring", 6, Some("camera-b"), true);
    if replaced {
        session.stop();
    }
    assert!(
        replaced && !session.running,
        "camera replacement must stop the prior session"
    );

    let stop = policy.reconcile("face", "continuous_monitoring", 7, Some("camera-b"), true);
    assert!(!stop, "the replacement camera's session is retained");
    session.start();
    assert_eq!(session.starts, 2, "the replacement session starts once");
}

#[test]
fn shutdown_stops_the_retained_session() {
    let mut policy = CameraLifecycle::new();
    let mut session = FakeSession::default();
    policy.reconcile("face", "continuous_monitoring", 7, Some("camera-b"), true);
    session.start();

    let shutdown = policy.reconcile("disabled", "stopped", 8, Some("camera-b"), true);
    if shutdown {
        session.stop();
    }
    assert!(
        shutdown && session.stops == 1 && !session.running,
        "shutdown stops the session"
    );
    assert!(
        !policy.should_keep_session(),
        "a disabled policy keeps no session"
    );
}

#[test]
fn identical_return_snapshot_keeps_the_session_and_generation_retarget_survives() {
    let mut policy = CameraLifecycle::new();
    let mut session = FakeSession::default();
    policy.reconcile("face", "continuous_return", 10, Some("camera-r"), false);
    session.start();

    let unchanged = policy.reconcile("face", "continuous_return", 10, Some("camera-r"), false);
    assert!(
        !unchanged,
        "an identical return snapshot must preserve the session"
    );
    assert_eq!(
        session.starts, 1,
        "an identical return snapshot must not restart the session"
    );

    let retargeted = policy.reconcile("face", "continuous_return", 11, Some("camera-r"), false);
    assert!(
        !retargeted,
        "a healthy return session survives generation retargeting"
    );
}

#[test]
fn identical_one_frame_snapshot_does_not_restart_pending_capture() {
    let mut policy = CameraLifecycle::new();
    policy.reconcile("face", "single_frame", 12, Some("camera-s"), false);

    let unchanged = policy.reconcile("face", "single_frame", 12, Some("camera-s"), false);
    assert!(
        !unchanged,
        "an unchanged pending one-frame policy does not restart"
    );
}

#[test]
fn single_frame_face_dark_settle_waits_five_seconds_then_assesses_face() {
    let mut dark_usable = CameraLifecycle::new();
    dark_usable.reconcile("face", "single_frame", 30, Some("camera-d"), false);
    dark_usable.begin_acquisition(0.0);
    dark_usable.acquired_frame();
    assert_eq!(
        dark_usable.frame(1.0),
        FrameDecision::AssessPresence,
        "first face frame"
    );

    assert!(
        dark_usable.begin_dark_settle(1.0),
        "first dark face frame starts a settle"
    );
    assert_eq!(
        dark_usable.settling_until(),
        Some(6.0),
        "dark settle lasts five seconds"
    );
    assert_eq!(
        dark_usable.frame(3.0),
        FrameDecision::DiscardWarmup,
        "settle discards frames"
    );
    assert_eq!(
        dark_usable.frame(5.9),
        FrameDecision::DiscardWarmup,
        "settle discards frames"
    );
    assert_eq!(
        dark_usable.frame(6.0),
        FrameDecision::AssessPresence,
        "usable frame at deadline"
    );
    assert!(
        dark_usable.settling_until().is_none(),
        "a settled frame clears the dark settle"
    );
}

#[test]
fn dark_frame_at_settle_deadline_is_final_and_next_sample_settles_again() {
    let mut dark_final = CameraLifecycle::new();
    dark_final.reconcile("face", "single_frame", 31, Some("camera-d"), false);
    dark_final.begin_acquisition(0.0);
    dark_final.acquired_frame();
    assert_eq!(
        dark_final.frame(1.0),
        FrameDecision::AssessPresence,
        "first dark face frame"
    );
    assert!(
        dark_final.begin_dark_settle(1.0),
        "first dark face frame starts a settle"
    );
    assert_eq!(
        dark_final.frame(6.0),
        FrameDecision::AssessPresence,
        "deadline frame is assessed"
    );
    assert!(
        !dark_final.begin_dark_settle(6.0),
        "a dark frame at the deadline is final"
    );

    dark_final.begin_acquisition(10.0);
    dark_final.acquired_frame();
    assert_eq!(
        dark_final.frame(11.0),
        FrameDecision::AssessPresence,
        "second sample first frame"
    );
    assert!(
        dark_final.begin_dark_settle(11.0),
        "second sample starts a fresh dark settle"
    );
    assert_eq!(
        dark_final.settling_until(),
        Some(16.0),
        "fresh dark settle lasts five seconds"
    );
    assert_eq!(
        dark_final.frame(16.0),
        FrameDecision::AssessPresence,
        "second sample settled"
    );
}

#[test]
fn light_only_samples_never_start_a_dark_settle() {
    let mut light_only = CameraLifecycle::new();
    light_only.reconcile("light_only", "single_frame", 32, Some("camera-l"), false);
    light_only.begin_acquisition(0.0);
    light_only.acquired_frame();

    assert!(
        !light_only.begin_dark_settle(1.0),
        "dark settle must not start outside face mode"
    );
    assert!(
        light_only.settling_until().is_none(),
        "a light-only frame starts no dark settle"
    );
    assert_eq!(
        light_only.frame(1.0),
        FrameDecision::AssessLight,
        "light-only frame assesses light",
    );
}

#[test]
fn cancel_deadlines_resets_dark_settle_for_the_same_sample() {
    let mut policy = CameraLifecycle::new();
    policy.reconcile("face", "single_frame", 40, Some("camera-x"), false);
    assert!(policy.begin_dark_settle(0.0), "first dark settle starts");
    assert!(
        !policy.begin_dark_settle(0.0),
        "second dark settle in one sample is refused"
    );
    assert!(policy.dark_settle_used(), "the spent allowance is visible");

    policy.cancel_deadlines();
    assert!(
        !policy.dark_settle_used(),
        "cancel_deadlines restores the allowance"
    );
    assert!(
        policy.begin_dark_settle(0.0),
        "a dark settle can start again after cancel"
    );
}

#[test]
fn acquisition_pending_follows_begin_acquisition_and_acquired_frame() {
    let mut policy = CameraLifecycle::new();
    assert!(!policy.acquisition_pending(), "no acquisition before begin");

    policy.begin_acquisition(1.0);
    assert!(
        policy.acquisition_pending(),
        "begin_acquisition arms the deadline"
    );
    assert_eq!(
        policy.acquisition_deadline(),
        Some(6.0),
        "acquisition lasts five seconds"
    );

    policy.acquired_frame();
    assert!(
        !policy.acquisition_pending(),
        "a frame clears the acquisition deadline"
    );
}

#[test]
fn default_state_is_inactive_single_frame_without_timers() {
    let policy = CameraLifecycle::new();
    assert_eq!(policy.mode(), "disabled");
    assert_eq!(policy.capture_mode(), "single_frame");
    assert_eq!(policy.generation(), 0);
    assert_eq!(policy.camera_id(), None);
    assert!(!policy.always_on());
    assert_eq!(policy.assessment_token(), 0);
    assert!(!policy.eligible());
    assert!(!policy.should_keep_session());
    assert!(policy.settling_until().is_none());
    assert!(policy.assessment_deadline().is_none());
    assert!(policy.acquisition_deadline().is_none());
    assert_eq!(policy, CameraLifecycle::default());
}

#[test]
fn should_keep_session_requires_always_on_and_continuous_capture() {
    let mut single = CameraLifecycle::new();
    single.reconcile("face", "single_frame", 1, Some("camera-a"), true);
    assert!(
        !single.should_keep_session(),
        "single-frame capture never keeps a session"
    );

    let mut monitoring = CameraLifecycle::new();
    monitoring.reconcile("face", "continuous_monitoring", 1, Some("camera-a"), false);
    assert!(
        !monitoring.should_keep_session(),
        "a session needs always-on"
    );

    let mut returning = CameraLifecycle::new();
    returning.reconcile("face", "continuous_return", 1, Some("camera-a"), true);
    assert!(
        returning.should_keep_session(),
        "continuous return with always-on is kept"
    );

    let mut suspended = CameraLifecycle::new();
    suspended.reconcile(
        "suspended",
        "continuous_monitoring",
        1,
        Some("camera-a"),
        true,
    );
    assert!(!suspended.eligible(), "suspended mode is not eligible");
    assert!(
        !suspended.should_keep_session(),
        "a suspended policy keeps no session"
    );
}

#[test]
fn step_reconcile_returns_the_stop_flag_and_a_full_state_snapshot() {
    let mut lifecycle = CameraLifecycle::new();
    let event = reconcile_event("face", "continuous_monitoring", 1, Some("camera-a"), true);
    let result = step_json(&mut lifecycle, &event);

    assert_eq!(
        value_of(&result),
        &json!(false),
        "an inactive policy needs no stop"
    );
    assert_eq!(
        result["state"],
        json!({
            "generation": 1,
            "camera_id": "camera-a",
            "mode": "face",
            "capture_mode": "continuous_monitoring",
            "always_on": true,
            "eligible": true,
            "acquisition_pending": false,
            "should_keep_session": true,
            "settling_until": null,
            "assessment_deadline": null,
            "acquisition_deadline": null,
            "assessment_token": 0,
        })
    );
}

#[test]
fn step_result_holds_only_value_and_state_with_contract_fields() {
    let mut lifecycle = CameraLifecycle::new();
    let event = reconcile_event(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    let result = step_json(&mut lifecycle, &event);
    assert_result_shape(&result);

    let acquisition = step_json(
        &mut lifecycle,
        &json!({"type": "begin_acquisition", "now": 0}),
    );
    assert_result_shape(&acquisition);
}

#[test]
fn step_frame_reports_snake_case_decision_strings() {
    let mut lifecycle = CameraLifecycle::new();
    let event = reconcile_event(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    step_json(&mut lifecycle, &event);
    step_json(
        &mut lifecycle,
        &json!({"type": "begin_acquisition", "now": 0}),
    );
    step_json(
        &mut lifecycle,
        &json!({"type": "begin_light_assessment", "now": 0}),
    );

    let warmup = step_json(&mut lifecycle, &json!({"type": "frame", "now": 4}));
    assert_eq!(value_of(&warmup), &json!("discard_warmup"));
    step_json(&mut lifecycle, &json!({"type": "acquired_frame"}));
    let assessed = step_json(&mut lifecycle, &json!({"type": "frame", "now": 5}));
    assert_eq!(value_of(&assessed), &json!("assess_light"));

    step_json(
        &mut lifecycle,
        &json!({"type": "begin_light_assessment", "now": 20}),
    );
    let expired = step_json(&mut lifecycle, &json!({"type": "frame", "now": 30}));
    assert_eq!(value_of(&expired), &json!("assessment_expired"));

    let mut face = CameraLifecycle::new();
    let event = reconcile_event("face", "continuous_monitoring", 1, Some("camera-a"), true);
    step_json(&mut face, &event);
    let presence = step_json(&mut face, &json!({"type": "frame", "now": 0}));
    assert_eq!(value_of(&presence), &json!("assess_presence"));
}

#[test]
fn step_state_only_events_return_null_and_predicates_return_booleans() {
    let mut lifecycle = CameraLifecycle::new();
    let event = reconcile_event("face", "single_frame", 1, Some("camera-a"), false);
    assert_eq!(value_of(&step_json(&mut lifecycle, &event)), &json!(false));

    for event in [
        json!({"type": "begin_acquisition", "now": 0}),
        json!({"type": "acquired_frame"}),
        json!({"type": "begin_light_assessment", "now": 0}),
        json!({"type": "cancel_deadlines"}),
    ] {
        assert_eq!(value_of(&step_json(&mut lifecycle, &event)), &Value::Null);
    }

    let dark = step_json(
        &mut lifecycle,
        &json!({"type": "begin_dark_settle", "now": 0}),
    );
    assert_eq!(value_of(&dark), &json!(true));
    let current = step_json(
        &mut lifecycle,
        &json!({"type": "is_current_assessment", "token": 0}),
    );
    assert_eq!(
        value_of(&current),
        &json!(false),
        "token 0 was superseded by the dark settle"
    );
    let timed = step_json(&mut lifecycle, &json!({"type": "timed_out", "now": 0}));
    assert_eq!(value_of(&timed), &json!(false));
    let callback = step_json(
        &mut lifecycle,
        &json!({"type": "accepts_callback", "generation": 1}),
    );
    assert_eq!(value_of(&callback), &json!(true));
}

#[test]
fn step_dark_settle_is_once_per_sample_and_cancel_deadlines_resets_it() {
    let mut lifecycle = CameraLifecycle::new();
    let event = reconcile_event("face", "single_frame", 40, Some("camera-x"), false);
    step_json(&mut lifecycle, &event);

    let first = step_json(
        &mut lifecycle,
        &json!({"type": "begin_dark_settle", "now": 0}),
    );
    let second = step_json(
        &mut lifecycle,
        &json!({"type": "begin_dark_settle", "now": 0}),
    );
    assert_eq!(value_of(&first), &json!(true));
    assert_eq!(
        value_of(&second),
        &json!(false),
        "second dark settle in one sample is refused"
    );

    let cancel = step_json(&mut lifecycle, &json!({"type": "cancel_deadlines"}));
    assert_eq!(value_of(&cancel), &Value::Null);
    let third = step_json(
        &mut lifecycle,
        &json!({"type": "begin_dark_settle", "now": 0}),
    );
    assert_eq!(
        value_of(&third),
        &json!(true),
        "cancel_deadlines must reset the dark settle"
    );
}

#[test]
fn step_warmup_sequence_matches_the_typed_api() {
    let mut typed = CameraLifecycle::new();
    typed.reconcile(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    typed.begin_acquisition(0.0);
    typed.begin_light_assessment(0.0);
    let warmup = typed.frame(4.0);
    typed.acquired_frame();
    let assessed = typed.frame(5.0);
    assert_eq!(warmup, FrameDecision::DiscardWarmup);
    assert_eq!(assessed, FrameDecision::AssessLight);

    let mut json_path = CameraLifecycle::new();
    let event = reconcile_event(
        "light_only",
        "continuous_monitoring",
        2,
        Some("camera-a"),
        true,
    );
    step_json(&mut json_path, &event);
    step_json(
        &mut json_path,
        &json!({"type": "begin_acquisition", "now": 0}),
    );
    step_json(
        &mut json_path,
        &json!({"type": "begin_light_assessment", "now": 0}),
    );
    let json_warmup = step_json(&mut json_path, &json!({"type": "frame", "now": 4}));
    assert_eq!(value_of(&json_warmup), &json!("discard_warmup"));
    step_json(&mut json_path, &json!({"type": "acquired_frame"}));
    let json_assessed = step_json(&mut json_path, &json!({"type": "frame", "now": 5}));
    assert_eq!(value_of(&json_assessed), &json!("assess_light"));

    assert_eq!(
        typed, json_path,
        "the JSON and typed paths must leave identical state"
    );
}

#[test]
fn step_accepts_full_u64_tokens_and_null_camera_id() {
    let mut lifecycle = CameraLifecycle::new();
    let event = reconcile_event("face", "continuous_monitoring", 1, None, true);
    let result = step_json(&mut lifecycle, &event);
    assert_eq!(
        result["state"]["camera_id"],
        Value::Null,
        "a null camera_id is no camera"
    );

    let max_token = json!({"type": "is_current_assessment", "token": u64::MAX});
    let result = step_json(&mut lifecycle, &max_token);
    assert_eq!(
        value_of(&result),
        &json!(false),
        "u64::MAX is a valid token and is not current"
    );
}

#[test]
fn unknown_event_type_is_rejected_and_leaves_state_unchanged() {
    let mut lifecycle = CameraLifecycle::new();
    lifecycle.reconcile("face", "continuous_monitoring", 1, Some("camera-a"), true);

    let error = expect_rejection(&mut lifecycle, br#"{"type":"explode","now":1}"#);
    assert_eq!(error, StepError::UnknownEventType("explode".to_owned()));
}

#[test]
fn unknown_field_is_rejected_and_leaves_state_unchanged() {
    let mut lifecycle = CameraLifecycle::new();
    lifecycle.reconcile("face", "continuous_monitoring", 1, Some("camera-a"), true);

    let error = expect_rejection(&mut lifecycle, br#"{"type":"frame","now":1,"extra":true}"#);
    assert_eq!(error, StepError::UnknownField("extra".to_owned()));

    let error = expect_rejection(&mut lifecycle, br#"{"type":"acquired_frame","now":1}"#);
    assert_eq!(error, StepError::UnknownField("now".to_owned()));

    let reconcile = br#"{"type":"reconcile","mode":"face","capture_mode":"single_frame","generation":2,"camera_id":null,"always_on":false,"extra":1}"#;
    let error = expect_rejection(&mut lifecycle, reconcile);
    assert_eq!(error, StepError::UnknownField("extra".to_owned()));
}

#[test]
fn duplicate_key_is_rejected_and_leaves_state_unchanged() {
    let mut lifecycle = CameraLifecycle::new();
    lifecycle.reconcile("face", "continuous_monitoring", 1, Some("camera-a"), true);

    let error = expect_rejection(&mut lifecycle, br#"{"type":"frame","now":1,"now":2}"#);
    assert_eq!(error, StepError::DuplicateKey("now".to_owned()));

    let error = expect_rejection(
        &mut lifecycle,
        br#"{"type":"frame","type":"frame","now":1}"#,
    );
    assert_eq!(error, StepError::DuplicateKey("type".to_owned()));
}

#[test]
fn duplicate_key_nested_in_a_value_is_rejected() {
    let mut lifecycle = CameraLifecycle::new();
    let input = br#"{"type":"acquired_frame","meta":{"a":1,"a":2}}"#;

    let error = expect_rejection(&mut lifecycle, input);
    assert_eq!(error, StepError::DuplicateKey("a".to_owned()));
}

#[test]
fn malformed_json_is_rejected_and_leaves_state_unchanged() {
    let mut lifecycle = CameraLifecycle::new();
    lifecycle.reconcile("face", "continuous_monitoring", 1, Some("camera-a"), true);

    let inputs: [&[u8]; 4] = [
        br#"{"type":"frame","now":1"#,
        br#"{"type":"acquired_frame"} trailing"#,
        b"",
        b"{\"type\":\"\xff\"}",
    ];
    for input in inputs {
        let error = expect_rejection(&mut lifecycle, input);
        assert!(
            matches!(error, StepError::MalformedJson(_)),
            "{input:?} must be malformed"
        );
    }
}

#[test]
fn non_object_event_is_rejected() {
    let mut lifecycle = CameraLifecycle::new();
    let inputs: [&[u8]; 3] = [b"[]", b"\"frame\"", b"1"];
    for input in inputs {
        let error = expect_rejection(&mut lifecycle, input);
        assert_eq!(
            error,
            StepError::NotAnObject,
            "{input:?} is not an event object"
        );
    }
}

#[test]
fn missing_type_and_missing_fields_are_rejected() {
    let mut lifecycle = CameraLifecycle::new();

    let error = expect_rejection(&mut lifecycle, br#"{"now":1}"#);
    assert_eq!(error, StepError::MissingField("type"));

    let error = expect_rejection(&mut lifecycle, br#"{"type":"begin_acquisition"}"#);
    assert_eq!(error, StepError::MissingField("now"));
}

#[test]
fn wrong_field_types_and_out_of_range_values_are_rejected() {
    let mut lifecycle = CameraLifecycle::new();

    let error = expect_rejection(&mut lifecycle, br#"{"type":1}"#);
    assert!(matches!(
        error,
        StepError::InvalidField { field: "type", .. }
    ));

    let error = expect_rejection(&mut lifecycle, br#"{"type":"frame","now":"5"}"#);
    assert!(matches!(
        error,
        StepError::InvalidField { field: "now", .. }
    ));

    let fractional = br#"{"type":"is_current_assessment","token":1.5}"#;
    let error = expect_rejection(&mut lifecycle, fractional);
    assert!(matches!(
        error,
        StepError::InvalidField { field: "token", .. }
    ));

    let negative = br#"{"type":"accepts_callback","generation":-1}"#;
    let error = expect_rejection(&mut lifecycle, negative);
    assert!(matches!(
        error,
        StepError::InvalidField {
            field: "generation",
            ..
        }
    ));

    let bad_flag = br#"{"type":"reconcile","mode":"face","capture_mode":"single_frame","generation":1,"camera_id":null,"always_on":"yes"}"#;
    let error = expect_rejection(&mut lifecycle, bad_flag);
    assert!(matches!(
        error,
        StepError::InvalidField {
            field: "always_on",
            ..
        }
    ));
}

#[test]
fn oversized_event_is_rejected_before_parsing_and_leaves_state_unchanged() {
    let mut lifecycle = CameraLifecycle::new();
    let mut bytes = br#"{"type":"acquired_frame"}"#.to_vec();
    bytes.resize(MAX_EVENT_BYTES + 1, b' ');

    let error = expect_rejection(&mut lifecycle, &bytes);
    let expected = StepError::InputTooLarge {
        length: MAX_EVENT_BYTES + 1,
        limit: MAX_EVENT_BYTES,
    };
    assert_eq!(error, expected);
}

#[test]
fn event_of_exactly_the_limit_is_accepted() {
    let mut lifecycle = CameraLifecycle::new();
    let mut bytes = br#"{"type":"acquired_frame"}"#.to_vec();
    bytes.resize(MAX_EVENT_BYTES, b' ');

    let result = lifecycle.step(&bytes).unwrap();
    let parsed: Value = serde_json::from_str(&result).unwrap();
    assert_result_shape(&parsed);
}

#[test]
fn two_events_with_the_same_now_are_deterministic() {
    let script = [
        reconcile_event(
            "light_only",
            "continuous_monitoring",
            2,
            Some("camera-a"),
            true,
        ),
        json!({"type": "begin_acquisition", "now": 5}),
        json!({"type": "begin_light_assessment", "now": 5}),
        json!({"type": "begin_light_assessment", "now": 5}),
        json!({"type": "frame", "now": 5}),
        json!({"type": "frame", "now": 5}),
        json!({"type": "timed_out", "now": 5}),
    ];

    let (first_results, first_state) = run_script(&script);
    let (second_results, second_state) = run_script(&script);
    assert_eq!(
        first_results, second_results,
        "the same events and times give the same results"
    );
    assert_eq!(
        first_state, second_state,
        "the same events and times give the same state"
    );

    let after_two_assessments: Value = serde_json::from_str(&first_results[3]).unwrap();
    assert_eq!(
        after_two_assessments["state"]["assessment_token"],
        json!(2),
        "each assessment bumps the token once"
    );
    let frame: Value = serde_json::from_str(&first_results[4]).unwrap();
    assert_eq!(
        value_of(&frame),
        &json!("discard_warmup"),
        "a frame inside the settle window"
    );
}

// ABI-level tests: the same behavior through the C functions in `telaviva_core::ffi`.

/// Returns a buffer that no C call has written to.
fn empty_buffer() -> TvBuffer {
    TvBuffer {
        data: std::ptr::null_mut(),
        len: 0,
    }
}

/// Asserts that a buffer holds no Rust-owned bytes.
fn assert_no_buffer(buffer: &TvBuffer) {
    assert!(
        buffer.data.is_null() && buffer.len == 0,
        "a call that fails must not write a buffer"
    );
}

/// Creates a lifecycle handle through the C ABI and checks that the call succeeded.
fn abi_create() -> *mut TvCameraLifecycle {
    let mut handle: *mut TvCameraLifecycle = std::ptr::null_mut();
    // SAFETY: `handle` is a writable pointer slot, as `tv_camera_lifecycle_create` requires.
    let status = unsafe { ffi::tv_camera_lifecycle_create(&mut handle) };
    assert_eq!(status, ffi::TV_OK, "create must succeed");
    assert!(!handle.is_null(), "create must write a handle");
    handle
}

/// Copies the JSON from a buffer written by a successful step, then frees the buffer.
fn take_result(out: &mut TvBuffer) -> Value {
    assert!(!out.data.is_null(), "a successful step writes a buffer");
    // SAFETY: a successful step wrote `len` initialized bytes at `data`; they are copied before
    // the buffer is freed.
    let bytes = unsafe { std::slice::from_raw_parts(out.data, out.len) }.to_vec();
    // SAFETY: `out` was written by `tv_camera_lifecycle_step` and is freed exactly once here.
    unsafe { ffi::tv_buffer_free(out) };
    assert_eq!(out.len, 0, "tv_buffer_free must zero the length");
    assert!(out.data.is_null(), "tv_buffer_free clears data");
    serde_json::from_slice(&bytes).unwrap()
}

/// Outcome of one call to `tv_camera_lifecycle_step`.
struct AbiStep {
    status: i32,
    /// The parsed result when `status` is `TV_OK`; `None` otherwise.
    result: Option<Value>,
}

/// Sends raw event bytes through `tv_camera_lifecycle_step`. A failed call must leave the
/// buffer untouched; a successful call's buffer is copied and freed by `take_result`.
fn abi_step_bytes(handle: *mut TvCameraLifecycle, input: &[u8]) -> AbiStep {
    let mut out = empty_buffer();
    // SAFETY: `handle` is live (every caller passes a handle from `abi_create`), `input` is
    // readable for `input.len()` bytes, and `out` is writable.
    let status =
        unsafe { ffi::tv_camera_lifecycle_step(handle, input.as_ptr(), input.len(), &mut out) };
    if status != ffi::TV_OK {
        assert_no_buffer(&out);
        return AbiStep {
            status,
            result: None,
        };
    }
    AbiStep {
        status,
        result: Some(take_result(&mut out)),
    }
}

/// Sends one event through the C ABI, expects `TV_OK` and returns the parsed result, which must
/// have the contract shape.
fn abi_ok(handle: *mut TvCameraLifecycle, event: &Value) -> Value {
    let input = serde_json::to_vec(event).unwrap();
    let step = abi_step_bytes(handle, &input);
    assert_eq!(step.status, ffi::TV_OK, "{event} must succeed");
    let result = step.result.expect("TV_OK must return a result");
    assert_result_shape(&result);
    result
}

/// Reads the lifecycle state through `accepts_callback`, which never changes the state.
fn abi_state(handle: *mut TvCameraLifecycle) -> Value {
    let probe = json!({"type": "accepts_callback", "generation": 0});
    let result = abi_ok(handle, &probe);
    result["state"].clone()
}

/// Checks that a rejected input returns `expected`, returns no result and leaves the state
/// as it was.
fn assert_rejected(handle: *mut TvCameraLifecycle, input: &[u8], expected: i32) {
    let before = abi_state(handle);
    let step = abi_step_bytes(handle, input);
    assert_eq!(
        step.status,
        expected,
        "wrong status for an input of {} bytes",
        input.len()
    );
    assert!(
        step.result.is_none(),
        "a rejected event must not return a result"
    );
    assert_eq!(
        abi_state(handle),
        before,
        "a rejected event must not change the state"
    );
}

#[test]
fn abi_create_step_and_destroy_run_acquisition_then_a_frame_decision() {
    let handle = abi_create();

    let reconciled = abi_ok(
        handle,
        &reconcile_event("face", "continuous_monitoring", 1, Some("camera-a"), true),
    );
    assert_eq!(
        value_of(&reconciled),
        &json!(false),
        "an inactive policy needs no stop"
    );
    assert_eq!(reconciled["state"]["should_keep_session"], json!(true));

    let acquisition = abi_ok(handle, &json!({"type": "begin_acquisition", "now": 0}));
    assert_eq!(value_of(&acquisition), &Value::Null);
    assert_eq!(acquisition["state"]["acquisition_pending"], json!(true));
    assert_eq!(acquisition["state"]["acquisition_deadline"], json!(5.0));

    let acquired = abi_ok(handle, &json!({"type": "acquired_frame"}));
    assert_eq!(acquired["state"]["acquisition_pending"], json!(false));

    let frame = abi_ok(handle, &json!({"type": "frame", "now": 1}));
    assert_eq!(value_of(&frame), &json!("assess_presence"));

    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_null_handle_is_rejected_by_step_and_ignored_by_destroy() {
    let event = br#"{"type":"acquired_frame"}"#;
    let null: *mut TvCameraLifecycle = std::ptr::null_mut();
    let mut out = empty_buffer();
    // SAFETY: the null handle is the input under test; `event` and `out` are valid.
    let status =
        unsafe { ffi::tv_camera_lifecycle_step(null, event.as_ptr(), event.len(), &mut out) };
    assert_eq!(status, ffi::TV_INVALID_ARGUMENT);
    assert_no_buffer(&out);

    // SAFETY: a null handle is the input under test; destroy must ignore it.
    unsafe { ffi::tv_camera_lifecycle_destroy(std::ptr::null_mut()) };
}

#[test]
fn abi_null_out_pointers_are_invalid_argument() {
    // SAFETY: a null out pointer is the input under test.
    let status = unsafe { ffi::tv_camera_lifecycle_create(std::ptr::null_mut()) };
    assert_eq!(status, ffi::TV_INVALID_ARGUMENT);

    let handle = abi_create();
    let event = br#"{"type":"acquired_frame"}"#;
    // SAFETY: `handle` is live; a null buffer is the input under test.
    let status = unsafe {
        ffi::tv_camera_lifecycle_step(handle, event.as_ptr(), event.len(), std::ptr::null_mut())
    };
    assert_eq!(status, ffi::TV_INVALID_ARGUMENT);
    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_null_or_empty_input_is_invalid_argument_and_leaves_state_unchanged() {
    let handle = abi_create();
    abi_ok(handle, &json!({"type": "begin_acquisition", "now": 0}));
    let before = abi_state(handle);

    let mut out = empty_buffer();
    // SAFETY: a null input with a non-zero length is the input under test; `out` is writable.
    let status = unsafe { ffi::tv_camera_lifecycle_step(handle, std::ptr::null(), 1, &mut out) };
    assert_eq!(status, ffi::TV_INVALID_ARGUMENT);
    // SAFETY: a null input with zero length is the input under test; `out` is writable.
    let status = unsafe { ffi::tv_camera_lifecycle_step(handle, std::ptr::null(), 0, &mut out) };
    assert_eq!(status, ffi::TV_INVALID_ARGUMENT);
    assert_no_buffer(&out);
    assert_eq!(abi_state(handle), before);

    // A zero-length input with a valid pointer is an empty event, which is not JSON.
    assert_rejected(handle, b"", ffi::TV_INVALID_ARGUMENT);

    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_oversized_input_returns_size_limit_and_leaves_state_unchanged() {
    let handle = abi_create();
    let mut oversized = br#"{"type":"acquired_frame"}"#.to_vec();
    oversized.resize(MAX_EVENT_BYTES + 1, b' ');

    assert_rejected(handle, &oversized, ffi::TV_SIZE_LIMIT);

    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_input_of_exactly_the_limit_is_accepted() {
    let handle = abi_create();
    let mut exact = br#"{"type":"acquired_frame"}"#.to_vec();
    exact.resize(MAX_EVENT_BYTES, b' ');

    let step = abi_step_bytes(handle, &exact);
    assert_eq!(
        step.status,
        ffi::TV_OK,
        "an input of exactly the limit must be accepted"
    );
    assert_result_shape(&step.result.expect("TV_OK must return a result"));

    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_rejected_events_return_invalid_argument_and_leave_state_unchanged() {
    let handle = abi_create();
    abi_ok(
        handle,
        &reconcile_event("face", "continuous_monitoring", 1, Some("camera-a"), true),
    );
    abi_ok(handle, &json!({"type": "begin_acquisition", "now": 0}));

    let inputs: [&[u8]; 7] = [
        br#"{"type":"frame","now":1"#,
        br#"{"type":"acquired_frame"} trailing"#,
        br#"{"type":"frame","now":1,"now":2}"#,
        br#"{"type":"explode","now":1}"#,
        br#"{"type":"frame","now":1,"extra":true}"#,
        br#"[]"#,
        b"{\"type\":\"\xff\"}",
    ];
    for input in inputs {
        assert_rejected(handle, input, ffi::TV_INVALID_ARGUMENT);
    }

    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_step_result_buffer_is_rust_owned_and_freed_by_tv_buffer_free() {
    let handle = abi_create();
    let input = br#"{"type":"accepts_callback","generation":0}"#;
    let mut out = empty_buffer();
    // SAFETY: `handle` is live; `input` and `out` are valid for the call.
    let status =
        unsafe { ffi::tv_camera_lifecycle_step(handle, input.as_ptr(), input.len(), &mut out) };
    assert_eq!(status, ffi::TV_OK);
    assert!(!out.data.is_null(), "a successful step writes a buffer");
    assert!(out.len > 0, "a successful step writes bytes");

    // SAFETY: the successful step wrote `len` initialized bytes at `data`; they are read before
    // the buffer is freed.
    let bytes = unsafe { std::slice::from_raw_parts(out.data, out.len) };
    let parsed: Value = serde_json::from_slice(bytes).unwrap();
    assert_result_shape(&parsed);
    assert_eq!(value_of(&parsed), &json!(false));

    // SAFETY: `out` was written by `tv_camera_lifecycle_step` and is freed here.
    unsafe { ffi::tv_buffer_free(&mut out) };
    assert!(out.data.is_null(), "tv_buffer_free clears data");
    assert_eq!(out.len, 0, "tv_buffer_free must zero the length");
    // SAFETY: a cleared buffer makes a second free a no-op.
    unsafe { ffi::tv_buffer_free(&mut out) };

    // SAFETY: `handle` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(handle) };
}

#[test]
fn abi_destroy_then_create_starts_from_a_fresh_lifecycle() {
    let first = abi_create();
    abi_ok(
        first,
        &reconcile_event("face", "continuous_monitoring", 7, Some("camera-a"), true),
    );
    let assessment = json!({"type": "begin_light_assessment", "now": 0});
    abi_ok(first, &assessment);
    // SAFETY: `first` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(first) };

    let second = abi_create();
    let state = abi_state(second);
    assert_eq!(state["generation"], json!(0));
    assert_eq!(state["mode"], json!("disabled"));
    assert_eq!(state["camera_id"], Value::Null);
    assert_eq!(state["assessment_token"], json!(0));
    assert_eq!(state["settling_until"], Value::Null);
    // SAFETY: `second` is live and is not used after this call.
    unsafe { ffi::tv_camera_lifecycle_destroy(second) };
}
