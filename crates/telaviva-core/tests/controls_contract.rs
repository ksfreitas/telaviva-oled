//! Contract tests for `telaviva_core::controls`.
//!
//! Every test drives the machine through `Controls::step` with JSON events, following the
//! envelope and error rules in `specs/archived/core-command-queue/contracts.md`.

use serde_json::{json, Value};
use telaviva_core::controls::{Controls, StepError, MAX_EVENT_BYTES};

// Harness, shared with later phases. `Fx` owns a machine and a clock. `send` adds `now` when the
// event has none, so a test only spells out `now` when the value matters. `raw` sends bytes
// unchanged, for input that must be rejected as written.

struct Fx {
    c: Controls,
    now: f64,
}

impl Fx {
    fn new() -> Self {
        Self {
            c: Controls::new(),
            now: 0.0,
        }
    }

    /// Sends one event and returns the rejection instead of panicking.
    fn try_send(&mut self, mut event: Value) -> Result<Value, StepError> {
        if let Value::Object(map) = &mut event {
            if !map.contains_key("now") {
                map.insert("now".to_owned(), json!(self.now));
            }
        }
        let bytes = serde_json::to_vec(&event).unwrap();
        raw(self, &bytes)
    }

    /// Sends one event and panics with the rejection if the machine refuses it.
    fn send(&mut self, event: Value) -> Value {
        match self.try_send(event) {
            Ok(result) => result,
            Err(error) => panic!("event rejected: {error:?}"),
        }
    }

    /// Moves the clock forward; events sent afterwards carry the new time.
    fn advance(&mut self, secs: f64) {
        self.now += secs;
    }
}

/// Sends raw bytes to the machine and parses the result.
fn raw(fx: &mut Fx, bytes: &[u8]) -> Result<Value, StepError> {
    let text = fx.c.step(bytes)?;
    Ok(serde_json::from_str(&text).expect("result is JSON"))
}

/// The `effects` array of a result.
fn effects(result: &Value) -> &Vec<Value> {
    result["effects"].as_array().expect("effects is an array")
}

/// The `next_deadline` of a result, if any.
fn deadline(result: &Value) -> Option<f64> {
    result["next_deadline"].as_f64()
}

/// One DDC-capable monitor, shaped like the `observe` sample in the contracts.
fn monitor_json(index: usize) -> Value {
    json!({
        "id": format!("mon-{index}"),
        "generation": 3,
        "display_state": "awake",
        "identity_status": "stable",
        "brightness": { "supported": true, "observed": 41.0, "min": 0, "max": 100 },
        "volume": { "supported": true, "observed": 20.0, "min": 0, "max": 100 }
    })
}

/// One pending DDC action, shaped like the `observe` sample in the contracts.
fn action_json(id: u64) -> Value {
    json!({
        "id": id,
        "generation": 8,
        "monitor_id": "mon-1",
        "kind": "set_brightness",
        "value": 37.0
    })
}

/// Builds an `observe` event from its arrays.
fn observe_event(revision: u64, monitors: Vec<Value>, pending: Vec<Value>) -> Value {
    json!({
        "type": "observe",
        "revision": revision,
        "monitors": monitors,
        "pending_actions": pending
    })
}

/// One monitor with the id `m1`, shaped like the `observe` sample in the contracts.
fn m1_json(generation: u64, display_state: &str) -> Value {
    json!({
        "id": "m1",
        "generation": generation,
        "display_state": display_state,
        "identity_status": "stable",
        "brightness": { "supported": true, "observed": 41.0, "min": 0, "max": 100 },
        "volume": { "supported": true, "observed": 20.0, "min": 0, "max": 100 }
    })
}

/// An `observe` event with no pending actions.
fn observe_json(revision: u64, monitors: Vec<Value>) -> Value {
    observe_event(revision, monitors, vec![])
}

/// A `slider` event for monitor `m1`, without `now`.
fn slider_json(feature: &str, value: f64) -> Value {
    json!({"type": "slider", "monitor_id": "m1", "feature": feature, "value": value})
}

/// The first `submit_command` effect of a result.
fn first_submit(result: &Value) -> &Value {
    effects(result)
        .iter()
        .find(|effect| effect["type"] == "submit_command")
        .expect("a submit_command effect")
}

/// The parsed `command_json` of the first `submit_command` effect of a result.
fn command_of(result: &Value) -> Value {
    let text = first_submit(result)["command_json"]
        .as_str()
        .expect("command_json is a string");
    serde_json::from_str(text).expect("command_json is JSON")
}

/// One `m1` monitor at generation 1, awake, with the given observed brightness and volume.
fn m1_levels(brightness: f64, volume: f64) -> Value {
    let mut monitor = m1_json(1, "awake");
    monitor["brightness"]["observed"] = json!(brightness);
    monitor["volume"]["observed"] = json!(volume);
    monitor
}

/// An `observe` event with one `m1` monitor at the given observed levels.
fn observe_levels(revision: u64, brightness: f64, volume: f64) -> Value {
    observe_json(revision, vec![m1_levels(brightness, volume)])
}

/// A `configure` event that sets the key steps and the key switch. Leaves the volume owner as is.
fn configure_keys(fx: &mut Fx, enabled: bool, brightness_step: f64, volume_step: f64) -> Value {
    fx.send(json!({
        "type": "configure",
        "brightness_step": brightness_step,
        "volume_step": volume_step,
        "keys_enabled": enabled
    }))
}

/// A `key` event. `owner_monitor` names the resolved `m1` target at generation 1, or null.
fn key_json(
    key: &str,
    phase: &str,
    repeat: bool,
    fine: bool,
    owner_monitor: Option<&str>,
) -> Value {
    let owner = match owner_monitor {
        Some(id) => json!({"monitor_id": id, "generation": 1}),
        None => Value::Null,
    };
    json!({
        "type": "key", "key": key, "phase": phase, "repeat": repeat, "fine": fine,
        "owner": owner
    })
}

/// Whether a `key` result consumes the key.
fn consumed(result: &Value) -> bool {
    let consume = &result["value"]["consume"];
    consume.as_bool().expect("a key result holds consume")
}

