use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use telaviva_core::{Core, CoreOptions};

fn send(core: &mut Core, event: Value) -> Value {
    core.submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap();
    core.snapshot_value().unwrap()
}

// Like send, but returns the command error so the test can check that nothing changed.
fn try_send(core: &mut Core, event: Value) -> Result<(), String> {
    core.submit_json(&serde_json::to_vec(&event).unwrap())
}

// A stable monitor with supported brightness, the shape the shell reports for a DDC display.
fn monitor_object(id: &str) -> Value {
    json!({
        "id": id,
        "name": id,
        "identity_status": "stable",
        "connection_path": "usb",
        "brightness": {"supported": true, "value": 50, "raw": 50, "min": 0, "max": 100},
        "volume": {"supported": false, "value": null, "raw": null, "min": 0, "max": 100}
    })
}

// Like monitor_object, at the given brightness level (0..=100).
fn monitor_at(id: &str, level: u64) -> Value {
    let mut monitor = monitor_object(id);
    monitor["brightness"]["value"] = json!(level);
    monitor["brightness"]["raw"] = json!(level);
    monitor
}

fn discover(core: &mut Core, monitor: Value) -> Value {
    send(
        core,
        json!({"type": "monitor_discovered", "monitor": monitor}),
    )
}

fn update(core: &mut Core, settings: Value) -> Value {
    send(
        core,
        json!({"type": "update_settings", "settings": settings}),
    )
}

fn try_update(core: &mut Core, settings: Value) -> Result<(), String> {
    try_send(
        core,
        json!({"type": "update_settings", "settings": settings}),
    )
}

// Turns presence protection on with AC power, as the shells report it.
fn enable_camera(core: &mut Core) -> Value {
    send(core, json!({"type": "power", "on_battery": false}));
    update(core, json!({"presence_protection_enabled": true}))
}

// One bright protected monitor keeps the camera in face mode, so absence can dim it.
// The lock option is set only when asked, so the default case can be tested on its own.
fn camera_core(lock_on: bool) -> Core {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 50));
    enable_camera(&mut core);
    if lock_on {
        update(&mut core, json!({"lock_on_absence": true}));
    }
    core
}

// Presence samples carry the current camera generation, as the shell tags them.
fn presence_sample(core: &mut Core, status: &str, offset_ms: u64) -> Value {
    let generation = core.snapshot_value().unwrap()["camera"]["generation"].clone();
    send(
        core,
        json!({"type": "presence_observation", "status": status, "captured_at_ms": capture_time(offset_ms), "camera_generation": generation}),
    )
}

// An unavailable sample carries the camera status the shell reports with it.
fn unavailable_sample(core: &mut Core, offset_ms: u64) -> Value {
    let generation = core.snapshot_value().unwrap()["camera"]["generation"].clone();
    send(
        core,
        json!({"type": "presence_observation", "status": "unavailable", "camera_status": "unavailable", "captured_at_ms": capture_time(offset_ms), "camera_generation": generation}),
    )
}

// Same clock as the other contract suites: a minute in the past, so offsets stay ordered.
fn capture_time(offset_ms: u64) -> u64 {
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64;
    now_ms - 60_000 + offset_ms
}

// Two absent samples dim the protected display. The stage arms on the second one, at offset 2_000.
fn arm(core: &mut Core) {
    presence_sample(core, "absent", 1_000);
    presence_sample(core, "absent", 2_000);
}

// The shell reports the result of its lock attempt for the given request token.
fn lock_result(core: &mut Core, token: u64, ok: bool) -> Value {
    send(
        core,
        json!({"type": "lock_result", "token": token, "ok": ok}),
    )
}

fn dimmed(snapshot: &Value) -> bool {
    snapshot["monitors"][0]["presence_dimmed"] == true
}

// Settings files are per test, so parallel tests never share a path.
fn temp_root(name: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!("telaviva-lock-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).unwrap();
    root
}

fn persisted_core(path: &Path) -> Core {
    Core::new(CoreOptions {
        config_path: Some(path.to_path_buf()),
        ..CoreOptions::default()
    })
    .unwrap()
}

#[test]
fn lock_defaults_to_off_and_snapshot_shows_idle() {
    let core = Core::new(CoreOptions::default()).unwrap();
    let snapshot = core.snapshot_value().unwrap();
    assert_eq!(
        snapshot["lock"],
        json!({"enabled": false, "stage": "idle", "requested": false, "token": null})
    );
    assert_eq!(snapshot["settings"]["lock_on_absence"], false);
    assert_eq!(snapshot["settings"]["lock_delay_s"].as_f64(), Some(5.0));
    // With the option off, the same absence pattern dims the display but never arms or requests.
    let mut core = camera_core(false);
    arm(&mut core);
    let s = presence_sample(&mut core, "absent", 12_000);
    assert!(dimmed(&s));
    assert_eq!(s["lock"]["stage"], "idle");
    assert_eq!(s["lock"]["requested"], false);
    assert!(s["lock"]["token"].is_null());
}

#[test]
fn lock_stage_arms_on_the_sample_that_dims() {
    let mut core = camera_core(true);
    let first = presence_sample(&mut core, "absent", 1_000);
    assert!(!dimmed(&first));
    assert_eq!(first["lock"]["stage"], "idle");
    let second = presence_sample(&mut core, "absent", 2_000);
    assert!(dimmed(&second));
    assert_eq!(second["lock"]["stage"], "armed");
    assert_eq!(second["lock"]["enabled"], true);
    assert_eq!(second["lock"]["requested"], false);
    assert!(second["lock"]["token"].is_null());
}

#[test]
fn no_lock_request_before_the_delay() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = presence_sample(&mut core, "absent", 6_000);
    // Four seconds after arming is still inside the default five-second delay.
    assert_eq!(s["lock"]["stage"], "armed");
    assert_eq!(s["lock"]["requested"], false);
}

