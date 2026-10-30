use serde_json::{json, Value};
use telaviva_core::{Core, CoreOptions};

fn send(core: &mut Core, event: Value) -> Value {
    core.submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap();
    core.snapshot_value().unwrap()
}

fn core_with_defaults() -> Core {
    Core::new(CoreOptions::default()).unwrap()
}

#[test]
fn builtin_display_accepts_protectable_and_holding_flags() {
    let mut core = core_with_defaults();
    let snapshot = send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":62,"calibrating":false,"protectable":true,"holding":false}),
    );
    assert_eq!(snapshot["builtin_protection"]["protectable"], true);
    assert_eq!(snapshot["builtin_protection"]["holding"], false);
}

#[test]
fn legacy_builtin_display_without_the_new_keys_leaves_protectable_and_holding_false() {
    let mut core = core_with_defaults();
    let snapshot = send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":62,"calibrating":false}),
    );
    assert_eq!(snapshot["builtin_protection"]["protectable"], false);
    assert_eq!(snapshot["builtin_protection"]["holding"], false);
    send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":62,"calibrating":false,"protectable":true,"holding":true}),
    );
    // An absent key means false, so a legacy report clears the earlier opt-in.
    let snapshot = send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":62,"calibrating":false}),
    );
    assert_eq!(snapshot["builtin_protection"]["protectable"], false);
    assert_eq!(snapshot["builtin_protection"]["holding"], false);
}

#[test]
fn non_boolean_protectable_or_holding_is_rejected_without_changing_state() {
    let mut core = core_with_defaults();
    let before = send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":50,"calibrating":false,"protectable":true,"holding":false}),
    );
    let rejected = [
        (
            json!({"type":"builtin_display","available":false,"brightness":10,"calibrating":true,"protectable":"yes"}),
            "protectable must be boolean",
        ),
        (
            json!({"type":"builtin_display","available":false,"brightness":10,"calibrating":true,"holding":1}),
            "holding must be boolean",
        ),
    ];
    for (event, message) in rejected {
        let error = core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .unwrap_err();
        assert_eq!(error, message);
        let after = core.snapshot_value().unwrap();
        assert_eq!(after["revision"], before["revision"]);
        assert_eq!(after["builtin_protection"], before["builtin_protection"]);
        assert_eq!(after["ambient"], before["ambient"]);
    }
}

#[test]
fn non_boolean_holding_is_rejected_even_when_protectable_is_false() {
    let mut core = core_with_defaults();
    let before = send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":50,"calibrating":false,"protectable":true,"holding":false}),
    );
    // holding is validated even though the shell did not opt in, so the whole report is rejected.
    let event = json!({"type":"builtin_display","available":false,"brightness":10,"calibrating":true,"protectable":false,"holding":"yes"});
    let error = core
        .submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap_err();
    assert_eq!(error, "holding must be boolean");
    let after = core.snapshot_value().unwrap();
    assert_eq!(after["revision"], before["revision"]);
    assert_eq!(after["builtin_protection"], before["builtin_protection"]);
    assert_eq!(after["ambient"], before["ambient"]);
}

#[test]
fn fresh_snapshot_publishes_an_inert_builtin_protection_object() {
    let core = core_with_defaults();
    let snapshot = core.snapshot_value().unwrap();
    assert_eq!(
        snapshot["builtin_protection"],
        json!({
            "protectable": false,
            "protected": false,
            "available": false,
            "reasons": [],
            "target_percent": null,
            "hold_auto_brightness": false,
            "holding": false
        })
    );
}

