use serde_json::{json, Value};
use telaviva_core::{Core, CoreOptions};

// Helpers are copied from core_contract.rs so this file stands alone.

fn send(core: &mut Core, event: Value) -> Value {
    core.submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap();
    core.snapshot_value().unwrap()
}

fn capture_time(offset_ms: u64) -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64
        - 60_000
        + offset_ms
}

fn discover_monitor(core: &mut Core, id: &str, raw: Value) -> Value {
    discover_monitor_as(core, id, "stable", true, raw)
}

// Discovers a monitor with the given identity status and brightness support at a raw level.
fn discover_monitor_as(
    core: &mut Core,
    id: &str,
    identity: &str,
    supported: bool,
    raw: Value,
) -> Value {
    send(
        core,
        json!({"type":"monitor_discovered","monitor":{
            "id":id,"name":id,"identity_status":identity,"connection_path":"usb",
            "brightness":{"supported":supported,"value":raw,"raw":raw,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    )
}

fn enable_presence(core: &mut Core) -> Value {
    send(core, json!({"type":"power","on_battery":false}));
    send(
        core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    )
}

fn monitor_generation(snapshot: &Value, id: &str) -> Value {
    snapshot["monitors"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["id"] == id)
        .unwrap()["generation"]
        .clone()
}

fn pending_action(snapshot: &Value, kind: &str) -> Value {
    snapshot["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == kind)
        .unwrap_or_else(|| panic!("missing pending {kind} action"))
        .clone()
}

// Pending actions for one monitor. The snapshot lists at most one action per monitor.
fn monitor_actions(snapshot: &Value, id: &str) -> Vec<Value> {
    snapshot["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|a| a["monitor_id"] == id)
        .cloned()
        .collect()
}

fn hardware_ok(core: &mut Core, action: &Value, value: f64) -> Value {
    send(
        core,
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":value}),
    )
}

// Sends a presence sample tagged with the current camera generation.
fn presence_sample(core: &mut Core, status: &str, offset_ms: u64) -> Value {
    let generation = core.snapshot_value().unwrap()["camera"]["generation"].clone();
    send(
        core,
        json!({"type":"presence_observation","status":status,"captured_at_ms":capture_time(offset_ms),"camera_generation":generation}),
    )
}

// Monitor "m1" at raw 45, dimmed to the protection level 1 by two absent samples. The dim write is
// acknowledged, so the core owns the device value.
fn dimmed_monitor() -> Core {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "m1", json!(45));
    enable_presence(&mut core);
    presence_sample(&mut core, "absent", 10_000);
    let s = presence_sample(&mut core, "absent", 20_000);
    let dim = pending_action(&s, "set_brightness");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    let s = hardware_ok(&mut core, &dim, 1.0);
    assert_eq!(s["monitors"][0]["brightness"]["owned"], true);
    core
}

// The dimmed monitor goes to sleep and is then disconnected while the device still holds 1.
fn dimmed_then_disconnected() -> Core {
    let mut core = dimmed_monitor();
    let s = core.snapshot_value().unwrap();
    let generation = monitor_generation(&s, "m1");
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"m1","generation":generation,"state":"asleep"}),
    );
    send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"m1"}),
    );
    core
}

#[test]
fn dim_left_by_sleep_and_disconnect_is_restored_when_the_monitor_returns_at_the_dimmed_level() {
    let mut core = dimmed_then_disconnected();
    let s = discover_monitor(&mut core, "m1", json!(1));
    let read = pending_action(&s, "read_brightness");
    assert_eq!(read["monitor_id"], "m1");
    let s = hardware_ok(&mut core, &read, 1.0);
    let restore = pending_action(&s, "set_brightness");
    assert_eq!(restore["monitor_id"], "m1");
    assert_eq!(restore["value"].as_f64(), Some(45.0));
    let lines = core.drain_diagnostics();
    assert!(lines
        .iter()
        .any(|line| line.starts_with("dim_resumed monitor=")));
    let s = hardware_ok(&mut core, &restore, 45.0);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(45.0));
    assert!(monitor_actions(&s, "m1").is_empty());
}

#[test]
fn dim_is_restored_when_the_monitor_is_replaced_in_place_at_the_dimmed_level() {
    let mut core = dimmed_monitor();
    let s = discover_monitor(&mut core, "m1", json!(1));
    let read = pending_action(&s, "read_brightness");
    let s = hardware_ok(&mut core, &read, 1.0);
    let restore = pending_action(&s, "set_brightness");
    assert_eq!(restore["value"].as_f64(), Some(45.0));
    let s = hardware_ok(&mut core, &restore, 45.0);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(45.0));
}

#[test]
fn monitor_returning_at_another_level_adopts_it_and_restores_nothing() {
    let mut core = dimmed_then_disconnected();
    let s = discover_monitor(&mut core, "m1", json!(30));
    assert!(monitor_actions(&s, "m1").is_empty());
    assert_eq!(s["monitors"][0]["brightness"]["owned"], false);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(30.0));
    let lines = core.drain_diagnostics();
    assert!(!lines.iter().any(|line| line.starts_with("dim_resumed")));
    // The adopted level is the new baseline: a later dim and restore return to 30, not 45.
    presence_sample(&mut core, "absent", 30_000);
    let s = presence_sample(&mut core, "absent", 40_000);
    let dim = pending_action(&s, "set_brightness");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    hardware_ok(&mut core, &dim, 1.0);
    let s = presence_sample(&mut core, "present", 50_000);
    let read = pending_action(&s, "read_brightness");
    let s = hardware_ok(&mut core, &read, 1.0);
    let restore = pending_action(&s, "set_brightness");
    assert_eq!(restore["value"].as_f64(), Some(30.0));
    let s = hardware_ok(&mut core, &restore, 30.0);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(30.0));
}