/// The `type` and raw `value` of each `submit_command` in a result, in effect order.
fn submitted_writes(result: &Value) -> Vec<(String, f64)> {
    effects(result)
        .iter()
        .filter(|effect| effect["type"] == "submit_command")
        .map(|effect| {
            let text = effect["command_json"]
                .as_str()
                .expect("command_json is a string");
            let command: Value = serde_json::from_str(text).expect("command_json is JSON");
            let kind = command["type"].as_str().expect("a command type");
            let value = command["value"].as_f64().expect("a command value");
            (kind.to_owned(), value)
        })
        .collect()
}

#[test]
fn configure_applies_step_and_keys_enabled() {
    let mut fx = Fx::new();

    // Each step is clamped on its own: brightness 7.4 with volume 99, then the reverse, then 0.
    fx.send(json!({
        "type": "configure", "brightness_step": 7.4, "volume_step": 99, "keys_enabled": true
    }));
    assert_eq!(
        fx.c.brightness_step(),
        7.0,
        "brightness step is clamped to whole points"
    );
    assert_eq!(fx.c.volume_step(), 15.0, "volume step is clamped to 15");
    assert!(fx.c.keys_enabled());

    fx.send(json!({
        "type": "configure", "brightness_step": 99, "volume_step": 0, "keys_enabled": true
    }));
    assert_eq!(
        fx.c.brightness_step(),
        15.0,
        "brightness step is clamped to 15"
    );
    assert_eq!(fx.c.volume_step(), 1.0, "volume step is clamped to 1");
    assert!(fx.c.keys_enabled());

    fx.send(json!({
        "type": "configure", "brightness_step": 0, "volume_step": 7.4, "keys_enabled": true
    }));
    assert_eq!(
        fx.c.brightness_step(),
        1.0,
        "brightness step is clamped to 1"
    );
    assert_eq!(
        fx.c.volume_step(),
        7.0,
        "volume step is clamped to whole points"
    );
    assert!(fx.c.keys_enabled());
}

#[test]
fn configure_rejects_the_old_step_member() {
    let mut fx = Fx::new();
    fx.send(json!({
        "type": "configure", "brightness_step": 9, "volume_step": 9, "keys_enabled": true
    }));

    let error = fx
        .try_send(json!({
            "type": "configure", "brightness_step": 3, "volume_step": 3, "step": 3,
            "keys_enabled": true
        }))
        .expect_err("the old step member must be rejected");
    assert!(matches!(error, StepError::UnknownField(field) if field == "step"));
    // The rejected event leaves the steps as they were.
    assert_eq!(fx.c.brightness_step(), 9.0);
    assert_eq!(fx.c.volume_step(), 9.0);
}

#[test]
fn configure_volume_owner_is_optional_nullable_string() {
    let mut fx = Fx::new();
    assert_eq!(fx.c.volume_owner(), None);

    // A string sets the owner.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
        "volume_owner": "m1"
    }));
    assert_eq!(fx.c.volume_owner(), Some("m1"));

    // Absent leaves the owner unchanged.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true
    }));
    assert_eq!(
        fx.c.volume_owner(),
        Some("m1"),
        "absent volume_owner is unchanged"
    );

    // A different string replaces it.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
        "volume_owner": "m2"
    }));
    assert_eq!(fx.c.volume_owner(), Some("m2"));

    // Null clears it.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
        "volume_owner": null
    }));
    assert_eq!(fx.c.volume_owner(), None, "null clears the owner");

    // A mistyped owner is rejected and changes nothing.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
        "volume_owner": "m3"
    }));
    let error = fx
        .try_send(json!({
            "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
            "volume_owner": 4
        }))
        .expect_err("a numeric owner must be rejected");
    assert!(matches!(
        error,
        StepError::InvalidField {
            field: "volume_owner",
            ..
        }
    ));
    assert_eq!(fx.c.volume_owner(), Some("m3"));
}

#[test]
fn configure_disabling_keys_clears_the_volume_owner() {
    let mut fx = Fx::new();
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
        "volume_owner": "m1"
    }));
    assert_eq!(fx.c.volume_owner(), Some("m1"));

    // Disabling keys without a volume_owner member clears the owner.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": false
    }));
    assert_eq!(fx.c.volume_owner(), None, "disabled keys clear the owner");

    // A volume_owner in the same event that disables keys is ignored: the owner stays cleared.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": false,
        "volume_owner": "m2"
    }));
    assert_eq!(
        fx.c.volume_owner(),
        None,
        "keys disabled ignore a volume_owner"
    );

    // With keys enabled, the same event applies its owner.
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
        "volume_owner": "m2"
    }));
    assert_eq!(fx.c.volume_owner(), Some("m2"));
}

#[test]
fn result_shape_has_value_effects_and_deadline() {
    let mut fx = Fx::new();
    fx.advance(1.0);

    let result = fx.send(json!({"type": "tick"}));
    assert_eq!(
        result.as_object().unwrap().len(),
        3,
        "a result holds exactly value, effects and next_deadline"
    );
    assert_eq!(result["value"], Value::Null);
    assert!(effects(&result).is_empty(), "a tick emits no effects");
    assert_eq!(deadline(&result), None, "an idle machine has no deadline");
}

#[test]
fn unknown_event_type_is_rejected() {
    let mut fx = Fx::new();
    let error = fx
        .try_send(json!({"type": "explode"}))
        .expect_err("unknown type must be rejected");
    assert!(matches!(error, StepError::UnknownEventType { .. }));
}

#[test]
fn unknown_field_is_rejected() {
    let mut fx = Fx::new();
    let error = fx
        .try_send(json!({
            "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true,
            "extra": 1
        }))
        .expect_err("unknown field must be rejected");
    assert!(matches!(error, StepError::UnknownField { .. }));
}

#[test]
fn duplicate_key_is_rejected() {
    let mut fx = Fx::new();
    let error = raw(
        &mut fx,
        br#"{"type":"configure","now":1,"now":2,"brightness_step":5,"volume_step":5,"keys_enabled":true}"#,
    )
    .expect_err("duplicate key must be rejected");
    assert!(matches!(error, StepError::DuplicateKey { .. }));
}