#[test]
fn builtin_display_never_adds_a_builtin_entry_to_monitors() {
    let mut core = core_with_defaults();
    let snapshot = send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":62,"calibrating":false,"protectable":true,"holding":false}),
    );
    let monitors = snapshot["monitors"].as_array().unwrap();
    assert!(monitors.iter().all(|m| m["id"] != "builtin"));
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
    send(
        core,
        json!({"type":"monitor_discovered","monitor":{
            "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":raw,"raw":raw,"min":0,"max":100},
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

// Shell report of the built-in display, including the protectable and holding flags.
fn report(
    core: &mut Core,
    available: bool,
    brightness: f64,
    protectable: bool,
    holding: bool,
    calibrating: bool,
) -> Value {
    send(
        core,
        json!({"type":"builtin_display","available":available,"brightness":brightness,"calibrating":calibrating,"protectable":protectable,"holding":holding}),
    )
}

// Sends a presence sample tagged with the current camera generation.
// offset_ms must increase for every sample sent to the same core.
fn presence_sample(core: &mut Core, status: &str, offset_ms: u64) -> Value {
    let generation = core.snapshot_value().unwrap()["camera"]["generation"].clone();
    send(
        core,
        json!({"type":"presence_observation","status":status,"captured_at_ms":capture_time(offset_ms),"camera_generation":generation}),
    )
}

// The set_brightness action queued for one monitor, if the snapshot shows one.
fn set_brightness_for(snapshot: &Value, monitor_id: &str) -> Option<Value> {
    snapshot["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["monitor_id"] == monitor_id && a["kind"] == "set_brightness")
        .cloned()
}

#[test]
fn builtin_dims_after_two_absent_samples_and_clears_on_present() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    let s = presence_sample(&mut core, "absent", 1_000);
    // One miss alone does not dim.
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    assert_eq!(
        s["builtin_protection"]["target_percent"].as_f64(),
        Some(1.0)
    );
    let s = presence_sample(&mut core, "present", 3_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    assert!(s["builtin_protection"]["target_percent"].is_null());
}

#[test]
fn builtin_presence_and_screensaver_share_one_per_screen_level() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true}}),
    );
    let s = send(&mut core, json!({"type":"screensaver","state":"active"}));
    assert_eq!(s["builtin_protection"]["reasons"], json!(["screensaver"]));
    // The default protection level for the built-in is 1 percent, shared by presence and the
    // screensaver.
    assert_eq!(
        s["builtin_protection"]["target_percent"].as_f64(),
        Some(1.0)
    );
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_targets":{"builtin":35}}}),
    );
    assert_eq!(s["builtin_protection"]["reasons"], json!(["screensaver"]));
    assert_eq!(
        s["builtin_protection"]["target_percent"].as_f64(),
        Some(35.0)
    );
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    let reasons = s["builtin_protection"]["reasons"].as_array().unwrap();
    assert!(reasons.contains(&json!("presence")));
    assert!(reasons.contains(&json!("screensaver")));
    // Presence and the screensaver share one level, so both reasons dim the built-in to 35
    // percent and neither stacks a lower value on top.
    assert_eq!(
        s["builtin_protection"]["target_percent"].as_f64(),
        Some(35.0)
    );
}

#[test]
fn builtin_presence_uses_the_builtin_screen_target() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_targets":{"builtin":35}}}),
    );
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    // The screensaver is off, so presence alone sets the level: the built-in's own target of 35.
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    assert_eq!(
        s["builtin_protection"]["target_percent"].as_f64(),
        Some(35.0)
    );
}

#[test]
fn builtin_protection_can_be_opted_out_per_screen() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["builtin"]}}),
    );
    assert_eq!(s["builtin_protection"]["protected"], false);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    // Protection is back on, but the stale dim was cleared on opt-out: a new dim needs two fresh
    // absent samples.
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":[]}}),
    );
    assert_eq!(s["builtin_protection"]["protected"], true);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    let s = presence_sample(&mut core, "absent", 3_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    let s = presence_sample(&mut core, "absent", 4_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
}

#[test]
fn builtin_is_inert_when_the_shell_does_not_opt_in() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    // A legacy report has no protectable key, so the built-in is not protectable.
    send(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":60,"calibrating":false}),
    );
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    assert_eq!(s["builtin_protection"]["protected"], false);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_no_monitor");
}

#[test]
fn builtin_unavailable_calibrating_and_shutdown_empty_the_reasons() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // Unavailable empties the reasons and clears the misses and the dim.
    let s = report(&mut core, false, 60.0, true, false, false);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    report(&mut core, true, 60.0, true, false, false);
    // Two new absent samples are needed again because the earlier misses were cleared.
    presence_sample(&mut core, "absent", 3_000);
    let s = presence_sample(&mut core, "absent", 4_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // Calibrating empties the reasons.
    let s = report(&mut core, true, 60.0, true, false, true);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    report(&mut core, true, 60.0, true, false, false);
    presence_sample(&mut core, "absent", 5_000);
    let s = presence_sample(&mut core, "absent", 6_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // Shutdown empties the reasons for good.
    let s = send(&mut core, json!({"type":"shutdown"}));
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
}

#[test]
fn laptop_alone_runs_the_camera_and_gates_on_the_threshold() {
    // No external monitor: the built-in alone must run the camera.
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    let s = report(&mut core, true, 60.0, true, false, false);
    assert_eq!(s["camera"]["mode"], "face");
    // 15 percent is at or below the default presence_min_brightness_percent of 20.
    let s = report(&mut core, true, 15.0, true, false, false);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = report(&mut core, true, 60.0, true, false, false);
    assert_eq!(s["camera"]["mode"], "face");
    // An unavailable built-in (lid closed or display asleep) with no monitors is display sleep.
    let s = report(&mut core, false, 60.0, true, false, false);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_display_sleep");
    report(&mut core, true, 60.0, true, false, false);
    // Opted out: no eligible display is left.
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["builtin"]}}),
    );
    assert_eq!(s["camera"]["display_status"], "suspended_no_monitor");
}