// An ambiguous rediscovery (duplicate EDID) does not establish which monitor is attached, so it
// must not use up the parked dim. A later stable rediscovery at the dimmed level still restores it.
#[test]
fn dim_survives_an_ambiguous_rediscovery_and_is_restored_by_a_later_stable_one() {
    let mut core = dimmed_then_disconnected();
    let s = discover_monitor_as(&mut core, "m1", "ambiguous", true, json!(1));
    assert!(monitor_actions(&s, "m1").is_empty());
    let lines = core.drain_diagnostics();
    assert!(!lines.iter().any(|line| line.starts_with("dim_resumed")));
    let s = send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"m1"}),
    );
    assert!(monitor_actions(&s, "m1").is_empty());
    let s = discover_monitor(&mut core, "m1", json!(1));
    let read = pending_action(&s, "read_brightness");
    assert_eq!(read["monitor_id"], "m1");
    let s = hardware_ok(&mut core, &read, 1.0);
    let restore = pending_action(&s, "set_brightness");
    assert_eq!(restore["monitor_id"], "m1");
    assert_eq!(restore["value"].as_f64(), Some(45.0));
    let lines = core.drain_diagnostics();
    assert!(lines
        .iter()
        .any(|line| line.starts_with("dim_resumed monitor=")));
    let s = hardware_ok(&mut core, &restore, 45.0);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(45.0));
    assert!(monitor_actions(&s, "m1").is_empty());
}

// A rediscovery whose brightness is unsupported also leaves the parked dim in place. A later
// supported rediscovery at the dimmed level restores the baseline.
#[test]
fn dim_survives_an_unsupported_rediscovery_and_is_restored_by_a_later_supported_one() {
    let mut core = dimmed_then_disconnected();
    let s = discover_monitor_as(&mut core, "m1", "stable", false, json!(1));
    assert!(monitor_actions(&s, "m1").is_empty());
    let s = send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"m1"}),
    );
    assert!(monitor_actions(&s, "m1").is_empty());
    let s = discover_monitor(&mut core, "m1", json!(1));
    let read = pending_action(&s, "read_brightness");
    assert_eq!(read["monitor_id"], "m1");
    let s = hardware_ok(&mut core, &read, 1.0);
    let restore = pending_action(&s, "set_brightness");
    assert_eq!(restore["monitor_id"], "m1");
    assert_eq!(restore["value"].as_f64(), Some(45.0));
    let s = hardware_ok(&mut core, &restore, 45.0);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(45.0));
}

#[test]
fn monitor_never_dimmed_and_rediscovered_at_the_same_level_gets_no_action() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "m1", json!(45));
    send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"m1"}),
    );
    let s = discover_monitor(&mut core, "m1", json!(45));
    assert!(monitor_actions(&s, "m1").is_empty());
    assert_eq!(s["monitors"][0]["brightness"]["owned"], false);
    let lines = core.drain_diagnostics();
    assert!(!lines.iter().any(|line| line.starts_with("dim_resumed")));
}

#[test]
fn manual_brightness_is_stored_as_a_whole_raw_unit_and_is_not_rewritten() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "m1", json!(70));
    let s = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"m1","value":44.4416}),
    );
    let write = pending_action(&s, "set_brightness");
    assert_eq!(write["value"].as_f64(), Some(44.0));
    assert_eq!(
        s["monitors"][0]["brightness"]["desired"].as_f64(),
        Some(44.0)
    );
    let s = hardware_ok(&mut core, &write, 44.0);
    assert_eq!(s["monitors"][0]["brightness"]["raw"].as_f64(), Some(44.0));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // A built-in display report reconciles every monitor and ends the manual override. The
    // readback 44 must match the stored level, so no read or write follows. Before the rounding
    // fix the stored level was 44.4416, and this report queued a read on every cycle.
    let s = send(
        &mut core,
        json!({"type":"builtin_display","available":false,"brightness":null,"calibrating":false}),
    );
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
}

#[test]
fn parked_dims_hold_sixteen_monitors_and_drop_the_first_id_when_full() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let ids: Vec<String> = (0..17).map(|i| format!("m{i:02}")).collect();
    for id in &ids {
        discover_monitor(&mut core, id, json!(45));
    }
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true}}),
    );
    let mut s = send(&mut core, json!({"type":"screensaver","state":"active"}));
    // The snapshot lists at most eight pending actions, so the dims are acknowledged in rounds.
    for _ in 0..10 {
        let pending = s["pending_actions"].as_array().unwrap().clone();
        for action in &pending {
            s = hardware_ok(&mut core, action, 1.0);
        }
    }
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    for id in &ids {
        send(
            &mut core,
            json!({"type":"monitor_disconnected","monitor_id":id}),
        );
    }
    // m00 was parked first and is dropped when m16 fills the last slot. m16 is still parked.
    discover_monitor(&mut core, "m00", json!(1));
    discover_monitor(&mut core, "m16", json!(1));
    let s = send(&mut core, json!({"type":"screensaver","state":"inactive"}));
    assert!(monitor_actions(&s, "m00").is_empty());
    let read = pending_action(&s, "read_brightness");
    assert_eq!(read["monitor_id"], "m16");
}