#[test]
fn missing_now_and_non_numeric_now_are_rejected() {
    let mut fx = Fx::new();
    assert!(
        raw(&mut fx, br#"{"type":"tick"}"#).is_err(),
        "an event without now is rejected"
    );
    assert!(
        raw(&mut fx, br#"{"type":"tick","now":"5"}"#).is_err(),
        "a string now is rejected"
    );
    assert!(
        fx.try_send(json!({"type": "tick", "now": true})).is_err(),
        "a boolean now is rejected"
    );
}

#[test]
fn missing_and_mistyped_fields_are_rejected() {
    let mut fx = Fx::new();

    let error = fx
        .try_send(json!({"type": "attempt_done", "token": 1, "ok": false, "value": null}))
        .expect_err("a failed attempt without error must be rejected");
    assert!(matches!(error, StepError::MissingField { .. }));

    assert!(
        fx.try_send(json!({"type": "slider", "monitor_id": "mon-1", "feature": "brightness"}))
            .is_err(),
        "a slider without value is rejected"
    );
    assert!(
        fx.try_send(
            json!({"type": "slider", "monitor_id": "mon-1", "feature": "brightness", "value": "40"})
        )
        .is_err(),
        "a slider with a string value is rejected"
    );
    assert!(
        fx.try_send(
            json!({"type": "slider", "monitor_id": "mon-1", "feature": "contrast", "value": 40})
        )
        .is_err(),
        "an unknown feature is rejected"
    );
    assert!(
        fx.try_send(json!({"type": "key", "key": "power", "phase": "down", "repeat": false, "fine": false, "owner": null}))
            .is_err(),
        "an unknown key name is rejected"
    );
    assert!(
        raw(&mut fx, br#"{"type":1,"now":0}"#).is_err(),
        "a non-string type is rejected"
    );
}

#[test]
fn trailing_data_and_non_object_are_rejected() {
    let mut fx = Fx::new();
    let inputs: [&[u8]; 6] = [
        br#"{"type":"tick","now":0} trailing"#,
        br#"{"type":"tick","now":0}{}"#,
        b"[]",
        br#""tick""#,
        b"1",
        b"",
    ];
    for input in inputs {
        assert!(raw(&mut fx, input).is_err(), "{input:?} must be rejected");
    }
}

#[test]
fn oversize_input_is_rejected_before_parsing() {
    let mut fx = Fx::new();
    let bytes = vec![b' '; MAX_EVENT_BYTES + 1];
    let error = raw(&mut fx, &bytes).expect_err("oversize input must be rejected");
    assert!(matches!(error, StepError::InputTooLarge { .. }));
}

#[test]
fn nested_unknown_fields_are_rejected() {
    let mut fx = Fx::new();

    let mut monitor = monitor_json(1);
    monitor["extra"] = json!(1);
    let error = fx
        .try_send(observe_event(1, vec![monitor], vec![]))
        .expect_err("unknown field in a monitor must be rejected");
    assert!(matches!(error, StepError::UnknownField { .. }));

    let mut action = action_json(17);
    action["extra"] = json!(true);
    let error = fx
        .try_send(observe_event(2, vec![], vec![action]))
        .expect_err("unknown field in a pending action must be rejected");
    assert!(matches!(error, StepError::UnknownField { .. }));

    let error = fx
        .try_send(json!({
            "type": "key",
            "key": "volume_up",
            "phase": "down",
            "repeat": false,
            "fine": false,
            "owner": { "monitor_id": "mon-1", "generation": 3, "extra": 1 }
        }))
        .expect_err("unknown field in a key owner must be rejected");
    assert!(matches!(error, StepError::UnknownField { .. }));
}

#[test]
fn observe_caps_are_enforced() {
    let mut fx = Fx::new();

    let monitors: Vec<Value> = (0..64).map(monitor_json).collect();
    fx.send(observe_event(1, monitors, vec![]));
    let too_many: Vec<Value> = (0..65).map(monitor_json).collect();
    assert!(
        fx.try_send(observe_event(2, too_many, vec![])).is_err(),
        "65 monitors must be rejected"
    );

    let actions: Vec<Value> = (0..128u64).map(action_json).collect();
    fx.send(observe_event(3, vec![], actions));
    let too_many: Vec<Value> = (0..129u64).map(action_json).collect();
    assert!(
        fx.try_send(observe_event(4, vec![], too_many)).is_err(),
        "129 pending actions must be rejected"
    );
}

#[test]
fn rejected_event_leaves_state_unchanged() {
    let mut fx = Fx::new();
    fx.send(json!({
        "type": "configure", "brightness_step": 9, "volume_step": 9, "keys_enabled": true
    }));
    assert_eq!(fx.c.brightness_step(), 9.0);

    // A rejected event that also has an unknown field must not change the steps.
    assert!(fx
        .try_send(json!({
            "type": "configure", "brightness_step": 3, "volume_step": 3, "keys_enabled": true,
            "extra": 1
        }))
        .is_err());
    assert_eq!(
        fx.c.brightness_step(),
        9.0,
        "a rejected configure keeps the steps"
    );
    assert_eq!(
        fx.c.volume_step(),
        9.0,
        "a rejected configure keeps the steps"
    );

    // A rejected event with a huge `now` must not advance the clock. The later event with an
    // earlier `now` still succeeds and applies.
    assert!(raw(
        &mut fx,
        br#"{"type":"configure","now":1e300,"brightness_step":3,"volume_step":3,"keys_enabled":true,"extra":1}"#
    )
    .is_err());
    assert_eq!(fx.c.brightness_step(), 9.0);

    fx.try_send(json!({
        "type": "configure", "now": 5, "brightness_step": 4, "volume_step": 4,
        "keys_enabled": true
    }))
    .expect("a valid event after a rejection is accepted");
    assert_eq!(fx.c.brightness_step(), 4.0);
    assert_eq!(fx.c.volume_step(), 4.0);
}

#[test]
fn now_never_moves_backward() {
    let mut fx = Fx::new();

    fx.send(json!({
        "type": "configure", "now": 10, "brightness_step": 4, "volume_step": 4,
        "keys_enabled": true
    }));
    assert_eq!(fx.c.brightness_step(), 4.0);

    // An earlier `now` is clamped to the previous one. The event still applies, and no panic.
    fx.send(json!({
        "type": "configure", "now": 5, "brightness_step": 6, "volume_step": 6,
        "keys_enabled": true
    }));
    assert_eq!(fx.c.brightness_step(), 6.0);
    fx.send(json!({"type": "tick", "now": 5}));
}

#[test]
fn shutdown_makes_later_events_inert() {
    let mut fx = Fx::new();
    fx.send(json!({
        "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true
    }));
    fx.send(json!({"type": "shutdown"}));
    fx.advance(1.0);

    let result = fx.send(json!({
        "type": "configure", "brightness_step": 3, "volume_step": 3, "keys_enabled": true
    }));
    assert_eq!(result["value"], Value::Null);
    assert!(
        effects(&result).is_empty(),
        "events after shutdown emit nothing"
    );
    assert_eq!(deadline(&result), None);
    assert_eq!(
        fx.c.brightness_step(),
        5.0,
        "events after shutdown change nothing"
    );
    assert_eq!(
        fx.c.volume_step(),
        5.0,
        "events after shutdown change nothing"
    );

    // Malformed input is still rejected after shutdown.
    assert!(raw(
        &mut fx,
        br#"{"type":"configure","now":1,"brightness_step":3,"volume_step":3,"keys_enabled":true,"extra":1}"#
    )
    .is_err());
    assert!(raw(&mut fx, br#"{"type":"configure","now":1"#).is_err());
}

#[test]
fn every_event_type_is_accepted_by_the_parser() {
    // One well-formed example per event type, sent in order. Only acceptance and the reply shape
    // are checked here; values and effects are covered by the dedicated tests.
    let mut fx = Fx::new();
    let events = [
        json!({
            "type": "configure", "brightness_step": 5, "volume_step": 5, "keys_enabled": true
        }),
        // Same shape as the `observe` sample in contracts.md.
        json!({
            "type": "observe", "now": 120.5, "revision": 42,
            "monitors": [
                {
                    "id": "mon-1", "generation": 3,
                    "display_state": "awake", "identity_status": "stable",
                    "brightness": { "supported": true, "observed": 41.0, "min": 0, "max": 100 },
                    "volume": { "supported": true, "observed": 20.0, "min": 0, "max": 100 }
                }
            ],
            "pending_actions": [
                { "id": 17, "generation": 8, "monitor_id": "mon-1", "kind": "set_brightness", "value": 37.0 }
            ]
        }),
        json!({"type": "monitor_profile", "monitor_id": "mon-1", "manufacturer": "DEL"}),
        json!({"type": "monitor_removed", "monitor_id": "mon-1"}),
        json!({"type": "display_power", "on": false}),
        json!({"type": "slider", "monitor_id": "mon-unknown", "feature": "brightness", "value": 40.0}),
        json!({
            "type": "key", "now": 120.6, "key": "volume_up", "phase": "down",
            "repeat": false, "fine": false,
            "owner": { "monitor_id": "mon-1", "generation": 3 }
        }),
        json!({"type": "submitted", "token": 1, "accepted": true}),
        json!({"type": "attempt_done", "token": 1, "ok": true, "value": 41.0}),
        json!({"type": "attempt_done", "token": 2, "ok": false, "error": "io", "value": null}),
        json!({"type": "query_draft", "monitor_id": "mon-unknown", "feature": "volume"}),
        json!({"type": "tick"}),
        json!({"type": "shutdown"}),
    ];

    for event in events {
        let kind = event["type"].clone();
        // `send` fails the test on a StepError, so reaching the assertions means accepted.
        let result = fx.send(event);
        for key in ["value", "effects", "next_deadline"] {
            assert!(result.get(key).is_some(), "{kind} reply must have `{key}`");
        }
        assert!(
            result["effects"].is_array(),
            "{kind} effects must be an array"
        );
    }
}

#[test]
fn manual_events_slider_on_eligible_monitor_returns_pending_draft() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);

    let result = fx.send(slider_json("brightness", 37.0));
    assert_eq!(result["value"]["draft"].as_f64(), Some(37.0));
    assert_eq!(result["value"]["status"], "pending");
    let next = deadline(&result).expect("a queued slider sets a deadline");
    assert!(
        (next - (fx.now + 0.020)).abs() < 1e-9,
        "the deadline is 20 ms after the slider"
    );
}

#[test]
fn manual_events_tick_before_deadline_keeps_deadline() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    let slider = fx.send(slider_json("brightness", 37.0));
    let set = deadline(&slider).expect("a queued slider sets a deadline");

    fx.advance(0.010);
    let tick = fx.send(json!({"type": "tick"}));
    assert!(
        effects(&tick).is_empty(),
        "a tick before the deadline emits nothing"
    );
    let kept = deadline(&tick).expect("the deadline is kept");
    assert!((kept - set).abs() < 1e-9, "the deadline does not move");
}

#[test]
fn manual_events_tick_at_deadline_submits_one_command() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("brightness", 37.0));

    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        effects(&tick).len(),
        1,
        "exactly one effect at the deadline"
    );
    assert_eq!(effects(&tick)[0]["type"], "submit_command");
    assert!(
        first_submit(&tick)["token"].is_u64(),
        "the token is numeric"
    );

    let command = command_of(&tick);
    assert!(command.is_object(), "command_json is an object");
    assert_eq!(command["type"], "set_brightness");
    assert_eq!(command["value"].as_f64(), Some(37.0));
    assert_eq!(command["connection_generation"].as_u64(), Some(1));
}

