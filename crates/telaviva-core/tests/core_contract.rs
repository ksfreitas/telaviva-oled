use serde_json::{json, Value};
use telaviva_core::{Core, CoreOptions, Settings};

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

#[test]
fn settings_defaults_validate_and_reject_future_schema_and_bad_ranges() {
    let defaults = Settings::default();
    assert_eq!(defaults.presence_sampling_interval_s, 10.0);
    assert_eq!(defaults.return_sampling_interval_s, 1.0);
    assert!(defaults.suspend_presence_on_battery);
    assert!(
        Settings::from_json(&serde_json::to_vec(&json!({"schema_version":2})).unwrap()).is_err()
    );
    assert!(Settings::from_json(
        &serde_json::to_vec(&json!({"schema_version":1,"presence_sampling_interval_s":0})).unwrap()
    )
    .is_err());
    assert!(Settings::from_json(
        &serde_json::to_vec(&json!({"schema_version":1,"presence_sampling_interval_s":4.0}))
            .unwrap()
    )
    .is_err());
    assert!(Settings::from_json(
        &serde_json::to_vec(&json!({"schema_version":1,"presence_sampling_interval_s":301.0}))
            .unwrap()
    )
    .is_err());
    assert!(Settings::from_json(
        &serde_json::to_vec(&json!({"schema_version":1,"return_sampling_interval_s":5.25}))
            .unwrap()
    )
    .is_err());
    assert!(
        Settings::from_json(br#"{"schema_version":1,"presence_sampling_interval_s":NaN}"#).is_err()
    );
    assert!(
        Settings::from_json(br#"{"schema_version":1,"locale":"en-US","locale":"pt-BR"}"#).is_err()
    );
    assert!(Settings::from_json(br#"{"schema_version":1,"selected_camera_id":""}"#).is_err());
}

#[test]
fn old_valid_interval_settings_are_clamped_without_losing_other_preferences() {
    let root = std::env::temp_dir().join(format!("telaviva-intervals-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        serde_json::to_vec(&json!({
            "schema_version": 1,
            "locale": "pt-BR",
            "presence_sampling_interval_s": 3600.0,
            "return_sampling_interval_s": 60.0,
            "suspend_presence_on_battery": false,
            "presence_protection_enabled": true,
            "screensaver_protection_enabled": true,
            "launch_at_login": true,
            "selected_camera_id": "camera-1",
            "screensaver_targets": {"display-1": 27.0}
        }))
        .unwrap(),
    )
    .unwrap();

    let core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    let settings = core.settings().unwrap();
    assert_eq!(settings.presence_sampling_interval_s, 300.0);
    assert_eq!(settings.return_sampling_interval_s, 5.0);
    assert_eq!(settings.locale, "pt-BR");
    assert!(!settings.suspend_presence_on_battery);
    assert!(settings.presence_protection_enabled);
    assert!(settings.screensaver_protection_enabled);
    assert!(settings.launch_at_login);
    assert_eq!(settings.selected_camera_id.as_deref(), Some("camera-1"));
    assert_eq!(settings.screensaver_targets.get("display-1"), Some(&27.0));
    assert!(path.exists());
    assert!(!root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn command_rejects_duplicate_and_unknown_fields_without_changing_revision() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let revision = core.snapshot_value().unwrap()["revision"].as_u64().unwrap();
    assert!(core
        .submit_json(br#"{"type":"power","type":"screensaver","on_battery":false}"#)
        .is_err());
    assert!(core
        .submit_json(br#"{"type":"power","on_battery":false,"extra":true}"#)
        .is_err());
    assert_eq!(
        core.snapshot_value().unwrap()["revision"].as_u64(),
        Some(revision)
    );
}

#[test]
fn settings_updates_enforce_new_interval_bounds() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for settings in [
        json!({"presence_sampling_interval_s": 4.0}),
        json!({"presence_sampling_interval_s": 301.0}),
        json!({"return_sampling_interval_s": 0.0}),
        json!({"return_sampling_interval_s": 5.25}),
    ] {
        assert!(core
            .submit_json(
                &serde_json::to_vec(&json!({"type":"update_settings","settings":settings}))
                    .unwrap()
            )
            .is_err());
    }
    assert_eq!(core.settings().unwrap(), Settings::default());
}

#[test]
fn actions_serialize_per_monitor_and_ignore_wrong_generation_acknowledgements() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
            "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
        }}),
    );
    let first = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":20}),
    )["pending_actions"][0]
        .clone();
    send(
        &mut core,
        json!({"type":"set_volume","monitor_id":"screen","value":80}),
    );
    let snapshot = core.snapshot_value().unwrap();
    assert_eq!(snapshot["pending_actions"].as_array().unwrap().len(), 1);
    assert_eq!(snapshot["pending_actions"][0]["kind"], "set_brightness");
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":first["id"],"generation":99999,"ok":true,"value":20}),
    );
    assert_eq!(
        core.snapshot_value().unwrap()["pending_actions"][0]["id"],
        first["id"]
    );
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":first["id"],"generation":first["generation"],"ok":true,"value":20}),
    );
    let next = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    assert_eq!(next["kind"], "set_volume");
}