#[test]
fn lock_request_is_issued_at_the_delay_with_a_token() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = presence_sample(&mut core, "absent", 7_000);
    // Five seconds after arming, by captured_at_ms, reaches the default delay.
    assert_eq!(s["lock"]["stage"], "requested");
    assert_eq!(s["lock"]["requested"], true);
    assert_eq!(s["lock"]["token"], 1);
}

#[test]
fn lock_delay_setting_changes_the_wait() {
    let mut core = camera_core(true);
    update(&mut core, json!({"lock_delay_s": 10}));
    arm(&mut core);
    let s = presence_sample(&mut core, "absent", 7_000);
    // Five seconds after arming is inside the new ten-second delay.
    assert_eq!(s["lock"]["stage"], "armed");
    let s = presence_sample(&mut core, "absent", 12_000);
    assert_eq!(s["lock"]["stage"], "requested");
    assert_eq!(s["lock"]["token"], 1);
}

#[test]
fn lock_delay_is_range_checked() {
    let mut core = camera_core(true);
    let before = core.snapshot_value().unwrap();
    for invalid in [0, 61] {
        assert!(try_update(&mut core, json!({"lock_delay_s": invalid})).is_err());
    }
    // Rejected updates store nothing and do not bump the revision.
    let after = core.snapshot_value().unwrap();
    assert_eq!(after["settings"]["lock_delay_s"].as_f64(), Some(5.0));
    assert_eq!(after["revision"], before["revision"]);
    for valid in [1, 60] {
        let s = update(&mut core, json!({"lock_delay_s": valid}));
        assert_eq!(s["settings"]["lock_delay_s"].as_f64(), Some(valid as f64));
    }
}

#[test]
fn stored_lock_delay_outside_the_range_is_clamped_on_load() {
    let root = temp_root("delay-clamp");
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"lock_on_absence":true,"lock_delay_s":500,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let core = persisted_core(&path);
    let s = core.snapshot_value().unwrap();
    assert_eq!(s["settings"]["lock_delay_s"].as_f64(), Some(60.0));
    // The unrelated settings are kept, so the file was loaded rather than replaced by defaults.
    assert_eq!(s["settings"]["lock_on_absence"], true);
    assert_eq!(s["settings"]["presence_protection_enabled"], true);
    assert!(!root.join("settings.json.corrupt").exists());
    drop(core);

    std::fs::write(
        &path,
        br#"{"schema_version":1,"lock_on_absence":true,"lock_delay_s":0,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let core = persisted_core(&path);
    let s = core.snapshot_value().unwrap();
    assert_eq!(s["settings"]["lock_delay_s"].as_f64(), Some(1.0));
    assert_eq!(s["settings"]["lock_on_absence"], true);
    assert!(!root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn present_sample_cancels_the_armed_stage() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = presence_sample(&mut core, "present", 3_000);
    assert_eq!(s["lock"]["stage"], "idle");
    assert!(!dimmed(&s));
}

#[test]
fn insufficient_light_never_requests_a_lock() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = presence_sample(&mut core, "insufficient_light", 3_000);
    assert_eq!(s["lock"]["stage"], "idle");
    // One absent sample at least five seconds after the original arming must not request a lock.
    let s = presence_sample(&mut core, "absent", 8_000);
    let stage = s["lock"]["stage"].as_str().unwrap();
    assert!(stage == "idle" || stage == "armed", "stage was {stage}");
    assert_eq!(s["lock"]["requested"], false);
    assert!(s["lock"]["token"].is_null());
}

#[test]
fn unavailable_camera_cancels_the_stage() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = unavailable_sample(&mut core, 3_000);
    assert_eq!(s["lock"]["stage"], "idle");
    assert_eq!(s["lock"]["requested"], false);
}

