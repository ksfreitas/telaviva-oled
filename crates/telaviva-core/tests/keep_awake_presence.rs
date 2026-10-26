use serde_json::{json, Value};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use telaviva_core::{Core, CoreOptions, Settings};

fn send(core: &mut Core, event: Value) -> Value {
    core.submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap();
    core.snapshot_value().unwrap()
}

fn captured_at() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64
}

fn enable(core: &mut Core) -> Value {
    send(
        core,
        json!({"type":"update_settings","settings":{"keep_awake_with_presence":true,"always_on_camera":false}}),
    );
    send(core, json!({"type":"keep_awake_session","active":true}))
}

fn observe(core: &mut Core, status: &str) -> Value {
    std::thread::sleep(Duration::from_millis(2));
    let generation = core.snapshot_value().unwrap()["camera"]["generation"]
        .as_u64()
        .unwrap();
    send(
        core,
        json!({"type":"presence_observation","status":status,"captured_at_ms":captured_at(),"camera_generation":generation}),
    )
}

fn add_monitor(core: &mut Core, raw: f64) -> Value {
    send(
        core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Screen","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":raw,"raw":raw,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    )
}

#[test]
fn setting_defaults_off_and_migrates_from_older_json() {
    assert!(!Settings::default().keep_awake_with_presence);
    let old =
        Settings::from_json(br#"{"schema_version":1,"presence_protection_enabled":true}"#).unwrap();
    assert!(old.presence_protection_enabled);
    assert!(!old.keep_awake_with_presence);
}

#[test]
fn keep_awake_enables_continuous_monitoring_without_displays_or_on_battery() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    enable(&mut core);
    let snapshot = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(snapshot["camera"]["mode"], "face");
    assert_eq!(snapshot["camera"]["capture_mode"], "continuous_monitoring");
    assert!(snapshot["settings"]["suspend_presence_on_battery"]
        .as_bool()
        .unwrap());
    assert!(!snapshot["settings"]["always_on_camera"].as_bool().unwrap());
}

#[test]
fn brightness_threshold_does_not_suspend_keep_awake_but_inactive_session_is_not_mislabeled_low_brightness(
) {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    add_monitor(&mut core, 40.0);
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{
            "presence_min_brightness_by_display":{"screen":40.0},
            "keep_awake_with_presence":true,
            "presence_protection_enabled":true
        }}),
    );

    let inactive = core.snapshot_value().unwrap();
    assert_eq!(inactive["camera"]["mode"], "suspended");
    assert_eq!(inactive["camera"]["display_status"], "suspended");

    let active = send(
        &mut core,
        json!({"type":"keep_awake_session","active":true}),
    );
    assert_eq!(active["camera"]["mode"], "face");
    assert_eq!(active["camera"]["display_status"], "checking");
    assert_eq!(active["camera"]["capture_mode"], "continuous_monitoring");
}

#[test]
fn monitoring_bypasses_power_threshold_and_protection_gates_without_changing_dimming_policy() {
    for (raw, on_battery, protection_disabled) in [
        (70.0, Some(true), false),
        (70.0, None, false),
        (10.0, Some(false), false),
        (70.0, Some(false), true),
    ] {
        let mut core = Core::new(CoreOptions::default()).unwrap();
        add_monitor(&mut core, raw);
        send(
            &mut core,
            json!({"type":"update_settings","settings":{
                "keep_awake_with_presence":true,
                "presence_protection_enabled":true,
                "suspend_presence_on_battery":true,
                "protection_disabled": if protection_disabled { json!(["screen"]) } else { json!([]) }
            }}),
        );
        if let Some(value) = on_battery {
            send(&mut core, json!({"type":"power","on_battery":value}));
        }
        let mut snapshot = send(
            &mut core,
            json!({"type":"keep_awake_session","active":true}),
        );
        assert_eq!(snapshot["camera"]["mode"], "face");
        assert_eq!(
            observe(&mut core, "present")["keep_awake"]["requested"],
            true
        );
        snapshot = observe(&mut core, "absent");
        assert_eq!(snapshot["keep_awake"]["requested"], false);
        assert_eq!(snapshot["monitors"][0]["presence_dimmed"], false);
        snapshot = observe(&mut core, "absent");
        assert_eq!(snapshot["monitors"][0]["presence_dimmed"], false);
    }
}

#[test]
fn first_absence_releases_keep_awake_but_keeps_brightness_two_miss_policy() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    add_monitor(&mut core, 70.0);
    send(
        &mut core,
        json!({"type":"update_settings","settings":{
            "keep_awake_with_presence":true,
            "presence_protection_enabled":true,
            "suspend_presence_on_battery":false
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"keep_awake_session","active":true}),
    );
    assert_eq!(
        observe(&mut core, "present")["keep_awake"]["requested"],
        true
    );
    let first_absence = observe(&mut core, "absent");
    assert_eq!(first_absence["keep_awake"]["requested"], false);
    assert_eq!(first_absence["monitors"][0]["presence_dimmed"], false);
    let second_absence = observe(&mut core, "absent");
    assert_eq!(second_absence["monitors"][0]["presence_dimmed"], true);
}