#[test]
fn locale_catalogs_have_matching_keys_and_auto_locale_uses_native_locale() {
    fn keys(src: &str) -> std::collections::BTreeSet<String> {
        src.lines()
            .filter_map(|line| line.split_once('=').map(|(key, _)| key.trim().to_owned()))
            .collect()
    }
    let en = include_str!("../../../locales/en-US.ftl");
    let pt = include_str!("../../../locales/pt-BR.ftl");
    assert_eq!(keys(en), keys(pt));
    let core = Core::new(CoreOptions {
        os_locale: Some("pt_BR".into()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(
        core.snapshot_value().unwrap()["messages"]["status-ready"],
        "Pronto"
    );
}

#[test]
fn controls_are_queued_and_stale_results_do_not_replace_newer_state() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let snapshot = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"edid-a@usb-1","name":"Display","identity_status":"stable","connection_path":"usb-1",
            "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
            "volume":{"supported":true,"value":30,"raw":30,"min":0,"max":100}
        }}),
    );
    assert_eq!(snapshot["monitors"][0]["id"], "edid-a@usb-1");
    let before = snapshot["revision"].as_u64().unwrap();
    let snapshot = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"edid-a@usb-1","value":22}),
    );
    assert!(snapshot["revision"].as_u64().unwrap() > before);
    let action = snapshot["pending_actions"]
        .as_array()
        .unwrap()
        .last()
        .unwrap()
        .clone();
    assert_eq!(action["kind"], "set_brightness");
    let snapshot = send(
        &mut core,
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":22}),
    );
    assert_eq!(
        snapshot["monitors"][0]["brightness"]["observed"].as_f64(),
        Some(22.0)
    );
    let snapshot = send(
        &mut core,
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":80}),
    );
    assert_eq!(
        snapshot["monitors"][0]["brightness"]["observed"].as_f64(),
        Some(22.0)
    );
}

#[test]
fn ambiguous_monitors_never_receive_control_actions() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let _snapshot = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"ambiguous","name":"Display","identity_status":"ambiguous","connection_path":"",
            "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
            "volume":{"supported":true,"value":30,"raw":30,"min":0,"max":100}
        }}),
    );
    let snapshot = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"ambiguous","value":10}),
    );
    assert_eq!(snapshot["pending_actions"].as_array().unwrap().len(), 0);
}

#[test]
fn protection_requires_two_fresh_misses_and_suspends_camera_on_battery() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"edid-a@usb-1","name":"Display","identity_status":"stable","connection_path":"usb-1",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let snapshot = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(snapshot["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(10_000),"camera_generation":snapshot["camera"]["generation"]}),
    );
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    let s = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":s["camera"]["generation"]}),
    );
    assert!(s["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .any(|a| a["kind"] == "set_brightness" && a["value"].as_f64() == Some(0.0)));
    assert_eq!(s["camera"]["capture_mode"], "continuous_return");
    assert_eq!(s["monitors"][0]["presence_dimmed"], true);
    let s = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["capture_mode"], "stopped");
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
}