#[test]
fn manual_events_slider_while_in_flight_coalesces() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("brightness", 37.0));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    let token = first_submit(&tick)["token"]
        .as_u64()
        .expect("numeric token");
    fx.send(json!({"type": "submitted", "token": token, "accepted": true}));

    // The same raw value as the in-flight command queues nothing.
    fx.send(slider_json("brightness", 37.0));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert!(
        effects(&tick).is_empty(),
        "a tick while in flight emits nothing"
    );
    fx.advance(1.0);
    let tick = fx.send(json!({"type": "tick"}));
    assert!(
        effects(&tick).is_empty(),
        "a later tick still emits nothing"
    );
}

#[test]
fn manual_events_rejected_command_fails_draft() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("brightness", 37.0));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    let token = first_submit(&tick)["token"]
        .as_u64()
        .expect("numeric token");

    let result = fx.send(json!({"type": "submitted", "token": token, "accepted": false}));
    let changed = effects(&result)
        .iter()
        .find(|effect| effect["type"] == "draft_changed")
        .expect("a draft_changed effect");
    assert_eq!(changed["monitor_id"], "m1");
    assert_eq!(changed["feature"], "brightness");

    let draft =
        fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "brightness"}));
    assert_eq!(draft["value"]["has_draft"], true);
    assert_eq!(draft["value"]["status"], "failed");
    assert_eq!(draft["value"]["draft"].as_f64(), Some(41.0));
}