#[test]
fn session_events_leave_camera_state_unchanged_while_keep_awake_is_disabled() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    add_monitor(&mut core, 70.0);
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    let active = send(
        &mut core,
        json!({"type":"keep_awake_session","active":true}),
    );
    assert_eq!(active["camera"]["mode"], "face");
    let present = observe(&mut core, "present");
    let generation = present["camera"]["generation"].clone();
    assert_eq!(present["camera"]["presence"], "present");

    let inactive = send(
        &mut core,
        json!({"type":"keep_awake_session","active":false}),
    );
    assert_eq!(inactive["camera"]["generation"], generation);
    assert_eq!(inactive["camera"]["mode"], "face");
    assert_eq!(inactive["camera"]["presence"], "present");
}

#[test]
fn fresh_presence_requests_inhibition_without_snapshot_reads_renewing_the_lease() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    enable(&mut core);
    let snapshot = observe(&mut core, "present");
    assert_eq!(snapshot["keep_awake"]["enabled"], true);
    assert_eq!(snapshot["keep_awake"]["requested"], true);
    assert!(snapshot["keep_awake"]["observation_id"].as_u64().unwrap() > 0);
    assert_eq!(
        snapshot["keep_awake"]["generation"],
        snapshot["camera"]["generation"]
    );
    let remaining = snapshot["keep_awake"]["valid_for_ms"].as_u64().unwrap();
    assert!((1..=25_000).contains(&remaining));

    let later = core.snapshot_value().unwrap();
    assert_eq!(later["keep_awake"]["requested"], true);
    assert!(later["keep_awake"]["valid_for_ms"].as_u64().unwrap() <= remaining);
}

#[test]
fn absence_and_unassessable_observations_release_immediately() {
    for status in ["absent", "insufficient_light", "unavailable"] {
        let mut core = Core::new(CoreOptions::default()).unwrap();
        enable(&mut core);
        assert_eq!(
            observe(&mut core, "present")["keep_awake"]["requested"],
            true
        );
        let snapshot = observe(&mut core, status);
        assert_eq!(
            snapshot["keep_awake"]["requested"], false,
            "status={status}"
        );
        assert_eq!(snapshot["keep_awake"]["valid_for_ms"], 0);
    }
}

#[test]
fn session_changes_and_settings_generation_changes_require_fresh_presence() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    enable(&mut core);
    let present = observe(&mut core, "present");
    assert_eq!(present["keep_awake"]["requested"], true);

    let inactive = send(
        &mut core,
        json!({"type":"keep_awake_session","active":false}),
    );
    assert_eq!(inactive["keep_awake"]["requested"], false);
    assert_eq!(inactive["keep_awake"]["observation_id"], 0);
    assert_eq!(inactive["camera"]["mode"], "suspended");

    let active = send(
        &mut core,
        json!({"type":"keep_awake_session","active":true}),
    );
    assert_eq!(active["keep_awake"]["requested"], false);
    assert!(
        active["camera"]["generation"].as_u64().unwrap()
            > present["camera"]["generation"].as_u64().unwrap()
    );

    let renewed = observe(&mut core, "present");
    let old_generation = renewed["keep_awake"]["generation"].as_u64().unwrap();
    let changed = send(
        &mut core,
        json!({"type":"update_settings","settings":{"locale":"pt-BR"}}),
    );
    assert_eq!(changed["keep_awake"]["requested"], false);
    assert_eq!(changed["keep_awake"]["observation_id"], 0);
    assert!(changed["camera"]["generation"].as_u64().unwrap() > old_generation);
    let stale = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":captured_at(),"camera_generation":old_generation}),
    );
    assert_eq!(stale["keep_awake"]["requested"], false);
    assert_eq!(stale["keep_awake"]["observation_id"], 0);
}

#[test]
fn snapshots_do_not_create_an_observation_and_session_event_is_strict() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let snapshot = enable(&mut core);
    assert_eq!(snapshot["keep_awake"]["requested"], false);
    assert_eq!(snapshot["keep_awake"]["observation_id"], 0);
    let generation = snapshot["camera"]["generation"].as_u64().unwrap();
    let unchanged = send(
        &mut core,
        json!({"type":"keep_awake_session","active":true}),
    );
    assert_eq!(unchanged["camera"]["generation"], generation);
    assert!(core
        .submit_json(br#"{"type":"keep_awake_session","active":true,"extra":1}"#)
        .is_err());
    assert!(core
        .submit_json(br#"{"type":"keep_awake_session","active":"yes"}"#)
        .is_err());
}