#[test]
fn camera_display_status_tracks_capture_success_and_policy_states() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let disabled = core.snapshot_value().unwrap();
    assert_eq!(disabled["camera"]["display_status"], "disabled");

    let checking = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(checking["camera"]["display_status"], "checking");
    let ready = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(10_000),"camera_generation":checking["camera"]["generation"]}),
    );
    assert_eq!(ready["camera"]["status"], "ready");
    assert_eq!(ready["camera"]["display_status"], "ready");

    let low_light = send(
        &mut core,
        json!({"type":"presence_observation","status":"insufficient_light","captured_at_ms":capture_time(20_000),"camera_generation":ready["camera"]["generation"]}),
    );
    assert_eq!(low_light["camera"]["display_status"], "insufficient_light");
    let suspended = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(suspended["camera"]["mode"], "suspended");
    assert_eq!(suspended["camera"]["display_status"], "suspended_battery");
    let power_unknown = send(&mut core, json!({"type":"power","on_battery":null}));
    assert_eq!(
        power_unknown["camera"]["display_status"],
        "suspended_power_unknown"
    );

    let resumed = send(&mut core, json!({"type":"power","on_battery":false}));
    assert_eq!(resumed["camera"]["display_status"], "insufficient_light");
    let disabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":false}}),
    );
    assert_eq!(disabled["camera"]["mode"], "disabled");
    assert_eq!(disabled["camera"]["display_status"], "disabled");

    let mut no_monitor = Core::new(CoreOptions::default()).unwrap();
    send(&mut no_monitor, json!({"type":"power","on_battery":false}));
    let no_monitor = send(
        &mut no_monitor,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(no_monitor["camera"]["mode"], "suspended");
    assert_eq!(
        no_monitor["camera"]["display_status"],
        "suspended_no_monitor"
    );
}

#[test]
fn stale_presence_success_does_not_clear_a_camera_failure() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let enabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    let failed = send(
        &mut core,
        json!({"type":"camera_status","status":"permission","generation":enabled["camera"]["generation"]}),
    );
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_sampling_interval_s":11}}),
    );
    send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    let after_stale = core.snapshot_value().unwrap();
    assert_eq!(after_stale["camera"]["status"], "permission");
    assert_eq!(after_stale["camera"]["display_status"], "permission");
    assert_eq!(failed["camera"]["display_status"], "permission");
}

#[test]
fn stale_camera_generation_is_ignored_across_battery_suspend_and_ac_resume() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":80,"raw":80,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let ac = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    let battery = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(battery["camera"]["mode"], "suspended");
    send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(10_000),"camera_generation":ac["camera"]["generation"]}),
    );
    let resumed = send(&mut core, json!({"type":"power","on_battery":false}));
    assert!(
        resumed["camera"]["generation"].as_u64().unwrap()
            > battery["camera"]["generation"].as_u64().unwrap()
    );
    send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":battery["camera"]["generation"]}),
    );
    send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(30_000),"camera_generation":resumed["camera"]["generation"]}),
    );
    let still_not_dimmed = core.snapshot_value().unwrap();
    assert!(still_not_dimmed["pending_actions"]
        .as_array()
        .unwrap()
        .is_empty());
    send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(40_000),"camera_generation":resumed["camera"]["generation"]}),
    );
    assert!(core.snapshot_value().unwrap()["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .any(|a| a["kind"] == "set_brightness" && a["value"] == 0.0));
}

#[test]
fn corrupt_settings_are_backed_up_before_defaults_load() {
    let root = std::env::temp_dir().join(format!("telaviva-test-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(&path, b"not-json").unwrap();
    let core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(core.settings().unwrap(), Settings::default());
    assert!(root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn future_schema_is_rejected_without_moving_or_replacing_it() {
    let root = std::env::temp_dir().join(format!("telaviva-future-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(&path, br#"{"schema_version":2}"#).unwrap();
    assert!(Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .is_err());
    assert!(path.exists());
    assert!(!root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn returning_sample_restores_owned_brightness_and_cadence_is_one_second() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(s["camera"]["capture_mode"], "single_frame");
    for offset in [10_000, 20_000] {
        let ts = capture_time(offset);
        let s2 = send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":ts,"camera_generation":s["camera"]["generation"]}),
        );
        if offset == 20_000 {
            assert_eq!(s2["camera"]["capture_mode"], "continuous_return");
            assert_eq!(s2["monitors"][0]["presence_dimmed"], true);
            assert_eq!(s2["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
        }
    }
    let action = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":0}),
    );
    let s = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(30_000),"camera_generation":s["camera"]["generation"]}),
    );
    assert_eq!(s["camera"]["capture_mode"], "single_frame");
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
    let read = s["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("restoration must verify ownership before writing");
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":0}),
    );
    let after_read = core.snapshot_value().unwrap();
    let restore = after_read["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .expect("owned brightness should restore after readback");
    assert_eq!(restore["value"].as_f64(), Some(70.0));
}

#[test]
fn external_brightness_change_before_restore_is_preserved() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    for offset in [10_000, 20_000] {
        send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":s["camera"]["generation"]}),
        );
    }
    let dim = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    let returned = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(30_000),"camera_generation":s["camera"]["generation"]}),
    );
    let read = returned["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .unwrap()
        .clone();
    let changed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":35}),
    );
    assert!(changed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    assert_eq!(
        changed["monitors"][0]["brightness"]["observed"].as_f64(),
        Some(35.0)
    );
    assert_eq!(changed["monitors"][0]["brightness"]["owned"], false);
}

#[test]
fn disabling_protection_during_an_unacknowledged_dim_does_not_restore_blindly() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let enabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    for offset in [10_000, 20_000] {
        send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":enabled["camera"]["generation"]}),
        );
    }
    let dim = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    assert_eq!(dim["value"].as_f64(), Some(0.0));
    let disabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":false}}),
    );
    assert_eq!(disabled["settings"]["presence_protection_enabled"], false);
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    let followup = core.snapshot_value().unwrap();
    let read = followup["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .unwrap();
    assert_eq!(read["target"].as_f64(), Some(70.0));
}