#[test]
fn manual_events_query_draft_reports_pending_draft() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("brightness", 37.0));

    let draft =
        fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "brightness"}));
    assert_eq!(draft["value"]["has_draft"], true);
    assert_eq!(draft["value"]["draft"].as_f64(), Some(37.0));
    assert_eq!(draft["value"]["status"], "pending");
}

#[test]
fn manual_events_stale_observe_is_ignored_and_equal_revision_applies() {
    let mut fx = Fx::new();
    fx.send(observe_json(5, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("brightness", 37.0));

    // A lower revision is ignored, so the draft stays.
    fx.send(observe_json(4, vec![m1_json(1, "asleep")]));
    let draft =
        fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "brightness"}));
    assert_eq!(
        draft["value"]["has_draft"], true,
        "a stale observe keeps the draft"
    );
    assert_eq!(draft["value"]["draft"].as_f64(), Some(37.0));

    // An equal revision is applied, so the asleep state drops the draft.
    fx.send(observe_json(5, vec![m1_json(1, "asleep")]));
    let draft =
        fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "brightness"}));
    assert_eq!(
        draft["value"]["has_draft"], false,
        "an equal revision is applied"
    );
}

#[test]
fn manual_events_asleep_observe_drops_draft() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("brightness", 37.0));

    fx.send(observe_json(2, vec![m1_json(1, "asleep")]));
    let draft =
        fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "brightness"}));
    assert_eq!(draft["value"]["has_draft"], false, "sleep drops the draft");
}

#[test]
fn manual_events_dropped_draft_clears_the_drain_deadline() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    let slider = fx.send(slider_json("brightness", 30.0));
    assert!(
        deadline(&slider).is_some(),
        "a queued slider sets a deadline"
    );

    // The monitor sleeps before the drain: the draft goes and so does its deadline.
    let asleep = fx.send(observe_json(2, vec![m1_json(1, "asleep")]));
    assert_eq!(
        deadline(&asleep),
        None,
        "a dropped draft leaves no deadline"
    );

    // After waking, a new slider sets a fresh deadline from the current time.
    fx.send(observe_json(3, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    let again = fx.send(slider_json("brightness", 30.0));
    let next = deadline(&again).expect("a queued slider sets a deadline");
    assert!(
        (next - (fx.now + 0.020)).abs() < 1e-9,
        "the deadline is 20 ms after the slider"
    );
}

#[test]
fn manual_events_shutdown_ignores_slider() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.send(json!({"type": "shutdown"}));
    fx.advance(1.0);

    let result = fx.send(slider_json("brightness", 37.0));
    assert_eq!(result["value"], Value::Null);
    assert!(
        effects(&result).is_empty(),
        "a slider after shutdown emits nothing"
    );
    assert_eq!(deadline(&result), None);
}

#[test]
fn manual_events_volume_slider_submits_set_volume() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    fx.advance(1.0);
    fx.send(slider_json("volume", 55.0));

    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        effects(&tick).len(),
        1,
        "exactly one effect at the deadline"
    );
    assert_eq!(effects(&tick)[0]["type"], "submit_command");
    let command = command_of(&tick);
    assert_eq!(command["type"], "set_volume");
    assert_eq!(command["value"].as_f64(), Some(55.0));
}

#[test]
fn key_events_volume_up_press_queues_a_step() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    configure_keys(&mut fx, true, 5.0, 5.0);
    fx.advance(1.0);

    let press = fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    assert_eq!(
        press["value"]["consume"], true,
        "an owned volume key is consumed"
    );
    let changed = effects(&press)
        .iter()
        .find(|effect| effect["type"] == "draft_changed")
        .expect("a draft_changed effect");
    assert_eq!(changed["monitor_id"], "m1");
    assert_eq!(changed["feature"], "volume");
    let next = deadline(&press).expect("a queued key sets a deadline");
    assert!(
        (next - (fx.now + 0.020)).abs() < 1e-9,
        "the deadline is 20 ms after the press"
    );

    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_volume".to_owned(), 25.0)]
    );
}

#[test]
fn key_events_pass_through_when_keys_are_disabled_or_unowned() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));

    configure_keys(&mut fx, false, 5.0, 5.0);
    fx.advance(1.0);
    let press = fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    assert!(!consumed(&press), "disabled keys pass through");
    assert!(effects(&press).is_empty(), "disabled keys emit nothing");

    configure_keys(&mut fx, true, 5.0, 5.0);
    for (phase, repeat) in [("down", false), ("down", true), ("up", false)] {
        let result = fx.send(key_json("volume_up", phase, repeat, false, None));
        assert!(!consumed(&result), "an unowned {phase} key passes through");
        assert!(
            effects(&result).is_empty(),
            "an unowned {phase} key emits nothing"
        );
    }
}

#[test]
fn key_events_held_key_coalesces_into_one_write() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    configure_keys(&mut fx, true, 5.0, 5.0);
    fx.advance(1.0);

    let down = fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    let repeat = fx.send(key_json("volume_up", "down", true, false, Some("m1")));
    let again = fx.send(key_json("volume_up", "down", true, false, Some("m1")));
    for result in [&down, &repeat, &again] {
        assert!(consumed(result), "every press of an owned key is consumed");
    }

    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_volume".to_owned(), 35.0)],
        "the held key is one write"
    );

    let up = fx.send(key_json("volume_up", "up", false, false, Some("m1")));
    assert!(consumed(&up), "key-up is consumed");
    assert!(effects(&up).is_empty(), "key-up adds no effects");
}