#[test]
fn builtin_absence_confirmation_speeds_up_the_sampling() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    // The interval right after enabling is the short resume sample (1000 ms), not the presence
    // interval, so it is not asserted here.
    // One miss: the confirming check uses the return interval.
    let s = presence_sample(&mut core, "absent", 1_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(500.0));
    // Two misses dim the built-in, and the return interval continues while it is dimmed.
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(500.0));
    // Presence returns to the presence sampling interval.
    let s = presence_sample(&mut core, "present", 3_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(10000.0));
}

#[test]
fn builtin_freeze_keeps_the_last_undimmed_level_and_resumes_with_a_fresh_one() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // While holding, the dimmed level is not the built-in level: the core keeps the undimmed 60.
    let s = report(&mut core, true, 0.0, true, true, false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(60.0));
    let s = presence_sample(&mut core, "present", 3_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    // Still holding after the reasons cleared: the report stays frozen until the restore is done.
    let s = report(&mut core, true, 5.0, true, true, false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(60.0));
    // Restore finished: the fresh level is accepted again.
    let s = report(&mut core, true, 55.0, true, false, false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(55.0));
}

#[test]
fn holding_without_protectable_is_ignored_and_never_freezes_follow() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    discover_monitor(&mut core, "ext", json!(50));
    // The same curve shape as the follow fixture in core_contract.rs.
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"ext":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}}),
    );
    // A shell that did not opt in sends holding:true. The core must ignore it, not hold on it.
    let s = report(&mut core, true, 60.0, false, true, false);
    assert_eq!(s["builtin_protection"]["holding"], false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(60.0));
    // A built-in level of 60 maps to 65 percent, which is raw 65 on the 0..=100 range.
    let write = set_brightness_for(&s, "ext").expect("follow write for the built-in level");
    assert_eq!(write["value"].as_f64(), Some(65.0));
    let s = send(
        &mut core,
        json!({"type":"hardware_result","action_id":write["id"],"generation":write["generation"],"ok":true,"value":65.0}),
    );
    assert!(set_brightness_for(&s, "ext").is_none());
    // The next report is not frozen: the fresh level is accepted and follow moves with it.
    let s = report(&mut core, true, 55.0, false, true, false);
    assert_eq!(s["builtin_protection"]["holding"], false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(55.0));
    // The monitor is owned after the first write, so the core reads it back before writing.
    let read = s["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["monitor_id"] == "ext" && a["kind"] == "read_brightness")
        .cloned()
        .expect("ownership check before the new write");
    let s = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":65.0}),
    );
    // A built-in level of 55 maps to 60 percent, which is raw 60.
    let write = set_brightness_for(&s, "ext").expect("follow write for the new level");
    assert_eq!(write["value"].as_f64(), Some(60.0));
}

#[test]
fn externals_do_not_follow_the_dimmed_builtin_level() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    discover_monitor(&mut core, "ext", json!(50));
    // The same curve shape as the follow fixture in core_contract.rs.
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"ext":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}}),
    );
    // Only the built-in dims: the external monitor is opted out of protection.
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["ext"]}}),
    );
    // A built-in level of 60 maps to 65 percent, which is raw 65 on the 0..=100 range.
    let s = report(&mut core, true, 60.0, true, false, false);
    let write = set_brightness_for(&s, "ext").expect("follow write for the undimmed level");
    assert_eq!(write["value"].as_f64(), Some(65.0));
    let s = send(
        &mut core,
        json!({"type":"hardware_result","action_id":write["id"],"generation":write["generation"],"ok":true,"value":65.0}),
    );
    assert!(set_brightness_for(&s, "ext").is_none());
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // The dimmed built-in must not queue any write for the external monitor.
    assert!(set_brightness_for(&s, "ext").is_none());
    let s = report(&mut core, true, 0.0, true, true, false);
    assert!(set_brightness_for(&s, "ext").is_none());
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(60.0));
}

#[test]
fn hold_auto_brightness_requires_the_option_and_a_reason() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // The option is off by default, so a dim does not hold automatic brightness.
    assert_eq!(s["builtin_protection"]["hold_auto_brightness"], false);
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"builtin_auto_brightness_off_on_protection":true}}),
    );
    assert_eq!(s["builtin_protection"]["hold_auto_brightness"], true);
    let s = presence_sample(&mut core, "present", 3_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    assert_eq!(s["builtin_protection"]["hold_auto_brightness"], false);
}

#[test]
fn builtin_dim_is_cleared_by_battery_and_camera_permission() {
    let mut core = core_with_defaults();
    enable_presence(&mut core);
    report(&mut core, true, 60.0, true, false, false);
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // suspend_presence_on_battery defaults to true, so battery power clears the dim.
    let s = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
    send(&mut core, json!({"type":"power","on_battery":false}));
    presence_sample(&mut core, "absent", 3_000);
    let s = presence_sample(&mut core, "absent", 4_000);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
    // A camera permission status suspends the camera and clears the dim.
    let s = send(
        &mut core,
        json!({"type":"camera_status","status":"permission"}),
    );
    assert_eq!(s["builtin_protection"]["reasons"], json!([]));
}