#[test]
fn shutdown_checks_an_owned_value_and_does_not_restore_pending_manual_write() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let before_shutdown = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(before_shutdown["camera"]["mode"], "face");
    let manual = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":25}),
    )["pending_actions"][0]
        .clone();
    let shutdown = send(&mut core, json!({"type":"shutdown"}));
    assert_eq!(shutdown["camera"]["mode"], "disabled");
    assert_eq!(shutdown["camera"]["capture_mode"], "stopped");
    assert_eq!(shutdown["camera"]["next_sample_in_ms"], Value::Null);
    assert!(shutdown["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness" || a["id"] == manual["id"]));
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":manual["id"],"generation":manual["generation"],"ok":true,"value":25}),
    );
    let after = core.snapshot_value().unwrap();
    assert!(after["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "read_brightness"));
    assert!(after["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["value"].as_f64() != Some(70.0)));
}

#[test]
fn camera_failure_clears_only_presence_protection_and_checks_restoration() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let enabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"screensaver_protection_enabled":true,"screensaver_targets":{"screen":50.0}}}),
    );
    send(&mut core, json!({"type":"screensaver","state":"active"}));
    for offset in [10_000, 20_000] {
        send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":enabled["camera"]["generation"]}),
        );
    }
    let dim = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":50}),
    );
    let presence_dim = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":presence_dim["id"],"generation":presence_dim["generation"],"ok":true,"value":0}),
    );
    let failed = send(
        &mut core,
        json!({"type":"camera_status","status":"permission","generation":enabled["camera"]["generation"]}),
    );
    assert_eq!(failed["camera"]["mode"], "suspended");
    assert_eq!(
        failed["monitors"][0]["brightness"]["reasons"][0],
        "screensaver"
    );
    let check = failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .unwrap_or_else(|| panic!("expected ownership read, got {failed:#}"));
    let current_check = check.clone();
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":current_check["id"],"generation":current_check["generation"],"ok":true,"value":0}),
    );
    let next_check = core.snapshot_value().unwrap();
    let check_for_new_target = next_check["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .unwrap();
    assert_eq!(check_for_new_target["target"].as_f64(), Some(50.0));
}

#[test]
fn insufficient_light_removes_presence_reason_and_requires_two_usable_samples() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    for offset in [10_000, 20_000] {
        let ts = capture_time(offset);
        send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":ts,"camera_generation":s["camera"]["generation"]}),
        );
    }
    let action = core.snapshot_value().unwrap()["pending_actions"][0].clone();
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":0}),
    );
    let low = send(
        &mut core,
        json!({"type":"presence_observation","status":"insufficient_light","captured_at_ms":capture_time(30_000),"camera_generation":s["camera"]["generation"]}),
    );
    assert_eq!(low["camera"]["mode"], "light_only");
    let restore_check = low["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("low light restoration checks ownership first");
    assert_eq!(restore_check["target"].as_f64(), Some(70.0));
    let first = send(
        &mut core,
        json!({"type":"presence_observation","status":"usable_light","captured_at_ms":capture_time(40_000),"camera_generation":s["camera"]["generation"]}),
    );
    assert_eq!(first["camera"]["mode"], "light_only");
    let second = send(
        &mut core,
        json!({"type":"presence_observation","status":"usable_light","captured_at_ms":capture_time(50_000),"camera_generation":s["camera"]["generation"]}),
    );
    assert_eq!(second["camera"]["mode"], "face");
}