#[test]
fn key_events_stale_snapshot_does_not_block_a_rebase() {
    let mut fx = Fx::new();
    fx.send(observe_levels(5, 41.0, 20.0));
    configure_keys(&mut fx, true, 5.0, 5.0);
    fx.advance(1.0);

    // First press: 20 becomes 25, is written and accepted.
    fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    fx.send(key_json("volume_up", "up", false, false, Some("m1")));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    let token = first_submit(&tick)["token"].clone();
    fx.send(json!({"type": "submitted", "token": token, "accepted": true}));

    // A snapshot older than revision 5 lists a pending volume action. It must not make the key
    // policy think one of its own writes is outstanding.
    let pending = json!({
        "id": 9, "generation": 2, "monitor_id": "m1", "kind": "set_volume", "value": 60.0
    });
    fx.send(observe_event(3, vec![m1_levels(41.0, 60.0)], vec![pending]));

    // The volume then really moved to 60 (an external change): the next press starts from it.
    fx.send(observe_levels(6, 41.0, 60.0));
    fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    let view = fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "volume"}));
    assert_eq!(view["value"]["draft"].as_f64(), Some(65.0));
}

#[test]
fn key_events_brightness_and_volume_use_their_own_steps() {
    let mut fx = Fx::new();
    fx.send(observe_levels(1, 41.0, 20.0));
    configure_keys(&mut fx, true, 3.0, 8.0);
    fx.advance(1.0);

    let brightness = fx.send(key_json("brightness_up", "down", false, false, Some("m1")));
    fx.send(key_json("brightness_up", "up", false, false, Some("m1")));
    let volume = fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    fx.send(key_json("volume_up", "up", false, false, Some("m1")));
    assert!(consumed(&brightness) && consumed(&volume));

    // Both drafts drain on the same tick; sort by type so the order of effects does not matter.
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    let mut writes = submitted_writes(&tick);
    writes.sort_by(|a, b| a.0.cmp(&b.0));
    assert_eq!(
        writes,
        vec![
            ("set_brightness".to_owned(), 44.0),
            ("set_volume".to_owned(), 28.0)
        ]
    );
}

#[test]
fn key_events_step_from_the_slider_target() {
    let mut fx = Fx::new();
    fx.send(observe_levels(1, 41.0, 20.0));
    configure_keys(&mut fx, true, 5.0, 5.0);
    fx.advance(1.0);

    fx.send(slider_json("brightness", 40.0));
    let press = fx.send(key_json("brightness_up", "down", false, false, Some("m1")));
    assert!(consumed(&press));

    // The key steps from the slider target 40, not from the readback 41.
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_brightness".to_owned(), 45.0)]
    );
}

#[test]
fn key_events_mute_writes_zero_and_consumes_its_release() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    configure_keys(&mut fx, true, 5.0, 5.0);
    fx.advance(1.0);

    let press = fx.send(key_json("mute", "down", false, false, Some("m1")));
    assert!(consumed(&press), "an owned mute is consumed");

    // The restore of the remembered level needs completion, which is not covered at this level.
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_volume".to_owned(), 0.0)]
    );

    let up = fx.send(key_json("mute", "up", false, false, Some("m1")));
    assert!(consumed(&up), "the mute key-up is consumed");
}

#[test]
fn key_events_reject_an_owner_with_extra_fields() {
    let mut fx = Fx::new();
    fx.send(observe_json(1, vec![m1_json(1, "awake")]));
    configure_keys(&mut fx, true, 5.0, 5.0);
    fx.advance(1.0);

    let error = fx
        .try_send(json!({
            "type": "key", "key": "volume_up", "phase": "down", "repeat": false, "fine": false,
            "owner": { "monitor_id": "m1", "generation": 1, "extra": 1 }
        }))
        .expect_err("an owner with an extra field must be rejected");
    assert!(matches!(error, StepError::UnknownField { .. }));

    // The rejected press left no state behind: the next valid press steps from 20 to 25.
    let press = fx.send(key_json("volume_up", "down", false, false, Some("m1")));
    assert!(consumed(&press));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_volume".to_owned(), 25.0)]
    );
}

// End-to-end tests: a real `Core` and `Controls` exchange events the way the shells do. Each
// effect is passed on verbatim, and each core snapshot becomes an `observe` event.

use telaviva_core::{Core, CoreOptions};

/// Submits one event to the core and returns its snapshot.
fn core_send(core: &mut Core, event: Value) -> Value {
    core.submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap();
    core.snapshot_value().unwrap()
}

/// A capture time `offset_ms` after the start of a minute, as in `core_contract.rs`.
fn capture_time(offset_ms: u64) -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64
        - 60_000
        + offset_ms
}

/// A core with one awake, DDC-capable monitor `m1` at the given raw brightness and volume.
fn core_with_m1(brightness: f64, volume: f64) -> Core {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    core_send(
        &mut core,
        json!({
            "type": "monitor_discovered",
            "monitor": {
                "id": "m1", "name": "m1", "identity_status": "stable", "connection_path": "usb",
                "brightness": {"supported": true, "value": brightness, "raw": brightness, "min": 0, "max": 100},
                "volume": {"supported": true, "value": volume, "raw": volume, "min": 0, "max": 100}
            }
        }),
    );
    core
}

/// One channel of a snapshot monitor, in the `observe` shape. `observed` is the normalized
/// percentage; `min` and `max` stay raw.
fn observe_channel(feature: &Value) -> Value {
    json!({
        "supported": feature["supported"].clone(),
        "observed": feature["normalized"].clone(),
        "min": feature["min"].clone(),
        "max": feature["max"].clone()
    })
}