#[test]
fn inactive_session_cancels_the_stage() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = send(
        &mut core,
        json!({"type": "keep_awake_session", "active": false}),
    );
    assert_eq!(s["lock"]["stage"], "idle");
    assert_eq!(s["lock"]["requested"], false);
}

#[test]
fn disabling_the_option_or_protection_cancels_the_stage() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = update(&mut core, json!({"lock_on_absence": false}));
    assert_eq!(s["lock"]["enabled"], false);
    assert_eq!(s["lock"]["stage"], "idle");

    let mut core = camera_core(true);
    arm(&mut core);
    let s = update(&mut core, json!({"presence_protection_enabled": false}));
    assert_eq!(s["lock"]["stage"], "idle");
    assert_eq!(s["lock"]["requested"], false);
}

#[test]
fn one_request_per_episode() {
    let mut core = camera_core(true);
    arm(&mut core);
    let s = presence_sample(&mut core, "absent", 7_000);
    assert_eq!(s["lock"]["token"], 1);
    // A further absent sample in the same episode keeps the same request.
    let s = presence_sample(&mut core, "absent", 8_000);
    assert_eq!(s["lock"]["stage"], "requested");
    assert_eq!(s["lock"]["token"], 1);
    let s = lock_result(&mut core, 1, true);
    assert_eq!(s["lock"]["stage"], "locked");
    assert_eq!(s["lock"]["requested"], false);
    // Later absent samples in the same episode never issue a second request.
    let s = presence_sample(&mut core, "absent", 9_000);
    assert_eq!(s["lock"]["stage"], "locked");
    assert_eq!(s["lock"]["requested"], false);
    assert_eq!(s["lock"]["token"], 1);
}

#[test]
fn failed_lock_keeps_the_dim_and_does_not_retry() {
    let mut core = camera_core(true);
    arm(&mut core);
    presence_sample(&mut core, "absent", 7_000);
    let s = lock_result(&mut core, 1, false);
    assert_eq!(s["lock"]["stage"], "failed");
    assert_eq!(s["lock"]["requested"], false);
    assert!(dimmed(&s));
    // A further absent sample keeps the failure and does not retry.
    let s = presence_sample(&mut core, "absent", 8_000);
    assert_eq!(s["lock"]["stage"], "failed");
    assert_eq!(s["lock"]["requested"], false);
    // A present sample ends the episode and restores the display.
    let s = presence_sample(&mut core, "present", 9_000);
    assert_eq!(s["lock"]["stage"], "idle");
    assert!(!dimmed(&s));
    // A new absence episode arms on its second absent sample, then the delay issues token 2.
    presence_sample(&mut core, "absent", 10_000);
    let s = presence_sample(&mut core, "absent", 11_000);
    assert_eq!(s["lock"]["stage"], "armed");
    let s = presence_sample(&mut core, "absent", 16_000);
    assert_eq!(s["lock"]["stage"], "requested");
    assert_eq!(s["lock"]["requested"], true);
    assert_eq!(s["lock"]["token"], 2);
}

#[test]
fn lock_result_with_a_wrong_token_is_ignored() {
    let mut core = camera_core(true);
    arm(&mut core);
    presence_sample(&mut core, "absent", 7_000);
    let s = lock_result(&mut core, 99, true);
    assert_eq!(s["lock"]["stage"], "requested");
    assert_eq!(s["lock"]["requested"], true);
    assert_eq!(s["lock"]["token"], 1);
    let s = lock_result(&mut core, 99, false);
    assert_eq!(s["lock"]["stage"], "requested");
    assert_eq!(s["lock"]["token"], 1);
}

#[test]
fn lock_result_outside_a_request_is_ignored() {
    let mut core = camera_core(true);
    assert!(try_send(
        &mut core,
        json!({"type": "lock_result", "token": 1, "ok": true})
    )
    .is_ok());
    let s = core.snapshot_value().unwrap();
    assert_eq!(s["lock"]["stage"], "idle");
    assert_eq!(s["lock"]["requested"], false);
    assert!(s["lock"]["token"].is_null());
}

#[test]
fn locked_session_suspends_the_camera_and_unlock_resumes_it() {
    let mut core = camera_core(true);
    arm(&mut core);
    presence_sample(&mut core, "absent", 7_000);
    lock_result(&mut core, 1, true);
    let before = core.snapshot_value().unwrap()["camera"]["generation"]
        .as_u64()
        .unwrap();
    // The session is locked, so the camera stops sampling.
    let s = send(
        &mut core,
        json!({"type": "keep_awake_session", "active": false}),
    );
    assert_eq!(s["camera"]["mode"], "suspended");
    // Unlock returns the stage to idle and resumes the camera with a new generation.
    let s = send(
        &mut core,
        json!({"type": "keep_awake_session", "active": true}),
    );
    assert_eq!(s["lock"]["stage"], "idle");
    assert_eq!(s["camera"]["mode"], "face");
    assert!(s["camera"]["generation"].as_u64().unwrap() > before);
}