/// Converts a core snapshot into a Controls `observe` event.
fn world_to_observe(snapshot: &Value, revision: u64) -> Value {
    let monitors: Vec<Value> = snapshot["monitors"]
        .as_array()
        .expect("the snapshot lists monitors")
        .iter()
        .map(|m| {
            json!({
                "id": m["id"].clone(),
                "generation": m["generation"].clone(),
                "display_state": m["display_state"].clone(),
                "identity_status": m["identity_status"].clone(),
                "brightness": observe_channel(&m["brightness"]),
                "volume": observe_channel(&m["volume"])
            })
        })
        .collect();
    let pending: Vec<Value> = snapshot["pending_actions"]
        .as_array()
        .expect("the snapshot lists pending actions")
        .iter()
        .map(|a| {
            json!({
                "id": a["id"].clone(),
                "generation": a["generation"].clone(),
                "monitor_id": a["monitor_id"].clone(),
                "kind": a["kind"].clone(),
                "value": a["value"].clone()
            })
        })
        .collect();
    json!({
        "type": "observe",
        "revision": revision,
        "monitors": monitors,
        "pending_actions": pending
    })
}

/// Sends the core's current snapshot to Controls as an `observe` event.
fn observe_core(fx: &mut Fx, core: &Core) -> Value {
    let snapshot = core.snapshot_value().unwrap();
    let revision = snapshot["revision"]
        .as_u64()
        .expect("the snapshot has a revision");
    fx.send(world_to_observe(&snapshot, revision))
}

/// A fresh machine that has observed the core's monitors and knows the profile of `m1`.
fn controls_for(core: &Core) -> Fx {
    let mut fx = Fx::new();
    fx.advance(1.0);
    observe_core(&mut fx, core);
    fx.send(json!({"type": "monitor_profile", "monitor_id": "m1", "manufacturer": "DEL"}));
    fx
}

/// The first effect of the given `type` in a result.
fn effect_of<'a>(result: &'a Value, kind: &str) -> Option<&'a Value> {
    effects(result).iter().find(|effect| effect["type"] == kind)
}

/// Passes a `submit_command` or `submit_result` effect verbatim to the core, then reports the
/// acceptance to Controls.
fn deliver(fx: &mut Fx, core: &mut Core, effect: &Value) -> Value {
    let text_key = if effect["type"] == "submit_command" {
        "command_json"
    } else {
        "result_json"
    };
    let text = effect[text_key]
        .as_str()
        .expect("the effect carries its JSON text");
    core.submit_json(text.as_bytes())
        .expect("the core accepts the effect verbatim");
    fx.send(json!({"type": "submitted", "token": effect["token"].clone(), "accepted": true}))
}

/// An `attempt_done` for a DDC primitive that succeeded. `value` is the raw readback for a read.
fn attempt_ok(token: &Value, value: Value) -> Value {
    json!({"type": "attempt_done", "token": token.clone(), "ok": true, "value": value})
}

/// An `attempt_done` for a DDC primitive that failed with an I/O error.
fn attempt_io(token: &Value) -> Value {
    json!({
        "type": "attempt_done", "token": token.clone(), "ok": false, "error": "io", "value": null
    })
}

/// A `key` event from the owner `m1` at the given generation.
fn key_for(key: &str, phase: &str, generation: &Value) -> Value {
    json!({
        "type": "key", "key": key, "phase": phase, "repeat": false, "fine": false,
        "owner": {"monitor_id": "m1", "generation": generation.clone()}
    })
}

/// Asserts that two times or percentages agree within 1e-9.
fn assert_near(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() < 1e-9,
        "{actual} is not {expected}"
    );
}

/// Drives one successful DDC write from its `submit_command` to the `submit_result` that the core
/// has not seen yet. The command goes to the core, then the write, the 60 ms settle and the
/// readback run as Controls asks. Returns the `submit_result` effect.
fn run_write_to_result(fx: &mut Fx, core: &mut Core, tick: &Value, code: u64, raw: f64) -> Value {
    let command = first_submit(tick).clone();
    deliver(fx, core, &command);
    let observed = observe_core(fx, core);
    let write = effect_of(&observed, "run_attempt")
        .expect("the listed write runs")
        .clone();
    assert_eq!(write["op"], "write");
    assert_eq!(write["code"], code);
    assert_near(write["value"].as_f64().unwrap(), raw);

    let written = fx.send(attempt_ok(&write["token"], Value::Null));
    let settle = deadline(&written).expect("a write sets the settle deadline");
    assert_near(settle, fx.now + 0.060);
    fx.now = settle;

    let settled = fx.send(json!({"type": "tick"}));
    let read = effect_of(&settled, "run_attempt")
        .expect("the readback runs after the settle")
        .clone();
    assert_eq!(read["op"], "read");
    assert_eq!(read["code"], code);
    let read_back = fx.send(attempt_ok(&read["token"], json!(raw)));
    effect_of(&read_back, "submit_result")
        .expect("a matching readback completes the action")
        .clone()
}

#[test]
fn e2e_slider_reaches_the_wire_and_clears_the_draft() {
    let mut core = core_with_m1(40.0, 20.0);
    let mut fx = controls_for(&core);

    fx.send(slider_json("brightness", 70.0));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_brightness".to_owned(), 70.0)]
    );

    let result = run_write_to_result(&mut fx, &mut core, &tick, 16, 70.0);
    deliver(&mut fx, &mut core, &result);
    observe_core(&mut fx, &core);

    let draft =
        fx.send(json!({"type": "query_draft", "monitor_id": "m1", "feature": "brightness"}));
    assert_eq!(draft["value"]["has_draft"], false, "the draft is cleared");
    assert_near(draft["value"]["draft"].as_f64().unwrap(), 70.0);
    let snapshot = core.snapshot_value().unwrap();
    assert_near(
        snapshot["monitors"][0]["brightness"]["normalized"]
            .as_f64()
            .unwrap(),
        70.0,
    );
}

#[test]
fn e2e_volume_key_press_reaches_the_wire() {
    let mut core = core_with_m1(40.0, 20.0);
    let mut fx = controls_for(&core);
    configure_keys(&mut fx, true, 5.0, 5.0);
    let generation = core.snapshot_value().unwrap()["monitors"][0]["generation"].clone();

    let press = fx.send(key_for("volume_up", "down", &generation));
    assert!(consumed(&press), "an owned volume key is consumed");
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    assert_eq!(
        submitted_writes(&tick),
        vec![("set_volume".to_owned(), 25.0)]
    );
    let up = fx.send(key_for("volume_up", "up", &generation));
    assert!(consumed(&up), "key-up is consumed");

    let result = run_write_to_result(&mut fx, &mut core, &tick, 98, 25.0);
    deliver(&mut fx, &mut core, &result);
    observe_core(&mut fx, &core);

    // The volume was 20 and one volume step is 5.
    let snapshot = core.snapshot_value().unwrap();
    assert_near(
        snapshot["monitors"][0]["volume"]["normalized"]
            .as_f64()
            .unwrap(),
        25.0,
    );
}

#[test]
fn e2e_core_rejection_is_resubmitted_not_rewritten() {
    let mut core = core_with_m1(40.0, 20.0);
    let mut fx = controls_for(&core);
    fx.send(slider_json("brightness", 70.0));
    fx.advance(0.020);
    let tick = fx.send(json!({"type": "tick"}));
    let result = run_write_to_result(&mut fx, &mut core, &tick, 16, 70.0);

    // The shell reports the result as rejected and does not pass it to the core.
    let rejected = fx.send(json!({
        "type": "submitted", "token": result["token"].clone(), "accepted": false
    }));
    let retry_at = deadline(&rejected).expect("a rejected result is resubmitted after a backoff");
    assert_near(retry_at, fx.now + 0.2);
    fx.now = retry_at;
    let again = fx.send(json!({"type": "tick"}));
    let resubmitted = effect_of(&again, "submit_result")
        .expect("the same result is submitted again")
        .clone();
    assert_eq!(resubmitted["result_json"], result["result_json"]);
    assert_ne!(
        resubmitted["token"], result["token"],
        "a resubmission has a new token"
    );

    // The core still lists the write, but Controls must not run it again.
    let observed = observe_core(&mut fx, &core);
    assert!(
        effect_of(&observed, "run_attempt").is_none(),
        "the write is not rewritten"
    );

    deliver(&mut fx, &mut core, &resubmitted);
    observe_core(&mut fx, &core);
    let snapshot = core.snapshot_value().unwrap();
    assert_near(
        snapshot["monitors"][0]["brightness"]["normalized"]
            .as_f64()
            .unwrap(),
        70.0,
    );
}

/// Plays the shell for one stuck monitor: every DDC attempt fails with an I/O error, and every
/// result goes to the core until the core lists no more actions. Returns the number of DDC
/// primitives, the number of finished operations and the backoff waited after failed attempts.
fn drive_stuck_reads(fx: &mut Fx, core: &mut Core, mut result: Value) -> (usize, usize, f64) {
    let (mut primitives, mut operations, mut backoff) = (0, 0, 0.0);
    for _ in 0..100 {
        if let Some(attempt) = effect_of(&result, "run_attempt").cloned() {
            assert_eq!(attempt["op"], "read", "a stuck monitor is only read back");
            primitives += 1;
            let answered = fx.send(attempt_io(&attempt["token"]));
            match deadline(&answered) {
                // The retry backoff after a failed attempt is the only wait that is counted.
                Some(next) if effects(&answered).is_empty() => {
                    backoff += next - fx.now;
                    fx.now = next;
                    result = fx.send(json!({"type": "tick"}));
                }
                _ => result = answered,
            }
        } else if let Some(submit) = effect_of(&result, "submit_result").cloned() {
            operations += 1;
            deliver(fx, core, &submit);
            result = observe_core(fx, core);
        } else if let Some(next) = deadline(&result) {
            // Not a retry backoff, for example a wake grace: waited on but not counted.
            fx.now = next;
            result = fx.send(json!({"type": "tick"}));
        } else {
            return (primitives, operations, backoff);
        }
    }
    panic!("the stuck-monitor episode did not settle");
}

#[test]
fn e2e_retry_ceiling_for_a_stuck_monitor() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = core_send(
        &mut core,
        json!({
            "type": "monitor_discovered",
            "monitor": {
                "id": "m1", "name": "m1", "identity_status": "stable", "connection_path": "usb",
                "brightness": {"supported": true, "value": 70, "raw": 70, "min": 0, "max": 100},
                "volume": {"supported": false, "value": null, "raw": null, "min": 0, "max": 100}
            }
        }),
    );
    // Controls learns the monitor before it is dimmed, so it never sees the dim write.
    let mut fx = controls_for(&core);
    core_send(&mut core, json!({"type": "power", "on_battery": false}));
    let generation = discovered["monitors"][0]["generation"].clone();
    let enabled = core_send(
        &mut core,
        json!({"type": "update_settings", "settings": {"presence_protection_enabled": true}}),
    );
    for offset in [10_000, 20_000] {
        core_send(
            &mut core,
            json!({
                "type": "presence_observation", "status": "absent",
                "captured_at_ms": capture_time(offset),
                "camera_generation": enabled["camera"]["generation"]
            }),
        );
    }
    // The core's dim is acknowledged directly: this test only exercises the readbacks.
    let dim = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    core_send(
        &mut core,
        json!({
            "type": "hardware_result", "action_id": dim["id"], "generation": dim["generation"],
            "ok": true, "value": 0
        }),
    );
    core_send(
        &mut core,
        json!({
            "type": "monitor_display_state", "monitor_id": "m1",
            "generation": generation, "state": "asleep"
        }),
    );
    core_send(
        &mut core,
        json!({
            "type": "monitor_display_state", "monitor_id": "m1",
            "generation": generation, "state": "awake"
        }),
    );

    let observed = observe_core(&mut fx, &core);
    let (primitives, operations, backoff) = drive_stuck_reads(&mut fx, &mut core, observed);
    assert!(
        primitives <= 12,
        "at most 4 read operations x 3 attempts, got {primitives}"
    );
    assert_eq!(
        operations, 4,
        "the core retries the readback three times and then gives up"
    );
    assert!(
        backoff <= 4.8 + 1e-9,
        "backoff is at most 4 x (0.2 + 1.0) s, got {backoff}"
    );
}
