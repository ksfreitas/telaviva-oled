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
    assert!(defaults.always_on_camera);
    assert!(!defaults.hardware_keys_enabled);
    assert_eq!(defaults.brightness_key_step, 5);
    assert_eq!(defaults.volume_key_step, 5);
    assert!(defaults.validate().is_ok());
    let mut invalid_steps = Settings::default();
    for step in [0, 16] {
        invalid_steps.brightness_key_step = step;
        assert_eq!(
            invalid_steps.validate().unwrap_err(),
            "brightness key step out of range"
        );
        invalid_steps.brightness_key_step = 5;
        invalid_steps.volume_key_step = step;
        assert_eq!(
            invalid_steps.validate().unwrap_err(),
            "volume key step out of range"
        );
        invalid_steps.volume_key_step = 5;
    }
    let old_schema = Settings::from_json(
        br#"{"schema_version":1,"locale":"pt-BR","presence_protection_enabled":true}"#,
    )
    .unwrap();
    assert!(old_schema.presence_protection_enabled);
    assert!(old_schema.always_on_camera);
    assert!(!old_schema.hardware_keys_enabled);
    assert!(
        Settings::from_json(br#"{"schema_version":1,"always_on_camera":true}"#)
            .unwrap()
            .always_on_camera
    );
    assert!(
        !Settings::from_json(br#"{"schema_version":1,"always_on_camera":false}"#)
            .unwrap()
            .always_on_camera
    );
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
    assert!(
        Settings::from_json(br#"{"schema_version":1,"hardware_keys_enabled":"enabled"}"#).is_err()
    );
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
    assert!(settings.always_on_camera);
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
    assert!(core
        .submit_json(br#"{"type":"monitor_display_state","monitor_id":"screen","generation":1,"state":"offline"}"#)
        .is_err());
    assert_eq!(
        core.snapshot_value().unwrap()["revision"].as_u64(),
        Some(revision)
    );
}

#[test]
fn revision_matches_snapshot_revision() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let snapshot_revision = |core: &Core| core.snapshot_value().unwrap()["revision"].as_u64();
    assert_eq!(Some(core.revision()), snapshot_revision(&core));
    let fresh = core.revision();
    core.submit_json(br#"{"type":"power","on_battery":false}"#)
        .unwrap();
    assert!(core.revision() > fresh);
    assert_eq!(Some(core.revision()), snapshot_revision(&core));
    let accepted = core.revision();
    assert!(core
        .submit_json(br#"{"type":"power","on_battery":false,"extra":true}"#)
        .is_err());
    assert_eq!(core.revision(), accepted);
    assert_eq!(Some(core.revision()), snapshot_revision(&core));
}

#[test]
fn consecutive_snapshots_without_a_change_are_identical() {
    let core = Core::new(CoreOptions::default()).unwrap();
    let first = core.snapshot_value().unwrap();
    let second = core.snapshot_value().unwrap();
    assert_eq!(first, second);
    assert_eq!(first["revision"], second["revision"]);
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
fn builtin_auto_brightness_setting_defaults_to_false_and_old_files_load_false() {
    assert!(!Settings::default().builtin_auto_brightness_off_on_protection);
    let old =
        Settings::from_json(br#"{"schema_version":1,"presence_protection_enabled":true}"#).unwrap();
    assert!(!old.builtin_auto_brightness_off_on_protection);
    let explicit = Settings::from_json(
        br#"{"schema_version":1,"builtin_auto_brightness_off_on_protection":true}"#,
    )
    .unwrap();
    assert!(explicit.builtin_auto_brightness_off_on_protection);
}

#[test]
fn builtin_auto_brightness_setting_is_persisted_reloaded_and_type_checked() {
    let root = std::env::temp_dir().join(format!("telaviva-builtin-auto-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert!(
        !core
            .settings()
            .unwrap()
            .builtin_auto_brightness_off_on_protection
    );
    assert!(core
        .submit_json(br#"{"type":"update_settings","settings":{"builtin_auto_brightness_off_on_protection":true}}"#)
        .is_ok());
    assert!(
        core.settings()
            .unwrap()
            .builtin_auto_brightness_off_on_protection
    );
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["schema_version"], 1);
    assert_eq!(persisted["builtin_auto_brightness_off_on_protection"], true);
    let reloaded = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert!(
        reloaded
            .settings()
            .unwrap()
            .builtin_auto_brightness_off_on_protection
    );
    let yes_event = br#"{"type":"update_settings","settings":{"builtin_auto_brightness_off_on_protection":"yes"}}"#;
    assert_eq!(
        core.submit_json(yes_event).unwrap_err(),
        "setting must be boolean"
    );
    assert!(
        core.settings()
            .unwrap()
            .builtin_auto_brightness_off_on_protection
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn hardware_key_setting_is_optional_default_false_persisted_and_type_checked() {
    let root = std::env::temp_dir().join(format!("telaviva-hardware-keys-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert!(!core.settings().unwrap().hardware_keys_enabled);
    assert!(core
        .submit_json(br#"{"type":"update_settings","settings":{"hardware_keys_enabled":true}}"#)
        .is_ok());
    assert!(core.settings().unwrap().hardware_keys_enabled);
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["schema_version"], 1);
    assert_eq!(persisted["hardware_keys_enabled"], true);
    assert!(core
        .submit_json(br#"{"type":"update_settings","settings":{"hardware_keys_enabled":"yes"}}"#)
        .is_err());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn key_step_settings_are_optional_default_five_persisted_and_range_checked() {
    let root = std::env::temp_dir().join(format!("telaviva-key-steps-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(core.settings().unwrap().brightness_key_step, 5);
    assert_eq!(core.settings().unwrap().volume_key_step, 5);
    for key in ["brightness_key_step", "volume_key_step"] {
        for step in [1, 15] {
            let event = json!({"type":"update_settings","settings":{key:step}});
            assert!(core
                .submit_json(&serde_json::to_vec(&event).unwrap())
                .is_ok());
            assert_eq!(core.snapshot_value().unwrap()["settings"][key], step);
            let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
            assert_eq!(persisted[key], step);
            let reloaded = Core::new(CoreOptions {
                config_path: Some(path.clone()),
                ..CoreOptions::default()
            })
            .unwrap();
            assert_eq!(reloaded.settings().unwrap(), core.settings().unwrap());
        }
    }
    let before = core.settings().unwrap();
    let before_file = std::fs::read(&path).unwrap();
    for key in ["brightness_key_step", "volume_key_step"] {
        for invalid in [
            json!(0),
            json!(16),
            json!(5.5),
            json!("5"),
            json!(true),
            json!(null),
            json!(-1),
        ] {
            let event = json!({"type":"update_settings","settings":{key:invalid}});
            assert!(core
                .submit_json(&serde_json::to_vec(&event).unwrap())
                .is_err());
            assert_eq!(core.settings().unwrap(), before);
        }
    }
    assert_eq!(std::fs::read(&path).unwrap(), before_file);
    assert!(core
        .submit_json(br#"{"type":"update_settings","settings":{"brightness_key_step":3}}"#)
        .is_ok());
    let after_brightness = core.settings().unwrap();
    assert_eq!(after_brightness.brightness_key_step, 3);
    assert_eq!(after_brightness.volume_key_step, before.volume_key_step);
    assert!(core
        .submit_json(br#"{"type":"update_settings","settings":{"volume_key_step":12}}"#)
        .is_ok());
    let after_volume = core.settings().unwrap();
    assert_eq!(after_volume.brightness_key_step, 3);
    assert_eq!(after_volume.volume_key_step, 12);
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn inference_device_defaults_to_auto_and_older_files_without_the_key_load() {
    assert_eq!(Settings::default().inference_device, "auto");
    let old = Settings::from_json(
        br#"{"schema_version":1,"locale":"pt-BR","presence_protection_enabled":true}"#,
    )
    .unwrap();
    assert_eq!(old.inference_device, "auto");
}

#[test]
fn inference_device_accepts_documented_values_and_rejects_others() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    assert_eq!(core.settings().unwrap().inference_device, "auto");
    for value in [
        "auto",
        "cpu",
        "ane",
        "gpu",
        "gpu:1002:164e:0",
        "gpu:10de:2684:12",
    ] {
        let snapshot = send(
            &mut core,
            json!({"type":"update_settings","settings":{"inference_device":value}}),
        );
        assert_eq!(snapshot["settings"]["inference_device"], value);
        assert_eq!(core.settings().unwrap().inference_device, value);
    }
    let too_long = "a".repeat(33);
    for invalid in [
        "",
        "AUTO",
        "Gpu",
        "ane ",
        "gpu:1:2:0",
        "gpu:1002:164e",
        "gpu:1002:164E:0",
        "gpu:1002:164e:",
        "gpu:1002:164e:1234",
        "npu",
        "cuda:0",
        too_long.as_str(),
    ] {
        let event = json!({"type":"update_settings","settings":{"inference_device":invalid}});
        let error = core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .unwrap_err();
        assert!(error.contains("inference_device"), "{invalid:?}: {error}");
        assert_eq!(
            core.settings().unwrap().inference_device,
            "gpu:10de:2684:12"
        );
    }
}

#[test]
fn inference_device_is_persisted_and_reloaded() {
    let root =
        std::env::temp_dir().join(format!("telaviva-inference-device-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(core.settings().unwrap().inference_device, "auto");
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"inference_device":"gpu:1002:164e:0"}}),
    );
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["schema_version"], 1);
    assert_eq!(persisted["inference_device"], "gpu:1002:164e:0");
    let reloaded = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(reloaded.settings().unwrap(), core.settings().unwrap());
    assert_eq!(
        reloaded.settings().unwrap().inference_device,
        "gpu:1002:164e:0"
    );
    let before_file = std::fs::read(&path).unwrap();
    assert!(core
        .submit_json(br#"{"type":"update_settings","settings":{"inference_device":"GPU"}}"#)
        .is_err());
    assert_eq!(std::fs::read(&path).unwrap(), before_file);
    assert_eq!(core.settings().unwrap().inference_device, "gpu:1002:164e:0");
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn inference_device_rejects_non_string_values() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for invalid in [json!(3), json!(true)] {
        let event = json!({"type":"update_settings","settings":{"inference_device":invalid}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.settings().unwrap().inference_device, "auto");
    }
}

#[test]
fn presence_min_brightness_defaults_to_20_is_range_checked_and_persisted() {
    assert_eq!(Settings::default().presence_min_brightness_percent, 20.0);
    assert_eq!(
        Settings::from_json(br#"{"schema_version":1}"#)
            .unwrap()
            .presence_min_brightness_percent,
        20.0
    );
    assert_eq!(
        Settings::from_json(br#"{"schema_version":1,"presence_min_brightness_percent":35}"#)
            .unwrap()
            .presence_min_brightness_percent,
        35.0
    );
    let root = std::env::temp_dir().join(format!("telaviva-min-brightness-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(
        core.settings().unwrap().presence_min_brightness_percent,
        20.0
    );
    let before = core.settings().unwrap();
    let before_file = std::fs::read(&path).unwrap();
    for invalid in [json!(-0.1), json!(90.1), json!(100), json!(-1), json!("20")] {
        let event = json!({"type":"update_settings","settings":{"presence_min_brightness_percent":invalid}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.settings().unwrap(), before);
    }
    assert_eq!(std::fs::read(&path).unwrap(), before_file);
    for accepted in [0.0, 10.0, 90.0] {
        let event = json!({"type":"update_settings","settings":{"presence_min_brightness_percent":accepted}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_ok());
        assert_eq!(
            core.settings().unwrap().presence_min_brightness_percent,
            accepted
        );
    }
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["presence_min_brightness_percent"], 90.0);
    let reloaded = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(reloaded.settings().unwrap(), core.settings().unwrap());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn manual_commands_reject_stale_or_invalid_connection_generation_without_mutation() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = json!({"type":"monitor_discovered","monitor":{
        "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
        "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
        "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
    }});
    let first = send(&mut core, discovered.clone());
    let old_generation = first["monitors"][0]["generation"].as_u64().unwrap();
    send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"screen"}),
    );
    let reconnected = send(&mut core, discovered);
    let current_generation = reconnected["monitors"][0]["generation"].as_u64().unwrap();
    assert!(current_generation > old_generation);
    let before = core.snapshot_value().unwrap();
    for invalid in [
        json!({"type":"set_brightness","monitor_id":"screen","value":80,"connection_generation":old_generation}),
        json!({"type":"set_volume","monitor_id":"screen","value":80,"connection_generation":"bad"}),
    ] {
        assert!(core
            .submit_json(&serde_json::to_vec(&invalid).unwrap())
            .is_err());
    }
    let after = core.snapshot_value().unwrap();
    assert_eq!(after, before);
    assert!(core.submit_json(&serde_json::to_vec(&json!({"type":"set_brightness","monitor_id":"screen","value":80,"connection_generation":current_generation})).unwrap()).is_ok());
    assert_eq!(
        core.snapshot_value().unwrap()["pending_actions"][0]["kind"],
        "set_brightness"
    );
    // Existing popup callers remain source-compatible when generation is omitted.
    assert!(core
        .submit_json(br#"{"type":"set_volume","monitor_id":"screen","value":60}"#)
        .is_ok());
}

#[test]
fn generation_checked_manual_command_keeps_existing_behavior_during_active_protections() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
            "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
        }}),
    );
    send(
        &mut core,
        json!({"type":"update_settings","settings":{
            "presence_protection_enabled":true,
            "screensaver_protection_enabled":true,
            "screensaver_targets":{"screen":5.0}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(&mut core, json!({"type":"screensaver","state":"active"}));
    let camera_generation = core.snapshot_value().unwrap()["camera"]["generation"]
        .as_u64()
        .unwrap();
    for timestamp in [capture_time(10_000), capture_time(20_000)] {
        send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":timestamp,"camera_generation":camera_generation}),
        );
    }
    let protected = core.snapshot_value().unwrap();
    let monitor = &protected["monitors"][0];
    assert!(monitor["presence_dimmed"].as_bool().unwrap());
    assert!(monitor["brightness"]["reasons"]
        .as_array()
        .unwrap()
        .contains(&json!("screensaver")));
    assert!(monitor["brightness"]["reasons"]
        .as_array()
        .unwrap()
        .contains(&json!("presence")));
    let generation = discovered["monitors"][0]["generation"].as_u64().unwrap();
    assert!(core
        .submit_json(
            &serde_json::to_vec(&json!({
                "type":"set_brightness",
                "monitor_id":"screen",
                "value":61.0,
                "connection_generation":generation
            }))
            .unwrap()
        )
        .is_ok());
    let after = core.snapshot_value().unwrap();
    assert_eq!(after["monitors"][0]["brightness"]["desired"], 61.0);
    assert_eq!(
        after["monitors"][0]["brightness"]["reasons"],
        protected["monitors"][0]["brightness"]["reasons"]
    );
}

#[test]
fn full_action_queue_does_not_mutate_generation_checked_manual_intent() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let mut first_generation = None;
    for index in 0..64 {
        let id = format!("screen-{index}");
        let discovered = send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
                "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
            }}),
        );
        if index == 0 {
            first_generation = discovered["monitors"][0]["generation"].as_u64();
        }
        send(
            &mut core,
            json!({"type":"set_brightness","monitor_id":id,"value":40}),
        );
        send(
            &mut core,
            json!({"type":"set_volume","monitor_id":id,"value":30}),
        );
    }
    let before = core.snapshot_value().unwrap();
    let error = core
        .submit_json(
            &serde_json::to_vec(&json!({
                "type":"set_brightness",
                "monitor_id":"screen-0",
                "value":90,
                "connection_generation":first_generation.unwrap()
            }))
            .unwrap(),
        )
        .unwrap_err();
    assert_eq!(error, "hardware action queue is full");
    assert_eq!(core.snapshot_value().unwrap(), before);
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
fn snapshot_omits_messages_and_locale_setting_still_validates() {
    // os_locale is still accepted by Core::new but no longer changes the snapshot.
    let core = Core::new(CoreOptions {
        os_locale: Some("pt_BR".into()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert!(core.snapshot_value().unwrap().get("messages").is_none());
    for locale in ["auto", "en-US", "pt-BR"] {
        let text = format!(r#"{{"schema_version":1,"locale":"{locale}"}}"#);
        assert!(
            Settings::from_json(text.as_bytes()).is_ok(),
            "{locale} should validate"
        );
    }
    let invalid = Settings::from_json(br#"{"schema_version":1,"locale":"fr-FR"}"#);
    assert_eq!(invalid.err().as_deref(), Some("unsupported locale"));
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
        .any(|a| a["kind"] == "set_brightness" && a["value"].as_f64() == Some(1.0)));
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
fn always_on_capture_is_default_and_keeps_return_mode() {
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
    let normal = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(normal["camera"]["capture_mode"], "continuous_monitoring");
    let single = send(
        &mut core,
        json!({"type":"update_settings","settings":{"always_on_camera":false}}),
    );
    assert_eq!(single["camera"]["capture_mode"], "single_frame");
    let always = send(
        &mut core,
        json!({"type":"update_settings","settings":{"always_on_camera":true}}),
    );
    assert_eq!(always["camera"]["mode"], "face");
    assert_eq!(always["camera"]["capture_mode"], "continuous_monitoring");
    let returning = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(10_000),"camera_generation":always["camera"]["generation"]}),
    );
    let returning = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":returning["camera"]["generation"]}),
    );
    assert_eq!(returning["camera"]["capture_mode"], "continuous_return");
}

#[test]
fn display_sleep_suspends_only_when_all_eligible_displays_sleep_and_ignores_stale_state() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for id in ["one", "two"] {
        send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
                "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
            }}),
        );
    }
    send(&mut core, json!({"type":"power","on_battery":false}));
    let active = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"always_on_camera":true}}),
    );
    let one = active["monitors"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["id"] == "one")
        .unwrap();
    let one_gen = one["generation"].as_u64().unwrap();
    let mixed = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"one","generation":one_gen,"state":"asleep"}),
    );
    assert_eq!(mixed["camera"]["mode"], "face");
    let observed = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(10_000),"camera_generation":mixed["camera"]["generation"]}),
    );
    let observed = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":observed["camera"]["generation"]}),
    );
    let monitors = observed["monitors"].as_array().unwrap();
    assert_eq!(
        monitors.iter().find(|m| m["id"] == "one").unwrap()["presence_dimmed"],
        false
    );
    assert_eq!(
        monitors.iter().find(|m| m["id"] == "two").unwrap()["presence_dimmed"],
        true
    );
    assert_eq!(
        observed["monitors"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["id"] == "one")
            .unwrap()["display_state"],
        "asleep"
    );
    let two = observed["monitors"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["id"] == "two")
        .unwrap();
    let asleep = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"two","generation":two["generation"],"state":"asleep"}),
    );
    assert_eq!(asleep["camera"]["mode"], "suspended");
    assert_eq!(
        asleep["camera"]["display_status"],
        "suspended_display_sleep"
    );
    assert_eq!(asleep["camera"]["capture_mode"], "stopped");
    let current_gen = asleep["camera"]["generation"].as_u64().unwrap();
    send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":current_gen}),
    );
    let rejected = core.snapshot_value().unwrap();
    assert_eq!(rejected["camera"]["status"], asleep["camera"]["status"]);
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"one","generation":one_gen,"state":"awake"}),
    );
    assert_eq!(awake["camera"]["mode"], "face");
    assert!(awake["camera"]["generation"].as_u64().unwrap() > current_gen);
    let before_stale = awake["camera"]["generation"].as_u64().unwrap();
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"one","generation":999,"state":"asleep"}),
    );
    assert_eq!(
        core.snapshot_value().unwrap()["camera"]["generation"],
        before_stale
    );
}

#[test]
fn display_sleep_handles_unknown_zero_brightness_and_no_eligible_monitor() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"zero","name":"Zero","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":30,"raw":30,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let active = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(active["camera"]["mode"], "face");
    let monitor = active["monitors"][0].clone();
    let unknown = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"zero","generation":monitor["generation"],"state":"unknown"}),
    );
    assert_eq!(unknown["camera"]["mode"], "face");
    let asleep = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"zero","generation":monitor["generation"],"state":"asleep"}),
    );
    assert_eq!(
        asleep["camera"]["display_status"],
        "suspended_display_sleep"
    );
    let mut empty = Core::new(CoreOptions::default()).unwrap();
    send(&mut empty, json!({"type":"power","on_battery":false}));
    let empty = send(
        &mut empty,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    assert_eq!(empty["camera"]["display_status"], "suspended_no_monitor");
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

#[test]
fn zero_brightness_suspends_as_low_brightness_not_display_sleep() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "zero", json!(0));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    assert!(s["camera"]["next_sample_in_ms"].is_null());
}

#[test]
fn threshold_suspends_camera_until_observed_brightness_rises_above_it() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(20));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["capture_mode"], "stopped");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    assert!(s["camera"]["next_sample_in_ms"].is_null());
    let before = s["camera"]["generation"].as_u64().unwrap();
    let s = send(
        &mut core,
        json!({"type":"monitor_observed","monitor_id":"screen","generation":monitor_generation(&s,"screen"),"brightness":{"raw":21}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    assert!(s["camera"]["generation"].as_u64().unwrap() > before);
}

#[test]
fn lowering_threshold_to_minimum_resumes_monitor_at_20() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(20));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "suspended");
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_min_brightness_percent":10}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
}

#[test]
fn raising_threshold_to_maximum_suspends_monitor_at_21_again() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(21));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_min_brightness_percent":90}}),
    );
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["capture_mode"], "stopped");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
}

#[test]
fn threshold_comparison_is_strict_at_the_exact_value() {
    let mut at_threshold = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut at_threshold, "screen", json!(20));
    let s = enable_presence(&mut at_threshold);
    assert_eq!(s["camera"]["mode"], "suspended");
    let mut just_above = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut just_above, "screen", json!(21));
    let s = enable_presence(&mut just_above);
    assert_eq!(s["camera"]["mode"], "face");
    let mut max_threshold = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut max_threshold, "screen", json!(90));
    send(
        &mut max_threshold,
        json!({"type":"power","on_battery":false}),
    );
    let s = send(
        &mut max_threshold,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"presence_min_brightness_percent":90}}),
    );
    assert_eq!(s["camera"]["mode"], "suspended");
    let mut max_above = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut max_above, "screen", json!(91));
    send(&mut max_above, json!({"type":"power","on_battery":false}));
    let s = send(
        &mut max_above,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"presence_min_brightness_percent":90}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn multiple_monitors_keep_camera_on_while_one_awake_protected_monitor_is_above() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "one", json!(10));
    discover_monitor(&mut core, "two", json!(50));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "suspended");
    let s = send(
        &mut core,
        json!({"type":"monitor_observed","monitor_id":"two","generation":monitor_generation(&s,"two"),"brightness":{"raw":10}}),
    );
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
}

#[test]
fn bright_asleep_or_protection_disabled_monitor_does_not_count_toward_the_camera() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "low", json!(10));
    discover_monitor(&mut core, "bright", json!(70));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"bright","generation":monitor_generation(&s,"bright"),"state":"asleep"}),
    );
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"bright","generation":monitor_generation(&s,"bright"),"state":"awake"}),
    );
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["bright"]}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":[]}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn presence_dim_keeps_camera_running_and_restore_never_suspends() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    enable_presence(&mut core);
    presence_sample(&mut core, "absent", 10_000);
    let s = presence_sample(&mut core, "absent", 20_000);
    let dim = pending_action(&s, "set_brightness");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    let s = hardware_ok(&mut core, &dim, 1.0);
    assert_eq!(s["monitors"][0]["presence_dimmed"], true);
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["camera"]["capture_mode"], "continuous_return");
    let mut modes = Vec::new();
    let s = presence_sample(&mut core, "present", 30_000);
    modes.push(s["camera"]["mode"].clone());
    let read = pending_action(&s, "read_brightness");
    let s = hardware_ok(&mut core, &read, 1.0);
    modes.push(s["camera"]["mode"].clone());
    let restore = pending_action(&s, "set_brightness");
    assert_eq!(restore["value"].as_f64(), Some(70.0));
    let s = hardware_ok(&mut core, &restore, 70.0);
    modes.push(s["camera"]["mode"].clone());
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
    assert!(modes.iter().all(|mode| *mode == "face"));
}

#[test]
fn screensaver_dim_keeps_camera_running() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    send(&mut core, json!({"type":"power","on_battery":false}));
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"screensaver_protection_enabled":true,"screensaver_targets":{"screen":5.0}}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(&mut core, json!({"type":"screensaver","state":"active"}));
    let dim = pending_action(&s, "set_brightness");
    assert_eq!(dim["value"].as_f64(), Some(5.0));
    let s = hardware_ok(&mut core, &dim, 5.0);
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
}

#[test]
fn manual_brightness_below_threshold_suspends_after_write_result_and_resumes_above() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    enable_presence(&mut core);
    let s = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":10}),
    );
    assert_eq!(s["camera"]["mode"], "face");
    let write = pending_action(&s, "set_brightness");
    let s = hardware_ok(&mut core, &write, 10.0);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":70}),
    );
    // The app owns the value after its own write, so the requested baseline counts at once.
    assert_eq!(s["camera"]["mode"], "face");
    let write = pending_action(&s, "set_brightness");
    let s = hardware_ok(&mut core, &write, 70.0);
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn entering_below_threshold_restarts_the_absence_count() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    enable_presence(&mut core);
    presence_sample(&mut core, "absent", 10_000);
    let s = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":10}),
    );
    let write = pending_action(&s, "set_brightness");
    let s = hardware_ok(&mut core, &write, 10.0);
    assert_eq!(s["camera"]["mode"], "suspended");
    let s = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":70}),
    );
    let write = pending_action(&s, "set_brightness");
    let s = hardware_ok(&mut core, &write, 70.0);
    assert_eq!(s["camera"]["mode"], "face");
    let s = presence_sample(&mut core, "absent", 20_000);
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
}

#[test]
fn unknown_brightness_monitor_does_not_suspend_while_another_monitor_is_above() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "unknown", json!(null));
    discover_monitor(&mut core, "bright", json!(70));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn asleep_displays_drop_pending_writes_and_wake_checks_ownership_before_write() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true,"screensaver_targets":{"screen":20.0}}}),
    );
    let pending = send(&mut core, json!({"type":"screensaver","state":"active"}));
    let pending_write = pending["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .unwrap()["id"]
        .clone();
    let asleep = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    assert_eq!(asleep["screensaver"]["state"], "active");
    assert!(asleep["monitors"][0]["brightness"]["reasons"]
        .as_array()
        .unwrap()
        .iter()
        .any(|reason| reason == "screensaver"));
    assert!(asleep["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":pending_write,"generation":pending["pending_actions"][0]["generation"],"ok":true,"value":20}),
    );
    let after_stale_write_ack = core.snapshot_value().unwrap();
    assert_eq!(
        after_stale_write_ack["monitors"][0]["brightness"]["observed"],
        asleep["monitors"][0]["brightness"]["observed"]
    );
    assert!(after_stale_write_ack["pending_actions"]
        .as_array()
        .unwrap()
        .is_empty());
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read = awake["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("wake must read before writing");
    let read = read.clone();
    let changed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":35}),
    );
    assert!(changed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    assert_eq!(changed["monitors"][0]["brightness"]["observed"], 35.0);
}

#[test]
fn wake_read_failure_does_not_issue_a_protection_write() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    let generation = discovered["monitors"][0]["generation"].clone();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true,"screensaver_targets":{"screen":20.0}}}),
    );
    send(&mut core, json!({"type":"screensaver","state":"active"}));
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read = awake["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|action| action["kind"] == "read_brightness")
        .unwrap()
        .clone();
    let after_failure = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"read_error"}),
    );
    assert!(after_failure["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|action| action["kind"] != "set_brightness"));
    assert_eq!(
        after_failure["monitors"][0]["brightness"]["status"],
        "read_error"
    );
}

#[test]
fn failed_wake_readback_keeps_ownership_and_retries_the_restore() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    let dimmed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    assert_eq!(dimmed["monitors"][0]["brightness"]["owned"], true);
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read = awake["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("wake must verify ownership before writing")
        .clone();
    let failed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
    );
    assert_eq!(failed["monitors"][0]["brightness"]["owned"], true);
    assert!(failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    let retry = failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("failed wake readback must be retried")
        .clone();
    let verified = send(
        &mut core,
        json!({"type":"hardware_result","action_id":retry["id"],"generation":retry["generation"],"ok":true,"value":0}),
    );
    let restore = verified["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .expect("matching readback must restore the baseline")
        .clone();
    assert_eq!(restore["value"].as_f64(), Some(70.0));
    let restored = send(
        &mut core,
        json!({"type":"hardware_result","action_id":restore["id"],"generation":restore["generation"],"ok":true,"value":70}),
    );
    assert_eq!(restored["monitors"][0]["brightness"]["raw"], 70.0);
    assert_eq!(restored["monitors"][0]["brightness"]["observed"], 70.0);
    assert_eq!(restored["monitors"][0]["brightness"]["owned"], true);
    assert_eq!(restored["pending_actions"], json!([]));
}

#[test]
fn failed_restore_write_is_retried_after_a_new_readback() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read = awake["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("wake must verify ownership before writing")
        .clone();
    let verified = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":0}),
    );
    let restore = verified["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .expect("matching readback must restore the baseline")
        .clone();
    assert_eq!(restore["value"].as_f64(), Some(70.0));
    let failed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":restore["id"],"generation":restore["generation"],"ok":false,"error":"io"}),
    );
    assert_eq!(failed["monitors"][0]["brightness"]["owned"], true);
    assert!(failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    let retry = failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("failed restore write must be preceded by a new readback")
        .clone();
    let verified_again = send(
        &mut core,
        json!({"type":"hardware_result","action_id":retry["id"],"generation":retry["generation"],"ok":true,"value":0}),
    );
    let restore_again = verified_again["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .expect("second matching readback must restore the baseline")
        .clone();
    assert_eq!(restore_again["value"].as_f64(), Some(70.0));
    let restored = send(
        &mut core,
        json!({"type":"hardware_result","action_id":restore_again["id"],"generation":restore_again["generation"],"ok":true,"value":70}),
    );
    assert_eq!(restored["monitors"][0]["brightness"]["raw"], 70.0);
    assert_eq!(restored["monitors"][0]["brightness"]["observed"], 70.0);
    assert_eq!(restored["monitors"][0]["brightness"]["owned"], true);
    assert_eq!(restored["pending_actions"], json!([]));
}

#[test]
fn restore_retries_are_bounded() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let mut read = awake["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("wake must verify ownership before writing")
        .clone();
    // Three failed readbacks each queue a new readback while ownership is kept.
    for _ in 0..3 {
        let failed = send(
            &mut core,
            json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
        );
        assert_eq!(failed["monitors"][0]["brightness"]["owned"], true);
        read = failed["pending_actions"]
            .as_array()
            .unwrap()
            .iter()
            .find(|a| a["kind"] == "read_brightness")
            .expect("failed readback must be retried while ownership is kept")
            .clone();
    }
    // The fourth failure exhausts the retries and reverts to the existing failure behavior.
    let exhausted = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
    );
    assert_eq!(exhausted["monitors"][0]["brightness"]["owned"], false);
    assert_eq!(
        exhausted["monitors"][0]["brightness"]["status"],
        "unsupported"
    );
    assert_eq!(exhausted["pending_actions"], json!([]));
}

#[test]
fn manual_brightness_change_wins_over_a_pending_restore_retry() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read = awake["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("wake must verify ownership before writing")
        .clone();
    let failed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
    );
    assert_eq!(failed["monitors"][0]["brightness"]["owned"], true);
    assert!(failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .any(|a| a["kind"] == "read_brightness"));
    let manual = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":40,"connection_generation":generation}),
    );
    assert_eq!(manual["monitors"][0]["brightness"]["desired"], 40.0);
    assert!(manual["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness" || a["value"] != 70.0));
    // The snapshot lists only the first pending action per monitor, so the manual write is
    // visible only after the retry readback at the head of the queue has been completed.
    let mut snapshot = manual;
    for _ in 0..6 {
        assert!(snapshot["pending_actions"]
            .as_array()
            .unwrap()
            .iter()
            .all(|a| a["kind"] != "set_brightness" || a["value"] != 70.0));
        let head = snapshot["pending_actions"][0].clone();
        if head["kind"] != "read_brightness" {
            break;
        }
        // The retry reads the monitor still at the dimmed value, which must not restore 70.
        snapshot = send(
            &mut core,
            json!({"type":"hardware_result","action_id":head["id"],"generation":head["generation"],"ok":true,"value":0}),
        );
    }
    let write = snapshot["pending_actions"][0].clone();
    assert_eq!(write["kind"], "set_brightness");
    assert_eq!(write["value"].as_f64(), Some(40.0));
    let acked = send(
        &mut core,
        json!({"type":"hardware_result","action_id":write["id"],"generation":write["generation"],"ok":true,"value":40}),
    );
    assert_eq!(acked["monitors"][0]["brightness"]["raw"], 40.0);
    assert_eq!(acked["monitors"][0]["brightness"]["observed"], 40.0);
    assert_eq!(acked["monitors"][0]["brightness"]["owned"], true);
    assert!(acked["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness" || a["value"] != 70.0));
}

#[test]
fn failed_dim_write_is_not_retried_as_a_restore() {
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
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    // A failed dim write is not a restore: ownership stays dropped and no restore is queued.
    let failed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":false,"error":"io"}),
    );
    assert_eq!(failed["monitors"][0]["brightness"]["owned"], false);
    assert!(failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "read_brightness"));
    assert!(failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness" || a["value"] != 70.0));
}

#[test]
fn shutdown_failed_readback_does_not_retry_the_restore() {
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
    let dimmed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":1}),
    );
    assert_eq!(dimmed["monitors"][0]["brightness"]["owned"], true);
    // Return the monitor and restore it, so that it owns its value but is no longer dimmed.
    let returned = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(30_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    let returned_read = returned["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("returning presence must verify ownership before writing")
        .clone();
    let verified = send(
        &mut core,
        json!({"type":"hardware_result","action_id":returned_read["id"],"generation":returned_read["generation"],"ok":true,"value":1}),
    );
    let restore = verified["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .expect("matching readback must restore the baseline")
        .clone();
    assert_eq!(restore["value"].as_f64(), Some(70.0));
    let restored = send(
        &mut core,
        json!({"type":"hardware_result","action_id":restore["id"],"generation":restore["generation"],"ok":true,"value":70}),
    );
    assert_eq!(restored["monitors"][0]["brightness"]["raw"], 70.0);
    assert_eq!(restored["monitors"][0]["brightness"]["observed"], 70.0);
    assert_eq!(restored["monitors"][0]["brightness"]["owned"], true);
    assert_eq!(restored["monitors"][0]["brightness"]["reasons"], json!([]));
    assert_eq!(restored["pending_actions"], json!([]));
    let shutdown = send(&mut core, json!({"type":"shutdown"}));
    let shutdown_read = shutdown["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .expect("shutdown must verify an owned value before quitting")
        .clone();
    // Once shutdown has started, a failed readback must not keep ownership or queue a retry.
    let failed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":shutdown_read["id"],"generation":shutdown_read["generation"],"ok":false,"error":"unsupported"}),
    );
    assert_eq!(failed["monitors"][0]["brightness"]["owned"], false);
    assert_eq!(failed["pending_actions"], json!([]));
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
        .any(|a| a["kind"] == "set_brightness" && a["value"] == 1.0));
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
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"always_on_camera":false}}),
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
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":1}),
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
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":1}),
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
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    let disabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":false}}),
    );
    assert_eq!(disabled["settings"]["presence_protection_enabled"], false);
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":1}),
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
    // Presence shares the screensaver's level of 50, so the write above already covers both
    // reasons and no separate presence write is queued.
    let failed = send(
        &mut core,
        json!({"type":"camera_status","status":"permission","generation":enabled["camera"]["generation"]}),
    );
    assert_eq!(failed["camera"]["mode"], "suspended");
    assert_eq!(
        failed["monitors"][0]["brightness"]["reasons"][0],
        "screensaver"
    );
    // Presence and the screensaver share one level, so clearing presence while the screensaver
    // still holds leaves the target at 50: no write is queued and the dim stays in place.
    assert!(failed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    // Restoration is checked once the screensaver clears: the owned value is read back first.
    let cleared = send(&mut core, json!({"type":"screensaver","state":"inactive"}));
    let check = cleared["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "read_brightness")
        .unwrap_or_else(|| panic!("expected ownership read, got {cleared:#}"))
        .clone();
    assert_eq!(check["target"].as_f64(), Some(70.0));
    let verified = send(
        &mut core,
        json!({"type":"hardware_result","action_id":check["id"],"generation":check["generation"],"ok":true,"value":50}),
    );
    let restore = verified["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["kind"] == "set_brightness")
        .expect("matching readback must restore the baseline");
    assert_eq!(restore["value"].as_f64(), Some(70.0));
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
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":1}),
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

#[test]
fn camera_history_defaults_empty_and_validation_rejects_invalid_entries() {
    let defaults = Settings::default();
    assert!(defaults.camera_history.is_empty());
    assert!(defaults.validate().is_ok());
    let with_history = |history: &[&str]| Settings {
        selected_camera_id: history.first().map(|id| id.to_string()),
        camera_history: history.iter().map(|id| id.to_string()).collect(),
        ..Default::default()
    };
    assert!(with_history(&["camera-a", "camera-b"]).validate().is_ok());
    assert!(with_history(&["camera-a", "camera-a"]).validate().is_err());
    assert!(with_history(&[""]).validate().is_err());
    let longest = "x".repeat(512);
    let oversized = "x".repeat(513);
    assert!(with_history(&[longest.as_str()]).validate().is_ok());
    assert!(with_history(&[oversized.as_str()]).validate().is_err());
    let ids: Vec<String> = (0..8).map(|i| format!("camera-{i}")).collect();
    let mut full = Settings {
        selected_camera_id: ids.first().cloned(),
        camera_history: ids,
        ..Default::default()
    };
    assert!(full.validate().is_ok());
    full.camera_history.push("camera-8".into());
    assert!(full.validate().is_err());
    let mut mismatch = with_history(&["camera-a", "camera-b"]);
    mismatch.selected_camera_id = Some("camera-b".into());
    assert!(mismatch.validate().is_err());
    mismatch.selected_camera_id = None;
    assert!(mismatch.validate().is_err());
}

#[test]
fn camera_history_legacy_selected_camera_loads_as_one_entry_without_corrupt_backup() {
    let root = std::env::temp_dir().join(format!("telaviva-camera-legacy-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"selected_camera_id":"camera-1"}"#,
    )
    .unwrap();
    let core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    let settings = core.settings().unwrap();
    assert_eq!(settings.selected_camera_id.as_deref(), Some("camera-1"));
    assert_eq!(settings.camera_history, vec!["camera-1"]);
    assert!(path.exists());
    assert!(!root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn camera_history_update_settings_moves_selection_to_front_and_null_clears_it() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let a = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-a"}}),
    );
    assert_eq!(a["settings"]["camera_history"], json!(["camera-a"]));
    let b = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-b"}}),
    );
    assert_eq!(b["settings"]["selected_camera_id"], "camera-b");
    assert_eq!(
        b["settings"]["camera_history"],
        json!(["camera-b", "camera-a"])
    );
    let reselected = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-a"}}),
    );
    assert_eq!(reselected["settings"]["selected_camera_id"], "camera-a");
    assert_eq!(
        reselected["settings"]["camera_history"],
        json!(["camera-a", "camera-b"])
    );
    let cleared = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":null}}),
    );
    assert_eq!(cleared["settings"]["selected_camera_id"], Value::Null);
    assert_eq!(cleared["settings"]["camera_history"], json!([]));
}

#[test]
fn camera_history_reselecting_current_front_keeps_generation_and_camera_status() {
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
    let selected = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-a","presence_protection_enabled":true}}),
    );
    let ready = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":selected["camera"]["generation"]}),
    );
    assert_eq!(ready["camera"]["status"], "ready");
    let same = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-a"}}),
    );
    assert_eq!(same["camera"]["generation"], ready["camera"]["generation"]);
    assert_eq!(same["camera"]["status"], "ready");
    assert_eq!(same["settings"]["camera_history"], json!(["camera-a"]));
}

#[test]
fn camera_history_is_capped_at_eight_most_recent_cameras() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let mut last = Value::Null;
    for i in 0..9 {
        last = send(
            &mut core,
            json!({"type":"update_settings","settings":{"selected_camera_id":format!("camera-{i}")}}),
        );
    }
    let expected: Vec<String> = (1..=8).rev().map(|i| format!("camera-{i}")).collect();
    assert_eq!(last["settings"]["camera_history"], json!(expected));
    assert_eq!(last["settings"]["selected_camera_id"], "camera-8");
}

#[test]
fn camera_history_payload_is_read_only_and_leaves_history_unchanged() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-a"}}),
    );
    let before = send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-b"}}),
    );
    let forged = json!({
        "type": "update_settings",
        "settings": {"camera_history": ["camera-x"]}
    });
    assert!(core
        .submit_json(&serde_json::to_vec(&forged).unwrap())
        .is_err());
    let forged_selection = json!({
        "type": "update_settings",
        "settings": {"selected_camera_id": "camera-c", "camera_history": ["camera-x"]}
    });
    assert!(core
        .submit_json(&serde_json::to_vec(&forged_selection).unwrap())
        .is_err());
    let after = core.snapshot_value().unwrap();
    assert_eq!(
        after["settings"]["camera_history"],
        json!(["camera-b", "camera-a"])
    );
    assert_eq!(after["settings"]["selected_camera_id"], "camera-b");
    assert_eq!(after["revision"], before["revision"]);
    assert_eq!(
        after["camera"]["generation"],
        before["camera"]["generation"]
    );
}

#[test]
fn camera_history_round_trips_through_persisted_settings_file() {
    let root = std::env::temp_dir().join(format!("telaviva-history-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-a"}}),
    );
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"selected_camera_id":"camera-b"}}),
    );
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["selected_camera_id"], "camera-b");
    assert_eq!(persisted["camera_history"], json!(["camera-b", "camera-a"]));
    let reloaded = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    assert_eq!(
        reloaded.settings().unwrap().camera_history,
        vec!["camera-b", "camera-a"]
    );
    assert_eq!(
        reloaded.settings().unwrap().selected_camera_id.as_deref(),
        Some("camera-b")
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn resume_after_display_wake_samples_after_one_second_then_configured_interval() {
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
    let active = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    let screen_generation = active["monitors"][0]["generation"].as_u64().unwrap();
    let asleep = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"asleep"}),
    );
    assert_eq!(asleep["camera"]["mode"], "suspended");
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"awake"}),
    );
    assert_eq!(awake["camera"]["mode"], "face");
    assert_eq!(awake["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    let waiting = core.snapshot_value().unwrap();
    assert_eq!(
        waiting["camera"]["next_sample_in_ms"].as_f64(),
        Some(1000.0)
    );
    let reported = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":awake["camera"]["generation"]}),
    );
    assert_eq!(
        reported["camera"]["next_sample_in_ms"].as_f64(),
        Some(10_000.0)
    );
}

#[test]
fn resume_after_power_return_and_protection_reenable_restarts_at_one_second() {
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
    assert_eq!(enabled["camera"]["mode"], "face");
    assert_eq!(
        enabled["camera"]["next_sample_in_ms"].as_f64(),
        Some(1000.0)
    );
    let reported = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    assert_eq!(
        reported["camera"]["next_sample_in_ms"].as_f64(),
        Some(10_000.0)
    );
    let battery = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(battery["camera"]["mode"], "suspended");
    let ac = send(&mut core, json!({"type":"power","on_battery":false}));
    assert_eq!(ac["camera"]["mode"], "face");
    assert_eq!(ac["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
}

#[test]
fn resume_clears_low_light_and_busy_status_but_keeps_permission() {
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
    let screen_generation = enabled["monitors"][0]["generation"].as_u64().unwrap();
    let dark = send(
        &mut core,
        json!({"type":"presence_observation","status":"insufficient_light","captured_at_ms":capture_time(10_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    assert_eq!(dark["camera"]["mode"], "light_only");
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"asleep"}),
    );
    let woke = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"awake"}),
    );
    assert_eq!(woke["camera"]["mode"], "face");
    let busy = send(
        &mut core,
        json!({"type":"camera_status","status":"busy","generation":woke["camera"]["generation"]}),
    );
    assert_eq!(busy["camera"]["mode"], "suspended");
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"asleep"}),
    );
    let woke_busy = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"awake"}),
    );
    assert_eq!(woke_busy["camera"]["status"], "unknown");
    assert_eq!(woke_busy["camera"]["mode"], "face");
    let denied = send(
        &mut core,
        json!({"type":"camera_status","status":"permission","generation":woke_busy["camera"]["generation"]}),
    );
    assert_eq!(denied["camera"]["mode"], "suspended");
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"asleep"}),
    );
    let woke_denied = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":screen_generation,"state":"awake"}),
    );
    assert_eq!(woke_denied["camera"]["status"], "permission");
    assert_eq!(woke_denied["camera"]["display_status"], "permission");
    assert_eq!(woke_denied["camera"]["mode"], "suspended");
    assert_eq!(woke_denied["camera"]["next_sample_in_ms"], Value::Null);
}

#[test]
fn camera_presence_is_null_when_presence_protection_is_disabled() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    let disabled = send(&mut core, json!({"type":"power","on_battery":false}));
    assert_eq!(disabled["camera"]["mode"], "disabled");
    assert_eq!(disabled["camera"]["presence"], Value::Null);
    let still_disabled = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":disabled["camera"]["generation"]}),
    );
    assert_eq!(still_disabled["camera"]["presence"], Value::Null);
}

#[test]
fn camera_presence_tracks_the_latest_face_sample_and_light_state() {
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
    assert_eq!(enabled["camera"]["mode"], "face");
    assert_eq!(enabled["camera"]["presence"], Value::Null);

    let present = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    assert_eq!(present["camera"]["presence"], "present");

    let absent = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":present["camera"]["generation"]}),
    );
    assert_eq!(absent["camera"]["mode"], "face");
    assert_eq!(absent["camera"]["presence"], "absent");

    let dark = send(
        &mut core,
        json!({"type":"presence_observation","status":"insufficient_light","captured_at_ms":capture_time(30_000),"camera_generation":absent["camera"]["generation"]}),
    );
    assert_eq!(dark["camera"]["mode"], "light_only");
    assert_eq!(dark["camera"]["presence"], "insufficient_light");

    let usable = send(
        &mut core,
        json!({"type":"presence_observation","status":"usable_light","captured_at_ms":capture_time(40_000),"camera_generation":dark["camera"]["generation"]}),
    );
    assert_eq!(usable["camera"]["mode"], "light_only");
    assert_eq!(usable["camera"]["presence"], "insufficient_light");
    let recovered = send(
        &mut core,
        json!({"type":"presence_observation","status":"usable_light","captured_at_ms":capture_time(50_000),"camera_generation":usable["camera"]["generation"]}),
    );
    assert_eq!(recovered["camera"]["mode"], "face");
    assert_eq!(recovered["camera"]["presence"], Value::Null);
}

#[test]
fn camera_presence_clears_on_suspend_and_stays_null_until_a_new_resume_sample() {
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
    let present = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(10_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    assert_eq!(present["camera"]["presence"], "present");

    let battery = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(battery["camera"]["mode"], "suspended");
    assert_eq!(battery["camera"]["presence"], Value::Null);
    let resumed = send(&mut core, json!({"type":"power","on_battery":false}));
    assert_eq!(resumed["camera"]["mode"], "face");
    assert_eq!(resumed["camera"]["presence"], Value::Null);

    let sampled = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":resumed["camera"]["generation"]}),
    );
    assert_eq!(sampled["camera"]["presence"], "absent");
}

#[test]
fn camera_presence_ignores_stale_generation_and_out_of_order_samples() {
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
    let old_generation = enabled["camera"]["generation"].clone();
    let suspended = send(&mut core, json!({"type":"power","on_battery":true}));
    assert_eq!(suspended["camera"]["presence"], Value::Null);
    let resumed = send(&mut core, json!({"type":"power","on_battery":false}));
    assert_eq!(resumed["camera"]["presence"], Value::Null);

    let stale = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":old_generation}),
    );
    assert_eq!(stale["camera"]["presence"], Value::Null);

    let present = send(
        &mut core,
        json!({"type":"presence_observation","status":"present","captured_at_ms":capture_time(30_000),"camera_generation":resumed["camera"]["generation"]}),
    );
    assert_eq!(present["camera"]["presence"], "present");
    let out_of_order = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(25_000),"camera_generation":resumed["camera"]["generation"]}),
    );
    assert_eq!(out_of_order["camera"]["presence"], "present");
    let stale_after_sample = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(40_000),"camera_generation":old_generation}),
    );
    assert_eq!(stale_after_sample["camera"]["presence"], "present");
}

// Ambient follow and per-monitor protection contracts from
// specs/archived/macbook-brightness-follow/contracts.md: settings, update_settings and snapshot fields.

// Per-display presence thresholds: values 0..=90 and finite, keys 1..=512 bytes, at most 128
// entries. A settings file without the key loads with an empty map.
#[test]
fn presence_thresholds_by_display_validate_and_default_to_an_empty_map() {
    assert!(Settings::default()
        .presence_min_brightness_by_display
        .is_empty());
    let old_file = br#"{"schema_version":1,"presence_protection_enabled":true}"#;
    let old = Settings::from_json(old_file).unwrap();
    assert!(old.presence_min_brightness_by_display.is_empty());
    // The key takes a JSON object literal; the bounds are checked at both edges.
    let file =
        |map: &str| format!(r#"{{"schema_version":1,"presence_min_brightness_by_display":{map}}}"#);
    assert!(Settings::from_json(file(r#"{"m1":0,"builtin":90}"#).as_bytes()).is_ok());
    assert!(Settings::from_json(file(r#"{"m1":95}"#).as_bytes()).is_err());
    assert!(Settings::from_json(file(r#"{"m1":-1}"#).as_bytes()).is_err());
    assert!(Settings::from_json(file(r#"{"":20}"#).as_bytes()).is_err());
    let with_key = |key: &str| Settings {
        presence_min_brightness_by_display: [(key.to_string(), 20.0)].into(),
        ..Default::default()
    };
    let longest = "x".repeat(512);
    let oversized = "x".repeat(513);
    assert!(with_key(longest.as_str()).validate().is_ok());
    assert!(with_key(oversized.as_str()).validate().is_err());
    let mut full = Settings {
        presence_min_brightness_by_display: (0..128).map(|i| (format!("m{i}"), 20.0)).collect(),
        ..Default::default()
    };
    assert!(full.validate().is_ok());
    full.presence_min_brightness_by_display
        .insert("m128".into(), 20.0);
    assert!(full.validate().is_err());

    // update_settings applies the same rules, and a rejected save leaves the stored map unchanged.
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for value in [json!({"m1": 95}), json!({"m1": -1}), json!({"": 20})] {
        let event = json!({"type": "update_settings", "settings": {"presence_min_brightness_by_display": value}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
    }
    let snapshot = core.snapshot_value().unwrap();
    assert_eq!(
        snapshot["settings"]["presence_min_brightness_by_display"],
        json!({})
    );
    let event = json!({"type": "update_settings", "settings": {"presence_min_brightness_by_display": {"m1": 0, "builtin": 90}}});
    core.submit_json(&serde_json::to_vec(&event).unwrap())
        .unwrap();
    let snapshot = core.snapshot_value().unwrap();
    assert_eq!(
        snapshot["settings"]["presence_min_brightness_by_display"],
        json!({"m1": 0.0, "builtin": 90.0})
    );
}

#[test]
fn ambient_settings_default_to_follow_off_with_empty_curves_and_protection_list() {
    let core = Core::new(CoreOptions::default()).unwrap();
    let snapshot = core.snapshot_value().unwrap();
    let settings = &snapshot["settings"];
    assert_eq!(settings["ambient_follow_enabled"].as_bool(), Some(false));
    assert_eq!(settings["ambient_curves"], json!({}));
    assert_eq!(settings["protection_disabled"], json!([]));
}

#[test]
fn ambient_curve_accepts_bounds_equal_builtin_levels_and_any_ascending_monitors() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let curves = [
        // 0 and 100 are inclusive bounds.
        json!({"points":[{"monitor":25.0,"builtin":0.0},{"monitor":50.0,"builtin":50.0},{"monitor":75.0,"builtin":75.0},{"monitor":100.0,"builtin":100.0}]}),
        // Equal neighboring builtin levels are non-decreasing.
        json!({"points":[{"monitor":25.0,"builtin":50.0},{"monitor":50.0,"builtin":50.0},{"monitor":75.0,"builtin":50.0},{"monitor":100.0,"builtin":50.0}]}),
        // Points need not sit at the former fixed percents; any strictly ascending monitors are kept.
        json!({"points":[{"monitor":10.0,"builtin":5.0},{"monitor":30.0,"builtin":30.0}]}),
    ];
    for curve in &curves {
        let snapshot = send(
            &mut core,
            json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}}),
        );
        let stored = &snapshot["settings"]["ambient_curves"]["screen"];
        assert_eq!(stored, curve);
    }
}

#[test]
fn ambient_curve_rejects_out_of_range_and_non_finite_points() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    // A valid boundary curve is saved first, so the rejections below cannot pass
    // merely because the field is unknown to the core.
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":0.0},{"monitor":50.0,"builtin":50.0},{"monitor":75.0,"builtin":75.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    for curve in [
        json!({"points":[{"monitor":25.0,"builtin":-1.0},{"monitor":50.0,"builtin":50.0},{"monitor":75.0,"builtin":75.0},{"monitor":100.0,"builtin":100.0}]}),
        json!({"points":[{"monitor":25.0,"builtin":0.0},{"monitor":50.0,"builtin":50.0},{"monitor":75.0,"builtin":75.0},{"monitor":100.0,"builtin":101.0}]}),
        json!({"points":[{"monitor":0.0,"builtin":0.0},{"monitor":50.0,"builtin":50.0}]}),
        json!({"points":[{"monitor":25.0,"builtin":0.0},{"monitor":101.0,"builtin":100.0}]}),
    ] {
        let event =
            json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    }
    // NaN and Infinity are not JSON, so these are sent as raw text the parser must reject.
    for raw in [
        r#"{"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25,"builtin":NaN},{"monitor":50,"builtin":50},{"monitor":75,"builtin":75},{"monitor":100,"builtin":100}]}}}}"#,
        r#"{"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25,"builtin":0},{"monitor":50,"builtin":Infinity},{"monitor":75,"builtin":75},{"monitor":100,"builtin":100}]}}}}"#,
        r#"{"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25,"builtin":0},{"monitor":-Infinity,"builtin":50},{"monitor":75,"builtin":75},{"monitor":100,"builtin":100}]}}}}"#,
    ] {
        assert!(core.submit_json(raw.as_bytes()).is_err());
        assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    }
}

#[test]
fn ambient_curve_rejects_decreasing_present_points_and_keeps_previous_curves() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    for curve in [
        json!({"points":[{"monitor":25.0,"builtin":50.0},{"monitor":50.0,"builtin":20.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
        json!({"points":[{"monitor":25.0,"builtin":40.0},{"monitor":75.0,"builtin":30.0}]}),
        json!({"points":[{"monitor":50.0,"builtin":60.0},{"monitor":75.0,"builtin":30.0}]}),
        json!({"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":60.0},{"monitor":75.0,"builtin":50.0},{"monitor":100.0,"builtin":40.0}]}),
    ] {
        let event =
            json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    }
}

#[test]
fn ambient_curve_keys_and_protection_ids_must_be_one_to_512_bytes() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let curve = json!({"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]});
    let longest = "x".repeat(512);
    let oversized = "x".repeat(513);
    // "é" is two bytes in UTF-8: 256 of them fill exactly 512 bytes, 257 exceed the limit.
    let longest_multibyte = "é".repeat(256);
    let oversized_multibyte = "é".repeat(257);
    let curve_update =
        |key: &str| json!({"type":"update_settings","settings":{"ambient_curves":{key: curve}}});
    let protection_update =
        |id: &str| json!({"type":"update_settings","settings":{"protection_disabled":[id]}});
    let first = send(&mut core, curve_update(longest.as_str()));
    let curves = &first["settings"]["ambient_curves"];
    assert!(curves.get(longest.as_str()).is_some());
    // Each update replaces the whole map, so only the multibyte key remains afterwards.
    let second = send(&mut core, curve_update(longest_multibyte.as_str()));
    let curves = &second["settings"]["ambient_curves"];
    assert!(curves.get(longest_multibyte.as_str()).is_some());
    let before = core.snapshot_value().unwrap()["settings"].clone();
    for key in ["", oversized.as_str(), oversized_multibyte.as_str()] {
        assert!(core
            .submit_json(&serde_json::to_vec(&curve_update(key)).unwrap())
            .is_err());
    }
    assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    // Protection IDs follow the same 1 to 512 byte rule.
    let listed = send(&mut core, protection_update(longest.as_str()));
    assert_eq!(listed["settings"]["protection_disabled"], json!([longest]));
    let before = core.snapshot_value().unwrap()["settings"].clone();
    for id in ["", oversized.as_str(), oversized_multibyte.as_str()] {
        assert!(core
            .submit_json(&serde_json::to_vec(&protection_update(id)).unwrap())
            .is_err());
    }
    assert_eq!(core.snapshot_value().unwrap()["settings"], before);
}

#[test]
fn ambient_curves_and_protection_disabled_cap_at_128_entries() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let curve = json!({"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]});
    let mut curves = serde_json::Map::new();
    for index in 0..128 {
        curves.insert(format!("display-{index}"), curve.clone());
    }
    let full = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":Value::Object(curves.clone())}}),
    );
    let stored = full["settings"]["ambient_curves"].clone();
    assert_eq!(stored.as_object().unwrap().len(), 128);
    curves.insert("display-128".to_string(), curve);
    let too_many =
        json!({"type":"update_settings","settings":{"ambient_curves":Value::Object(curves)}});
    assert!(core
        .submit_json(&serde_json::to_vec(&too_many).unwrap())
        .is_err());
    assert_eq!(
        core.snapshot_value().unwrap()["settings"]["ambient_curves"],
        stored
    );

    let ids: Vec<String> = (0..128).map(|index| format!("display-{index}")).collect();
    let mut sorted_ids = ids.clone();
    sorted_ids.sort();
    let listed = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":ids}}),
    );
    assert_eq!(listed["settings"]["protection_disabled"], json!(sorted_ids));
    let mut too_many_ids = ids.clone();
    too_many_ids.push("display-128".to_string());
    let rejected =
        json!({"type":"update_settings","settings":{"protection_disabled":too_many_ids}});
    assert!(core
        .submit_json(&serde_json::to_vec(&rejected).unwrap())
        .is_err());
    assert_eq!(
        core.snapshot_value().unwrap()["settings"]["protection_disabled"],
        json!(sorted_ids)
    );
}

#[test]
fn ambient_settings_round_trip_through_persisted_settings_file() {
    let root = std::env::temp_dir().join(format!("telaviva-ambient-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    let curve = json!({"points":[{"monitor":25.0,"builtin":18.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]});
    let saved = send(
        &mut core,
        json!({"type":"update_settings","settings":{
            "ambient_follow_enabled":true,
            "ambient_curves":{"screen":curve},
            "protection_disabled":["screen"]
        }}),
    );
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["ambient_follow_enabled"].as_bool(), Some(true));
    assert_eq!(persisted["ambient_curves"], json!({"screen": curve}));
    assert_eq!(persisted["protection_disabled"], json!(["screen"]));
    let reloaded = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    let loaded = reloaded.snapshot_value().unwrap();
    assert_eq!(loaded["settings"], saved["settings"]);
    assert_eq!(
        loaded["settings"]["ambient_follow_enabled"].as_bool(),
        Some(true)
    );
    assert_eq!(
        loaded["settings"]["ambient_curves"],
        json!({"screen": curve})
    );
    assert_eq!(loaded["settings"]["protection_disabled"], json!(["screen"]));
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn ambient_points_serialize_only_as_points_and_round_trip() {
    // A curve with four points serializes as a points list and never with pN keys.
    let plain = Settings::from_json(
        r#"{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}"#
            .as_bytes(),
    )
    .unwrap();
    let value = serde_json::to_value(&plain).unwrap();
    assert_eq!(
        value["ambient_curves"],
        json!({"screen":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}})
    );
    // Low-end points (5, 10, 15 and 20 percent) are serialized in order and survive a round trip.
    let refined = Settings::from_json(
        r#"{"ambient_curves":{"screen":{"points":[{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":12.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}"#.as_bytes(),
    )
    .unwrap();
    let value = serde_json::to_value(&refined).unwrap();
    assert_eq!(
        value["ambient_curves"]["screen"],
        json!({"points":[{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":12.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]})
    );
    let reparsed = Settings::from_json(&serde_json::to_vec(&refined).unwrap()).unwrap();
    assert_eq!(reparsed, refined);
}

#[test]
fn ambient_point_at_one_percent_serializes_first_and_round_trips() {
    // Without a 1% point the list starts at 5 percent, and no point has monitor 1.
    let without_1 = Settings::from_json(
        r#"{"ambient_curves":{"screen":{"points":[{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}"#
            .as_bytes(),
    )
    .unwrap();
    let value = serde_json::to_value(&without_1).unwrap();
    let points = value["ambient_curves"]["screen"]["points"]
        .as_array()
        .unwrap();
    assert_eq!(points.len(), 6);
    assert_eq!(points[0]["monitor"], 5.0);
    // A 1 percent point is serialized as the first entry and survives a round trip.
    let with_1 = Settings::from_json(
        r#"{"ambient_curves":{"screen":{"points":[{"monitor":1.0,"builtin":2.0},{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":12.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}"#.as_bytes(),
    )
    .unwrap();
    let value = serde_json::to_value(&with_1).unwrap();
    assert_eq!(
        value["ambient_curves"]["screen"]["points"][0],
        json!({"monitor":1.0,"builtin":2.0})
    );
    let reparsed = Settings::from_json(&serde_json::to_vec(&with_1).unwrap()).unwrap();
    assert_eq!(reparsed, with_1);
}

#[test]
fn ambient_update_settings_replaces_curves_and_protection_list_whole() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let first_curve = json!({"points":[{"monitor":25.0,"builtin":18.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]});
    let second_curve = json!({"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":20.0},{"monitor":75.0,"builtin":30.0},{"monitor":100.0,"builtin":40.0}]});
    let first = send(
        &mut core,
        json!({"type":"update_settings","settings":{
            "ambient_follow_enabled":true,
            "ambient_curves":{"screen-a":first_curve},
            "protection_disabled":["screen-a"]
        }}),
    );
    assert_eq!(
        first["settings"]["ambient_curves"],
        json!({"screen-a": first_curve})
    );
    assert_eq!(
        first["settings"]["protection_disabled"],
        json!(["screen-a"])
    );
    let replaced = send(
        &mut core,
        json!({"type":"update_settings","settings":{
            "ambient_curves":{"screen-b":second_curve},
            "protection_disabled":["screen-b"]
        }}),
    );
    assert_eq!(
        replaced["settings"]["ambient_curves"],
        json!({"screen-b": second_curve})
    );
    assert_eq!(
        replaced["settings"]["protection_disabled"],
        json!(["screen-b"])
    );
    // Keys omitted from an update keep their values.
    assert_eq!(
        replaced["settings"]["ambient_follow_enabled"].as_bool(),
        Some(true)
    );
    let toggled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":false}}),
    );
    assert_eq!(
        toggled["settings"]["ambient_follow_enabled"].as_bool(),
        Some(false)
    );
    assert_eq!(
        toggled["settings"]["ambient_curves"],
        json!({"screen-b": second_curve})
    );
    assert_eq!(
        toggled["settings"]["protection_disabled"],
        json!(["screen-b"])
    );
    // An empty value is still a whole replacement.
    let cleared = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{},"protection_disabled":[]}}),
    );
    assert_eq!(cleared["settings"]["ambient_curves"], json!({}));
    assert_eq!(cleared["settings"]["protection_disabled"], json!([]));
    assert_eq!(
        cleared["settings"]["ambient_follow_enabled"].as_bool(),
        Some(false)
    );
}

#[test]
fn ambient_invalid_curve_rejects_the_whole_update_and_keeps_previous_settings() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    // The rejected update also carries a valid toggle; none of it may be applied.
    let invalid = json!({"type":"update_settings","settings":{
        "ambient_follow_enabled":true,
        "ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":50.0},{"monitor":50.0,"builtin":20.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}
    }});
    let message = core
        .submit_json(&serde_json::to_vec(&invalid).unwrap())
        .unwrap_err();
    assert!(!message.is_empty());
    assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    let out_of_range = json!({"type":"update_settings","settings":{
        "ambient_follow_enabled":true,
        "ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":120.0}]}}
    }});
    assert!(core
        .submit_json(&serde_json::to_vec(&out_of_range).unwrap())
        .is_err());
    assert_eq!(core.snapshot_value().unwrap()["settings"], before);
}

#[test]
fn ambient_low_end_points_follow_the_same_range_and_order_rules() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    // A 10 percent point with a higher builtin level than the 25 percent point breaks the order;
    // a 5 percent point with a builtin level below 0 or above 100 breaks the range.
    for curve in [
        json!({"points":[{"monitor":10.0,"builtin":30.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
        json!({"points":[{"monitor":5.0,"builtin":-1.0},{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
        json!({"points":[{"monitor":5.0,"builtin":101.0},{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
    ] {
        let event =
            json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    }
    // Equal neighboring low-end builtin levels are non-decreasing, so the curve is stored as sent.
    let curve = json!({"points":[{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":8.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]});
    let saved = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}}),
    );
    assert_eq!(saved["settings"]["ambient_curves"]["screen"], curve);
}

#[test]
fn ambient_one_percent_point_follows_the_same_range_and_order_rules() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    // A 1 percent point with a builtin level above the 5 percent point's breaks the order;
    // a 1 percent point with a builtin level below 0 or above 100 breaks the range.
    for curve in [
        json!({"points":[{"monitor":1.0,"builtin":5.0},{"monitor":5.0,"builtin":4.0},{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
        json!({"points":[{"monitor":1.0,"builtin":-1.0},{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
        json!({"points":[{"monitor":1.0,"builtin":101.0},{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}),
    ] {
        let event =
            json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    }
    // An equal 1 and 5 percent builtin level is non-decreasing, so the curve is stored as sent.
    let curve = json!({"points":[{"monitor":1.0,"builtin":4.0},{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":12.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]});
    let saved = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}}),
    );
    assert_eq!(saved["settings"]["ambient_curves"]["screen"], curve);
}

#[test]
fn ambient_legacy_point_keys_are_imported_as_ascending_points() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    // Each non-null pN becomes the point { monitor: N, builtin: pN }; nulls and omitted keys are skipped.
    let expected = json!({"points":[{"monitor":1.0,"builtin":2.0},{"monitor":5.0,"builtin":4.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":100.0,"builtin":90.0}]});
    let saved = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"p50":45.0,"p1":2.0,"p100":90.0,"p25":20.0,"p75":null,"p5":4.0}}}}),
    );
    assert_eq!(saved["settings"]["ambient_curves"]["screen"], expected);
    // The same legacy payload read from Settings::from_json is written back as points only.
    let settings = Settings::from_json(
        br#"{"ambient_curves":{"screen":{"p50":45.0,"p1":2.0,"p100":90.0,"p25":20.0,"p75":null,"p5":4.0}}}"#,
    )
    .unwrap();
    let value = serde_json::to_value(&settings).unwrap();
    assert_eq!(value["ambient_curves"]["screen"], expected);
    assert!(value["ambient_curves"]["screen"].get("p25").is_none());
    let reparsed = Settings::from_json(&serde_json::to_vec(&settings).unwrap()).unwrap();
    assert_eq!(reparsed, settings);
}

#[test]
fn ambient_legacy_points_in_a_settings_file_are_rewritten_as_points_on_the_next_store() {
    let root = std::env::temp_dir().join(format!("telaviva-ambient-legacy-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::create_dir_all(&root);
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        r#"{"ambient_curves":{"screen":{"p25":20.0,"p50":45.0,"p75":70.0,"p100":90.0}}}"#,
    )
    .unwrap();
    let mut core = Core::new(CoreOptions {
        config_path: Some(path.clone()),
        ..CoreOptions::default()
    })
    .unwrap();
    let expected = json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]});
    let loaded = core.snapshot_value().unwrap();
    assert_eq!(loaded["settings"]["ambient_curves"]["screen"], expected);
    // The next store writes the points form, and no pN key is ever written.
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true}}),
    );
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["ambient_curves"], json!({"screen": expected}));
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn ambient_legacy_curve_with_one_non_null_point_is_incomplete() {
    let one = Settings::from_json(
        br#"{"ambient_curves":{"screen":{"p25":20.0,"p50":null,"p75":null,"p100":null}}}"#,
    )
    .unwrap();
    assert!(!one.ambient_curves["screen"].is_complete());
    let two = Settings::from_json(
        br#"{"ambient_curves":{"screen":{"p25":20.0,"p50":null,"p75":null,"p100":90.0}}}"#,
    )
    .unwrap();
    assert!(two.ambient_curves["screen"].is_complete());
}

#[test]
fn ambient_curve_rejects_unsorted_and_duplicate_monitor_percents() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    for curve in [
        // Monitor percents must be strictly ascending in the stored list.
        json!({"points":[{"monitor":50.0,"builtin":40.0},{"monitor":25.0,"builtin":10.0}]}),
        // A repeated monitor percent is not strictly ascending.
        json!({"points":[{"monitor":25.0,"builtin":10.0},{"monitor":25.0,"builtin":20.0}]}),
    ] {
        let event =
            json!({"type":"update_settings","settings":{"ambient_curves":{"screen":curve}}});
        assert!(core
            .submit_json(&serde_json::to_vec(&event).unwrap())
            .is_err());
        assert_eq!(core.snapshot_value().unwrap()["settings"], before);
    }
}

#[test]
fn ambient_curve_accepts_exactly_32_points_and_rejects_33() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    // Monitor percents 1, 4, ..., 94 and builtin levels 0, 3, ..., 93 are ascending and in range.
    let points = |count: usize| -> Value {
        Value::Array(
            (0..count)
                .map(|index| {
                    json!({"monitor": 1.0 + 3.0 * index as f64, "builtin": 3.0 * index as f64})
                })
                .collect(),
        )
    };
    let full = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":points(32)}}}}),
    );
    assert_eq!(
        full["settings"]["ambient_curves"]["screen"]["points"]
            .as_array()
            .unwrap()
            .len(),
        32
    );
    let before = core.snapshot_value().unwrap()["settings"].clone();
    let too_many = json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":points(33)}}}});
    assert!(core
        .submit_json(&serde_json::to_vec(&too_many).unwrap())
        .is_err());
    assert_eq!(core.snapshot_value().unwrap()["settings"], before);
}

#[test]
fn ambient_snapshot_reports_static_builtin_state_and_mirrors_the_enabled_flag() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let initial = core.snapshot_value().unwrap();
    let ambient = &initial["ambient"];
    assert_eq!(ambient["enabled"].as_bool(), Some(false));
    assert_eq!(ambient["available"].as_bool(), Some(false));
    assert_eq!(ambient["active"].as_bool(), Some(false));
    assert_eq!(ambient["calibrating"].as_bool(), Some(false));
    assert_eq!(ambient["builtin_percent"], Value::Null);
    let enabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true}}),
    );
    let ambient = &enabled["ambient"];
    assert_eq!(ambient["enabled"].as_bool(), Some(true));
    // Until the shell reports the built-in display, it is unavailable, so follow is not active.
    assert_eq!(ambient["available"].as_bool(), Some(false));
    assert_eq!(ambient["active"].as_bool(), Some(false));
    assert_eq!(ambient["calibrating"].as_bool(), Some(false));
    assert_eq!(ambient["builtin_percent"], Value::Null);
}

#[test]
fn ambient_follow_state_is_uncalibrated_until_a_complete_curve_exists() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
            "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
        }}),
    );
    assert_eq!(discovered["monitors"][0]["follow"], "uncalibrated");
    let partial = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":10.0}]}}}}),
    );
    assert_eq!(partial["monitors"][0]["follow"], "uncalibrated");
    let complete = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":18.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    assert_eq!(complete["monitors"][0]["follow"], "eligible");
    // Eligibility is reported whether or not follow is switched on.
    let enabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true}}),
    );
    assert_eq!(enabled["monitors"][0]["follow"], "eligible");
}

#[test]
fn ambient_curve_with_fewer_than_two_points_is_uncalibrated() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, follow_discovery("screen", 0.0, 100.0, 50.0));
    let partial = json!({"points":[{"monitor":5.0,"builtin":4.0}]});
    let snapshot = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"screen":partial.clone()}}}),
    );
    assert_eq!(snapshot["monitors"][0]["follow"], "uncalibrated");
    // With one point the curve maps no level, so a built-in change writes nothing.
    assert_eq!(follow_first_write_for(partial, 0.0, 100.0, 90.0, 6.0), None);
}

#[test]
fn ambient_completeness_needs_two_points_and_no_fixed_percent() {
    // A curve is complete with any two points; one point or none is not.
    let two = Settings::from_json(
        br#"{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":100.0,"builtin":90.0}]}}}"#,
    )
    .unwrap();
    assert!(two.ambient_curves["screen"].is_complete());
    let one = Settings::from_json(
        br#"{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":20.0}]}}}"#,
    )
    .unwrap();
    assert!(!one.ambient_curves["screen"].is_complete());
    let none = Settings::from_json(br#"{"ambient_curves":{"screen":{"points":[]}}}"#).unwrap();
    assert!(!none.ambient_curves["screen"].is_complete());
    // Through the core: a one-percent point alone does not make the monitor eligible.
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, follow_discovery("screen", 0.0, 100.0, 50.0));
    let snapshot = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"screen":{"points":[{"monitor":1.0,"builtin":2.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]}}}}),
    );
    assert_eq!(snapshot["monitors"][0]["follow"], "eligible");
    let snapshot = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":1.0,"builtin":2.0}]}}}}),
    );
    assert_eq!(snapshot["monitors"][0]["follow"], "uncalibrated");
    assert_eq!(
        follow_first_write_for(
            json!({"points":[{"monitor":1.0,"builtin":2.0}]}),
            0.0,
            100.0,
            90.0,
            6.0
        ),
        None
    );
}

#[test]
fn ambient_follow_state_is_unsupported_without_ddc_brightness() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":false,"value":null,"raw":null,"min":0,"max":100},
            "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
        }}),
    );
    // A complete curve cannot make a monitor without DDC brightness eligible.
    let snapshot = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"screen":{"points":[{"monitor":25.0,"builtin":18.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    assert_eq!(snapshot["monitors"][0]["follow"], "unsupported");
}

#[test]
fn ambient_follow_state_is_unstable_for_ambiguous_identity() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"ambiguous","name":"Display","identity_status":"ambiguous","connection_path":"",
            "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
            "volume":{"supported":true,"value":30,"raw":30,"min":0,"max":100}
        }}),
    );
    assert_eq!(discovered["monitors"][0]["follow"], "unstable");
    let complete = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_curves":{"ambiguous":{"points":[{"monitor":25.0,"builtin":18.0},{"monitor":50.0,"builtin":40.0},{"monitor":75.0,"builtin":66.0},{"monitor":100.0,"builtin":100.0}]}}}}),
    );
    // A complete curve does not make a monitor with ambiguous identity eligible.
    assert_eq!(complete["monitors"][0]["follow"], "unstable");
}

#[test]
fn protection_flag_is_true_by_default_and_false_only_for_listed_monitor_ids() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for id in ["screen-a", "screen-b"] {
        send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":50,"raw":50,"min":0,"max":100},
                "volume":{"supported":true,"value":25,"raw":25,"min":0,"max":100}
            }}),
        );
    }
    let protected_of = |snapshot: &Value, id: &str| -> Option<bool> {
        snapshot["monitors"]
            .as_array()
            .unwrap()
            .iter()
            .find(|monitor| monitor["id"] == id)
            .unwrap()["protected"]
            .as_bool()
    };
    let before = core.snapshot_value().unwrap();
    assert_eq!(protected_of(&before, "screen-a"), Some(true));
    assert_eq!(protected_of(&before, "screen-b"), Some(true));
    let disabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["screen-a"]}}),
    );
    assert_eq!(protected_of(&disabled, "screen-a"), Some(false));
    assert_eq!(protected_of(&disabled, "screen-b"), Some(true));
    let restored = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":[]}}),
    );
    assert_eq!(protected_of(&restored, "screen-a"), Some(true));
}

#[test]
fn protection_monitor_disabled_monitor_is_not_presence_dimmed() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for id in ["one", "two"] {
        send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
                "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
            }}),
        );
    }
    send(&mut core, json!({"type":"power","on_battery":false}));
    let mut observed = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"protection_disabled":["two"]}}),
    );
    for offset in [10_000, 20_000] {
        observed = send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":observed["camera"]["generation"]}),
        );
    }
    let monitor = |snapshot: &Value, id: &str| -> Value {
        snapshot["monitors"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["id"] == id)
            .unwrap()
            .clone()
    };
    let one = monitor(&observed, "one");
    let two = monitor(&observed, "two");
    let actions = observed["pending_actions"].as_array().unwrap();
    // Positive control: "one" dims on the second absent sample.
    assert_eq!(one["presence_dimmed"], true);
    let dim = actions
        .iter()
        .find(|a| a["monitor_id"] == "one")
        .expect("one must have a dim action");
    assert_eq!(dim["kind"], "set_brightness");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    // "two" is protection-disabled: no dim and no pending action.
    assert_eq!(two["presence_dimmed"], false);
    assert!(actions.iter().all(|a| a["monitor_id"] != "two"));
}

#[test]
fn protection_monitor_disabled_monitor_is_not_screensaver_dimmed() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for id in ["one", "two"] {
        send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
                "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
            }}),
        );
    }
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true,"protection_disabled":["two"]}}),
    );
    let saver = send(&mut core, json!({"type":"screensaver","state":"active"}));
    let monitor = |snapshot: &Value, id: &str| -> Value {
        snapshot["monitors"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["id"] == id)
            .unwrap()
            .clone()
    };
    let one = monitor(&saver, "one");
    let two = monitor(&saver, "two");
    let actions = saver["pending_actions"].as_array().unwrap();
    // Positive control: "one" is screensaver-dimmed.
    assert_eq!(saver["screensaver"]["state"], "active");
    assert!(one["brightness"]["reasons"]
        .as_array()
        .unwrap()
        .iter()
        .any(|reason| reason == "screensaver"));
    assert!(actions
        .iter()
        .any(|a| a["monitor_id"] == "one" && a["kind"] == "set_brightness"));
    // "two" is protection-disabled: no screensaver reason and no write.
    assert!(!two["brightness"]["reasons"]
        .as_array()
        .unwrap()
        .iter()
        .any(|reason| reason == "screensaver"));
    assert!(actions
        .iter()
        .all(|a| !(a["monitor_id"] == "two" && a["kind"] == "set_brightness")));
}

// Protection level: one fixed 1..=50 percent per screen, shared by presence and the screensaver.
// Stored targets outside that range are clamped when applied, never rejected.
fn protection_test_monitor(id: &str, raw: u32, max: u32) -> Value {
    json!({"type":"monitor_discovered","monitor":{
        "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
        "brightness":{"supported":true,"value":raw,"raw":raw,"min":0,"max":max},
        "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":max}
    }})
}

fn queued_set_brightness(snapshot: &Value, id: &str) -> Option<f64> {
    snapshot["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["monitor_id"] == id && a["kind"] == "set_brightness")
        .and_then(|a| a["value"].as_f64())
}

#[test]
fn protection_level_from_the_screen_target_applies_to_presence_too() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, protection_test_monitor("screen", 70, 100));
    send(&mut core, json!({"type":"power","on_battery":false}));
    let mut observed = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"screensaver_targets":{"screen":30.0}}}),
    );
    for offset in [10_000, 20_000] {
        observed = send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":observed["camera"]["generation"]}),
        );
    }
    // Presence dims to the screen's level of 30 percent, not to the raw minimum of 0.
    assert_eq!(observed["monitors"][0]["presence_dimmed"], true);
    assert_eq!(queued_set_brightness(&observed, "screen"), Some(30.0));
}

#[test]
fn protection_level_never_raises_a_screen_already_below_it() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, protection_test_monitor("screen", 20, 100));
    send(&mut core, json!({"type":"power","on_battery":false}));
    // The threshold is lowered to 10 so a monitor at 20 keeps driving the camera.
    let mut observed = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"presence_min_brightness_percent":10.0,"screensaver_targets":{"screen":30.0}}}),
    );
    for offset in [10_000, 20_000] {
        observed = send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":observed["camera"]["generation"]}),
        );
    }
    let screen = &observed["monitors"][0];
    assert_eq!(screen["presence_dimmed"], true);
    assert!(screen["brightness"]["reasons"]
        .as_array()
        .unwrap()
        .contains(&json!("presence")));
    // The baseline of 20 is below the level of 30, so the dim keeps 20 and never queues a raise.
    assert!(observed["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["monitor_id"] != "screen"
            || a["kind"] != "set_brightness"
            || a["value"].as_f64().unwrap() <= 20.0));
}

#[test]
fn stored_screen_targets_outside_the_range_are_clamped_when_applied() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, protection_test_monitor("low", 70, 100));
    send(&mut core, protection_test_monitor("high", 70, 100));
    send(&mut core, json!({"type":"power","on_battery":false}));
    // Validation still accepts 0 and 75, so a settings file written before the range existed loads.
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true,"screensaver_targets":{"low":0.0,"high":75.0}}}),
    );
    let saver = send(&mut core, json!({"type":"screensaver","state":"active"}));
    // Stored 0 is applied as the 1 percent floor and stored 75 as the 50 percent ceiling.
    assert_eq!(queued_set_brightness(&saver, "low"), Some(1.0));
    assert_eq!(queued_set_brightness(&saver, "high"), Some(50.0));
}

#[test]
fn protection_level_is_a_whole_raw_unit_on_a_255_step_range() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, protection_test_monitor("default", 200, 255));
    send(&mut core, protection_test_monitor("half", 200, 255));
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"screensaver_protection_enabled":true,"screensaver_targets":{"half":50.0}}}),
    );
    let saver = send(&mut core, json!({"type":"screensaver","state":"active"}));
    // 1 percent of 0..=255 is 2.55 and 50 percent is 127.5; both round to whole raw units.
    assert_eq!(queued_set_brightness(&saver, "default"), Some(3.0));
    assert_eq!(queued_set_brightness(&saver, "half"), Some(128.0));
}

#[test]
fn protection_monitor_toggle_off_while_dimmed_restores_only_that_monitor() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for id in ["one", "two"] {
        send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
                "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
            }}),
        );
    }
    send(&mut core, json!({"type":"power","on_battery":false}));
    let mut observed = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    for offset in [10_000, 20_000] {
        observed = send(
            &mut core,
            json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(offset),"camera_generation":observed["camera"]["generation"]}),
        );
    }
    let monitor = |snapshot: &Value, id: &str| -> Value {
        snapshot["monitors"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["id"] == id)
            .unwrap()
            .clone()
    };
    // Positive control: both monitors dim and are acknowledged.
    for id in ["one", "two"] {
        let dim = observed["pending_actions"]
            .as_array()
            .unwrap()
            .iter()
            .find(|a| a["monitor_id"] == id && a["kind"] == "set_brightness")
            .expect("both monitors must dim")
            .clone();
        assert_eq!(dim["value"].as_f64(), Some(1.0));
        observed = send(
            &mut core,
            json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":1}),
        );
    }
    let dimmed_one = monitor(&observed, "one");
    let dimmed_two = monitor(&observed, "two");
    assert_eq!(dimmed_one["presence_dimmed"], true);
    assert_eq!(dimmed_two["presence_dimmed"], true);
    let toggled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["two"]}}),
    );
    let actions = toggled["pending_actions"].as_array().unwrap();
    // "two" verifies ownership with a readback before any restoring write.
    let read = actions
        .iter()
        .find(|a| a["monitor_id"] == "two")
        .expect("unprotected monitor must read back first");
    assert_eq!(read["kind"], "read_brightness");
    assert_eq!(read["target"].as_f64(), Some(70.0));
    assert!(actions
        .iter()
        .all(|a| !(a["monitor_id"] == "two" && a["kind"] == "set_brightness")));
    // "one" stays dimmed and gets no restoring write.
    let still_one = monitor(&toggled, "one");
    let still_two = monitor(&toggled, "two");
    assert_eq!(still_one["presence_dimmed"], true);
    assert_eq!(still_two["presence_dimmed"], false);
    assert!(actions
        .iter()
        .all(|a| !(a["monitor_id"] == "one" && a["kind"] == "set_brightness")));
    let readback = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":1}),
    );
    let restore = readback["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["monitor_id"] == "two" && a["kind"] == "set_brightness")
        .expect("owned dim must restore after readback");
    assert_eq!(restore["value"].as_f64(), Some(70.0));
    assert!(readback["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| !(a["monitor_id"] == "one" && a["kind"] == "set_brightness")));
}

#[test]
fn protection_monitor_reenabled_needs_two_fresh_misses_before_dimming() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    for id in ["one", "two"] {
        send(
            &mut core,
            json!({"type":"monitor_discovered","monitor":{
                "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
                "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
                "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
            }}),
        );
    }
    send(&mut core, json!({"type":"power","on_battery":false}));
    let enabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    // One miss for both monitors.
    send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(10_000),"camera_generation":enabled["camera"]["generation"]}),
    );
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["two"]}}),
    );
    let reenabled = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":[]}}),
    );
    let monitor = |snapshot: &Value, id: &str| -> Value {
        snapshot["monitors"]
            .as_array()
            .unwrap()
            .iter()
            .find(|m| m["id"] == id)
            .unwrap()
            .clone()
    };
    // Positive control: "one" reaches its second miss and dims.
    let second = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(20_000),"camera_generation":reenabled["camera"]["generation"]}),
    );
    let one = monitor(&second, "one");
    assert_eq!(one["presence_dimmed"], true);
    // "two" counted from zero again, so one miss is not enough.
    let two = monitor(&second, "two");
    assert_eq!(two["presence_dimmed"], false);
    assert!(second["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| !(a["monitor_id"] == "two" && a["kind"] == "set_brightness")));
    let third = send(
        &mut core,
        json!({"type":"presence_observation","status":"absent","captured_at_ms":capture_time(30_000),"camera_generation":second["camera"]["generation"]}),
    );
    let two_after = monitor(&third, "two");
    assert_eq!(two_after["presence_dimmed"], true);
}

#[test]
fn protection_monitor_unprotected_only_camera_matches_no_monitor_state() {
    let mut empty = Core::new(CoreOptions::default()).unwrap();
    send(&mut empty, json!({"type":"power","on_battery":false}));
    let none = send(
        &mut empty,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"always_on_camera":false}}),
    );
    assert_eq!(none["camera"]["mode"], "suspended");
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"one","name":"one","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    // Positive control: a protected monitor keeps the camera in face mode.
    let protected = send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true,"always_on_camera":false}}),
    );
    assert_eq!(protected["camera"]["mode"], "face");
    let unprotected = send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["one"]}}),
    );
    assert_eq!(unprotected["camera"]["mode"], none["camera"]["mode"]);
    assert_eq!(unprotected["camera"]["status"], none["camera"]["status"]);
    assert_eq!(
        unprotected["camera"]["display_status"],
        none["camera"]["display_status"]
    );
    assert_eq!(
        unprotected["camera"]["capture_mode"],
        none["camera"]["capture_mode"]
    );
    // Adding a protected monitor makes the camera eligible again.
    let added = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"two","name":"two","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    assert_eq!(added["camera"]["mode"], "face");
    assert_eq!(added["camera"]["capture_mode"], "single_frame");
}

fn follow_curve() -> Value {
    json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]})
}

fn follow_discovery(id: &str, min: f64, max: f64, raw: f64) -> Value {
    json!({"type":"monitor_discovered","monitor":{
        "id":id,"name":id,"identity_status":"stable","connection_path":"usb",
        "brightness":{"supported":true,"value":raw,"raw":raw,"min":min,"max":max},
        "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
    }})
}

// Fresh core with one calibrated monitor "screen" and follow switched on.
fn follow_core(min: f64, max: f64, raw: f64) -> Core {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, follow_discovery("screen", min, max, raw));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"screen":follow_curve()}}}),
    );
    core
}

fn follow_builtin(core: &mut Core, brightness: Value, calibrating: bool) -> Value {
    send(
        core,
        json!({"type":"builtin_display","available":true,"brightness":brightness,"calibrating":calibrating}),
    )
}

fn follow_pending(snapshot: &Value, monitor_id: &str, kind: &str) -> Option<Value> {
    snapshot["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .find(|a| a["monitor_id"] == monitor_id && a["kind"] == kind)
        .cloned()
}

fn follow_monitor<'a>(snapshot: &'a Value, id: &str) -> &'a Value {
    snapshot["monitors"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["id"] == id)
        .expect("monitor is connected")
}

fn follow_ack(core: &mut Core, action: &Value, value: f64) -> Value {
    send(
        core,
        json!({"type":"hardware_result","action_id":action["id"],"generation":action["generation"],"ok":true,"value":value}),
    )
}

fn follow_presence(core: &mut Core, status: &str, offset: u64, snapshot: &Value) -> Value {
    send(
        core,
        json!({"type":"presence_observation","status":status,"captured_at_ms":capture_time(offset),"camera_generation":snapshot["camera"]["generation"]}),
    )
}

fn follow_assert_rejected(core: &mut Core, event: Value) {
    let before = core.snapshot_value().unwrap();
    let bytes = serde_json::to_vec(&event).unwrap();
    assert!(core.submit_json(&bytes).is_err(), "must reject {}", event);
    let after = core.snapshot_value().unwrap();
    assert_eq!(after["revision"], before["revision"]);
    assert_eq!(after["ambient"], before["ambient"]);
}

// Raw value of the set_brightness emitted by one built-in change on a fresh core.
fn follow_first_write(min: f64, max: f64, raw: f64, brightness: f64) -> Option<f64> {
    let mut core = follow_core(min, max, raw);
    let s = follow_builtin(&mut core, json!(brightness), false);
    follow_pending(&s, "screen", "set_brightness").and_then(|action| action["value"].as_f64())
}

// Like follow_first_write, for a curve other than the shared follow_curve() fixture.
fn follow_first_write_for(
    curve: Value,
    min: f64,
    max: f64,
    raw: f64,
    brightness: f64,
) -> Option<f64> {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, follow_discovery("screen", min, max, raw));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"screen":curve}}}),
    );
    let s = follow_builtin(&mut core, json!(brightness), false);
    follow_pending(&s, "screen", "set_brightness").and_then(|action| action["value"].as_f64())
}

#[test]
fn follow_curve_maps_anchor_percents_to_exact_raw_targets() {
    // The monitor starts at 90, so no anchor target equals its current raw value.
    // MacBook levels 20, 45, 70 and 90 are the 25, 50, 75 and 100 percent points of the fixture.
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 20.0), Some(25.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 45.0), Some(50.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 70.0), Some(75.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 90.0), Some(100.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 0.0), Some(0.0));
}

#[test]
fn follow_curve_interpolates_linearly_between_anchors_and_from_origin() {
    // Between the origin and the 25 percent point: MacBook 4 and 8 are monitor 5 and 10 percent.
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 4.0), Some(5.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 8.0), Some(10.0));
    // MacBook 10 is 12.5 percent: raw 25 on a 0..200 range.
    assert_eq!(follow_first_write(0.0, 200.0, 90.0, 10.0), Some(25.0));
    // Between the 25 and 50, 50 and 75, and 75 and 100 percent points.
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 30.0), Some(35.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 60.0), Some(65.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 78.0), Some(85.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 86.0), Some(95.0));
}

#[test]
fn follow_curve_without_low_end_points_keeps_the_origin_segment() {
    // Without points below 25 percent the origin segment is unchanged:
    // MacBook 4 and 8 are monitor 5 and 10 percent.
    let curve = json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]});
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 100.0, 90.0, 4.0),
        Some(5.0)
    );
    assert_eq!(
        follow_first_write_for(curve, 0.0, 100.0, 90.0, 8.0),
        Some(10.0)
    );
}

#[test]
fn follow_curve_optional_low_points_refine_the_low_end() {
    // With 5, 10, 15 and 20 percent points added to the fixture, the low end is piecewise linear
    // through (0, 0), (4, 5), (8, 10), (12, 15), (16, 20) and (20, 25).
    // A 0..200 raw range makes every target exact: raw is twice the monitor percent.
    let curve = json!({"points":[{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":12.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]});
    // MacBook 6 is monitor 7.5 percent: raw 15.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 6.0),
        Some(15.0)
    );
    // MacBook 12 is monitor 15 percent: raw 30.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 12.0),
        Some(30.0)
    );
    // MacBook 18 is monitor 22.5 percent: raw 45.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 18.0),
        Some(45.0)
    );
    // MacBook 20 is monitor 25 percent: raw 50.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 20.0),
        Some(50.0)
    );
    // MacBook 2 is monitor 2.5 percent, linear from the origin to (4, 5): raw 5.
    assert_eq!(
        follow_first_write_for(curve, 0.0, 200.0, 90.0, 2.0),
        Some(5.0)
    );
}

#[test]
fn follow_curve_one_percent_point_refines_the_origin_segment() {
    // With a 1 percent point at MacBook 2 added to the refined fixture, the origin segment runs
    // through (0, 0), (2, 1) and (4, 5). A 0..200 raw range makes every target exact: raw is
    // twice the monitor percent.
    let curve = json!({"points":[{"monitor":1.0,"builtin":2.0},{"monitor":5.0,"builtin":4.0},{"monitor":10.0,"builtin":8.0},{"monitor":15.0,"builtin":12.0},{"monitor":20.0,"builtin":16.0},{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]});
    // MacBook 1 is monitor 0.5 percent, linear from the origin to (2, 1): raw 1.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 1.0),
        Some(1.0)
    );
    // MacBook 2 is monitor 1 percent: raw 2.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 2.0),
        Some(2.0)
    );
    // MacBook 3 is monitor 3 percent, between (2, 1) and (4, 5): 1 + (3 - 2) * 4 / 2 = 3, raw 6.
    assert_eq!(
        follow_first_write_for(curve, 0.0, 200.0, 90.0, 3.0),
        Some(6.0)
    );
}

#[test]
fn follow_curve_rounds_fractional_targets_to_whole_raw_units() {
    // 10 maps to 12.5 (rounds away from zero to 13), 11 to 13.75 and 13 to 16.25.
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 10.0), Some(13.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 11.0), Some(14.0));
    assert_eq!(follow_first_write(0.0, 100.0, 90.0, 13.0), Some(16.0));
}

#[test]
fn follow_curve_maps_through_a_0_to_255_raw_range() {
    // MacBook 35 is monitor 40 percent: raw 102.0 on 0..255.
    // MacBook 12 is monitor 15 percent: raw 38.25, rounded to 38.
    assert_eq!(follow_first_write(0.0, 255.0, 100.0, 100.0), Some(255.0));
    assert_eq!(follow_first_write(0.0, 255.0, 100.0, 0.0), Some(0.0));
    assert_eq!(follow_first_write(0.0, 255.0, 100.0, 35.0), Some(102.0));
    assert_eq!(follow_first_write(0.0, 255.0, 100.0, 12.0), Some(38.0));
}

#[test]
fn follow_curve_maps_through_a_monitor_minimum_above_zero() {
    // MacBook 0, 20, 45 and 100 are monitor 0, 25, 50 and 100 percent: raw 20, 45, 70 and 120
    // on a 20..=120 range, where raw 20 is the monitor minimum.
    assert_eq!(follow_first_write(20.0, 120.0, 100.0, 0.0), Some(20.0));
    assert_eq!(follow_first_write(20.0, 120.0, 100.0, 20.0), Some(45.0));
    assert_eq!(follow_first_write(20.0, 120.0, 100.0, 45.0), Some(70.0));
    assert_eq!(follow_first_write(20.0, 120.0, 100.0, 100.0), Some(120.0));
}

#[test]
fn follow_curve_with_zero_builtin_first_point_returns_25_percent_at_level_zero_without_nan() {
    // A first point with builtin 0 makes the first segment zero-width: level 0 returns its upper
    // point, 25 percent.
    let curve = json!({"points":[{"monitor":25.0,"builtin":0.0},{"monitor":50.0,"builtin":50.0},{"monitor":75.0,"builtin":75.0},{"monitor":100.0,"builtin":100.0}]});
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 100.0, 90.0, 0.0),
        Some(25.0)
    );
    // Levels above it still interpolate from that point: MacBook 10 is monitor 30 percent.
    assert_eq!(
        follow_first_write_for(curve, 0.0, 100.0, 90.0, 10.0),
        Some(30.0)
    );
}

#[test]
fn follow_curve_with_equal_consecutive_points_is_finite_and_non_decreasing() {
    // The 50 and 75 percent points share builtin 60, which makes the segment at level 60
    // zero-width. The monitor starts at raw 62, which no target reaches on this curve, so every
    // level produces a write.
    let curve = json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":60.0},{"monitor":75.0,"builtin":60.0},{"monitor":100.0,"builtin":100.0}]});
    let mut previous = f64::NEG_INFINITY;
    for level in 0..=100_u32 {
        let level = f64::from(level);
        let target = follow_first_write_for(curve.clone(), 0.0, 100.0, 62.0, level)
            .unwrap_or_else(|| panic!("no write at level {level}"));
        assert!(target.is_finite(), "level {level} gave {target}");
        assert!(
            target >= previous,
            "level {level} gave {target} after {previous}"
        );
        previous = target;
    }
}

#[test]
fn follow_curve_reaches_100_percent_at_a_last_point_below_100_builtin_and_holds_it() {
    // The 100 percent point is at MacBook 80: that level already reaches monitor 100 percent,
    // which is raw 255 on 0..255, and levels above it hold the same value.
    let curve = json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":80.0}]});
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 255.0, 100.0, 80.0),
        Some(255.0)
    );
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 255.0, 100.0, 85.0),
        Some(255.0)
    );
    assert_eq!(
        follow_first_write_for(curve, 0.0, 255.0, 100.0, 100.0),
        Some(255.0)
    );
}

#[test]
fn follow_curve_above_a_last_point_below_100_stays_at_its_monitor_percent() {
    // The last point is monitor 80 at MacBook 90. Above it the monitor holds 80 percent, which is
    // raw 204 on 0..255, instead of jumping to 100 percent.
    let curve = json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":80.0,"builtin":90.0}]});
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 255.0, 100.0, 90.0),
        Some(204.0)
    );
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 255.0, 100.0, 95.0),
        Some(204.0)
    );
    assert_eq!(
        follow_first_write_for(curve, 0.0, 255.0, 100.0, 100.0),
        Some(204.0)
    );
}

#[test]
fn follow_curve_with_exactly_two_points_is_eligible_and_maps_through_both() {
    // Two points, 25 percent at MacBook 20 and 100 percent at MacBook 90, are a complete curve.
    // A 0..200 raw range makes every target exact: raw is twice the monitor percent.
    let curve =
        json!({"points":[{"monitor":25.0,"builtin":20.0},{"monitor":100.0,"builtin":90.0}]});
    // MacBook 20 is monitor 25 percent: raw 50.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 20.0),
        Some(50.0)
    );
    // MacBook 55 is between the points, monitor 62.5 percent: raw 125.
    assert_eq!(
        follow_first_write_for(curve.clone(), 0.0, 200.0, 90.0, 55.0),
        Some(125.0)
    );
    // MacBook 90 is monitor 100 percent: raw 200, and levels above it hold there.
    assert_eq!(
        follow_first_write_for(curve, 0.0, 200.0, 90.0, 100.0),
        Some(200.0)
    );
}

#[test]
fn follow_builtin_display_reports_ambient_state_on_success() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let before = core.snapshot_value().unwrap()["revision"].as_u64().unwrap();
    let s = follow_builtin(&mut core, json!(44.0), false);
    assert!(s["revision"].as_u64().unwrap() > before);
    assert_eq!(s["ambient"]["available"].as_bool(), Some(true));
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(44.0));
    assert_eq!(s["ambient"]["calibrating"].as_bool(), Some(false));
    // Follow is still off, so an available built-in display alone is not active.
    assert_eq!(s["ambient"]["enabled"].as_bool(), Some(false));
    assert_eq!(s["ambient"]["active"].as_bool(), Some(false));
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true}}),
    );
    assert_eq!(s["ambient"]["active"].as_bool(), Some(true));
    // An unknown level is accepted and reported as null.
    let s = follow_builtin(&mut core, Value::Null, true);
    assert_eq!(s["ambient"]["builtin_percent"], Value::Null);
    assert_eq!(s["ambient"]["calibrating"].as_bool(), Some(true));
    assert_eq!(s["ambient"]["active"].as_bool(), Some(true));
    let s = send(
        &mut core,
        json!({"type":"builtin_display","available":false,"brightness":44.0,"calibrating":false}),
    );
    assert_eq!(s["ambient"]["available"].as_bool(), Some(false));
    assert_eq!(s["ambient"]["active"].as_bool(), Some(false));
}

#[test]
fn follow_builtin_display_rejects_brightness_outside_0_to_100_without_mutation() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    // Positive control: a valid report is accepted before the invalid ones are tried.
    let s = follow_builtin(&mut core, json!(50.0), false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(50.0));
    for brightness in [-1.0, 101.0] {
        follow_assert_rejected(
            &mut core,
            json!({"type":"builtin_display","available":true,"brightness":brightness,"calibrating":false}),
        );
    }
    let s = follow_builtin(&mut core, json!(100.0), false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(100.0));
    let s = follow_builtin(&mut core, json!(0.0), false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(0.0));
}

#[test]
fn follow_builtin_display_rejects_wrong_types_and_bad_keys_without_mutation() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    // Positive control: a valid report is accepted before the invalid ones are tried.
    let s = follow_builtin(&mut core, json!(44.0), false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(44.0));
    follow_assert_rejected(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":"44","calibrating":false}),
    );
    follow_assert_rejected(
        &mut core,
        json!({"type":"builtin_display","available":"yes","brightness":44.0,"calibrating":false}),
    );
    follow_assert_rejected(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":44.0,"calibrating":false,"source":"shell"}),
    );
    follow_assert_rejected(
        &mut core,
        json!({"type":"builtin_display","brightness":44.0,"calibrating":false}),
    );
    follow_assert_rejected(
        &mut core,
        json!({"type":"builtin_display","available":true,"brightness":44.0}),
    );
}

#[test]
fn follow_writes_only_to_calibrated_supported_monitors() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    send(&mut core, follow_discovery("screen", 0.0, 100.0, 90.0));
    send(&mut core, follow_discovery("bare", 0.0, 100.0, 90.0));
    send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"ddc-less","name":"ddc-less","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":false,"value":null,"raw":null,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true,"ambient_curves":{"screen":follow_curve()}}}),
    );
    let s = follow_builtin(&mut core, json!(50.0), false);
    // Positive control: the calibrated monitor is written.
    let write = follow_pending(&s, "screen", "set_brightness").expect("calibrated monitor follows");
    assert_eq!(write["value"].as_f64(), Some(55.0));
    assert_eq!(follow_monitor(&s, "bare")["follow"], "uncalibrated");
    assert_eq!(follow_monitor(&s, "ddc-less")["follow"], "unsupported");
    assert!(follow_pending(&s, "bare", "set_brightness").is_none());
    assert!(follow_pending(&s, "ddc-less", "set_brightness").is_none());
}

#[test]
fn follow_writes_do_not_set_manual_override_so_presence_still_dims() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("follow write");
    assert_eq!(write["value"].as_f64(), Some(55.0));
    let mut s = follow_ack(&mut core, &write, 55.0);
    s = follow_presence(&mut core, "absent", 10_000, &s);
    s = follow_presence(&mut core, "absent", 20_000, &s);
    assert_eq!(follow_monitor(&s, "screen")["presence_dimmed"], true);
    // The owned monitor is read back before the dim write.
    let read = follow_pending(&s, "screen", "read_brightness").expect("dim verifies ownership");
    s = follow_ack(&mut core, &read, 55.0);
    let dim = follow_pending(&s, "screen", "set_brightness").expect("presence still dims");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
}

#[test]
fn follow_dimmed_monitor_defers_builtin_changes_and_restores_the_new_mapped_value() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    send(&mut core, json!({"type":"power","on_battery":false}));
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_protection_enabled":true}}),
    );
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("follow write");
    let mut s = follow_ack(&mut core, &write, 55.0);
    s = follow_presence(&mut core, "absent", 10_000, &s);
    s = follow_presence(&mut core, "absent", 20_000, &s);
    let read = follow_pending(&s, "screen", "read_brightness").expect("dim verifies ownership");
    s = follow_ack(&mut core, &read, 55.0);
    let dim = follow_pending(&s, "screen", "set_brightness").expect("presence dim");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
    s = follow_ack(&mut core, &dim, 1.0);
    assert_eq!(follow_monitor(&s, "screen")["presence_dimmed"], true);
    // While dimmed, a built-in change only moves the baseline: nothing is queued.
    let s = follow_builtin(&mut core, json!(75.0), false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(75.0));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // Presence returns: read back, then restore the new mapped value (81), not the old 55.
    let mut s = follow_presence(&mut core, "present", 30_000, &s);
    let read = follow_pending(&s, "screen", "read_brightness").expect("restore verifies ownership");
    s = follow_ack(&mut core, &read, 1.0);
    let restore =
        follow_pending(&s, "screen", "set_brightness").expect("restore after presence returns");
    assert_eq!(restore["value"].as_f64(), Some(81.0));
}

#[test]
fn follow_asleep_monitor_defers_then_reads_before_writing_the_mapped_value() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let generation = core.snapshot_value().unwrap()["monitors"][0]["generation"].clone();
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let s = follow_builtin(&mut core, json!(50.0), false);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(50.0));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    let s = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read =
        follow_pending(&s, "screen", "read_brightness").expect("wake verifies ownership first");
    assert!(follow_pending(&s, "screen", "set_brightness").is_none());
    let s = follow_ack(&mut core, &read, 90.0);
    let write = follow_pending(&s, "screen", "set_brightness").expect("mapped value after wake");
    assert_eq!(write["value"].as_f64(), Some(55.0));
}

#[test]
fn follow_dead_band_ignores_targets_within_one_raw_unit_of_desired() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("first write");
    let s = follow_ack(&mut core, &write, 55.0);
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // 50.2 maps to 55.2 on the 45..70 segment, which rounds to the current raw value 55.
    let s = follow_builtin(&mut core, json!(50.2), false);
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // Positive control: a real change reads the owned monitor back, then writes 81.
    let s = follow_builtin(&mut core, json!(75.0), false);
    let read =
        follow_pending(&s, "screen", "read_brightness").expect("owned monitor verifies first");
    let s = follow_ack(&mut core, &read, 55.0);
    let write = follow_pending(&s, "screen", "set_brightness").expect("a real change writes");
    assert_eq!(write["value"].as_f64(), Some(81.0));
}

#[test]
fn follow_calibrating_pauses_writes_until_calibration_ends() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let s = follow_builtin(&mut core, json!(50.0), true);
    assert_eq!(s["ambient"]["calibrating"].as_bool(), Some(true));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    let s = follow_builtin(&mut core, json!(75.0), true);
    assert_eq!(s["ambient"]["builtin_percent"].as_f64(), Some(75.0));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    let s = follow_builtin(&mut core, json!(75.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("calibration ended");
    assert_eq!(write["value"].as_f64(), Some(81.0));
}

#[test]
fn follow_stops_when_builtin_is_unavailable_or_follow_is_disabled() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let s = send(
        &mut core,
        json!({"type":"builtin_display","available":false,"brightness":50.0,"calibrating":false}),
    );
    assert_eq!(s["ambient"]["active"].as_bool(), Some(false));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // Positive control: the same level writes once the built-in display is available.
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("available builtin writes");
    assert_eq!(write["value"].as_f64(), Some(55.0));
    follow_ack(&mut core, &write, 55.0);
    // Follow off: a new level is not written and the monitor keeps its last value.
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":false}}),
    );
    assert_eq!(s["ambient"]["active"].as_bool(), Some(false));
    let s = follow_builtin(&mut core, json!(25.0), false);
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // Positive control: follow back on reads the owned monitor, then writes 30.
    let s = send(
        &mut core,
        json!({"type":"update_settings","settings":{"ambient_follow_enabled":true}}),
    );
    let read =
        follow_pending(&s, "screen", "read_brightness").expect("owned monitor verifies first");
    let s = follow_ack(&mut core, &read, 55.0);
    let write = follow_pending(&s, "screen", "set_brightness").expect("re-enabled follow writes");
    assert_eq!(write["value"].as_f64(), Some(30.0));
}

#[test]
fn follow_pauses_while_builtin_is_unavailable_and_resumes_with_the_current_level() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("available builtin writes");
    assert_eq!(write["value"].as_f64(), Some(55.0));
    follow_ack(&mut core, &write, 55.0);
    // Built-in unavailable: follow pauses, queues nothing and keeps the monitor eligible.
    let s = send(
        &mut core,
        json!({"type":"builtin_display","available":false,"brightness":50.0,"calibrating":false}),
    );
    assert_eq!(s["ambient"]["active"].as_bool(), Some(false));
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    assert_eq!(follow_monitor(&s, "screen")["follow"], "eligible");
    // Manual control still writes while paused.
    let generation = follow_monitor(&s, "screen")["generation"].clone();
    let s = send(
        &mut core,
        json!({"type":"set_brightness","monitor_id":"screen","value":20,"connection_generation":generation}),
    );
    let manual = follow_pending(&s, "screen", "set_brightness").expect("manual write");
    assert_eq!(manual["value"].as_f64(), Some(20.0));
    follow_ack(&mut core, &manual, 20.0);
    // Built-in back at 75: follow resumes and replaces the manual 20 with the mapped value.
    let mut s = follow_builtin(&mut core, json!(75.0), false);
    assert_eq!(s["ambient"]["active"].as_bool(), Some(true));
    if let Some(read) = follow_pending(&s, "screen", "read_brightness") {
        s = follow_ack(&mut core, &read, 20.0);
    }
    // MacBook 75 is monitor 81.25 percent, which rounds to raw 81 on the 0..100 range.
    let write = follow_pending(&s, "screen", "set_brightness").expect("follow resumes");
    assert_eq!(write["value"].as_f64(), Some(81.0));
}

#[test]
fn follow_reconnect_reapplies_the_curve_only_to_the_same_monitor_id() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("first write");
    follow_ack(&mut core, &write, 55.0);
    send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"screen"}),
    );
    let s = send(&mut core, follow_discovery("screen", 0.0, 100.0, 90.0));
    let write = follow_pending(&s, "screen", "set_brightness").expect("same ID keeps its curve");
    assert_eq!(write["value"].as_f64(), Some(55.0));
    let s = send(&mut core, follow_discovery("screen-2", 0.0, 100.0, 90.0));
    assert_eq!(follow_monitor(&s, "screen-2")["follow"], "uncalibrated");
    assert!(follow_pending(&s, "screen-2", "set_brightness").is_none());
}

#[test]
fn follow_null_builtin_brightness_emits_no_write() {
    let mut core = follow_core(0.0, 100.0, 90.0);
    let s = follow_builtin(&mut core, Value::Null, false);
    assert_eq!(s["ambient"]["available"].as_bool(), Some(true));
    assert_eq!(s["ambient"]["builtin_percent"], Value::Null);
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
    // Positive control: a known level writes.
    let s = follow_builtin(&mut core, json!(50.0), false);
    let write = follow_pending(&s, "screen", "set_brightness").expect("known level writes");
    assert_eq!(write["value"].as_f64(), Some(55.0));
    follow_ack(&mut core, &write, 55.0);
    // An unknown level after a known one must not be treated as 0.
    let s = follow_builtin(&mut core, Value::Null, false);
    assert_eq!(s["ambient"]["builtin_percent"], Value::Null);
    assert!(s["pending_actions"].as_array().unwrap().is_empty());
}

#[test]
fn first_miss_shortens_the_wait_for_the_confirming_check() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    let s = enable_presence(&mut core);
    assert_eq!(s["camera"]["mode"], "face");
    // The first sample after enabling is a short resume sample; a present result moves to the presence interval.
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    let s = presence_sample(&mut core, "present", 10_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(10000.0));
    // First miss: the next check is the short return interval, and nothing dims yet.
    let s = presence_sample(&mut core, "absent", 20_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    assert!(s["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
    // Confirming miss: the monitor dims and the short interval continues.
    let s = presence_sample(&mut core, "absent", 30_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    assert_eq!(s["monitors"][0]["presence_dimmed"], true);
    let dim = pending_action(&s, "set_brightness");
    assert_eq!(dim["value"].as_f64(), Some(1.0));
}

#[test]
fn present_after_the_first_miss_restores_the_presence_interval() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    enable_presence(&mut core);
    let s = presence_sample(&mut core, "absent", 10_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    // Presence clears the miss count and returns to the presence interval.
    let s = presence_sample(&mut core, "present", 20_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(10000.0));
    // A single new miss is not a confirmed absence, so nothing dims.
    let s = presence_sample(&mut core, "absent", 30_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
    assert!(s["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
}

#[test]
fn confirmation_cadence_follows_the_return_interval_not_the_presence_interval() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    enable_presence(&mut core);
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"presence_sampling_interval_s":60,"return_sampling_interval_s":2}}),
    );
    // A present sample ends the resume state, so the configured presence interval applies.
    let s = presence_sample(&mut core, "present", 10_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(60000.0));
    let s = presence_sample(&mut core, "absent", 20_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(2000.0));
}

#[test]
fn unprotected_monitor_misses_do_not_speed_up_sampling() {
    // Monitor "a" is protected; monitor "b" is excluded with protection_disabled.
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "a", json!(70));
    discover_monitor(&mut core, "b", json!(70));
    enable_presence(&mut core);
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["b"]}}),
    );
    // The short cadence comes from the protected monitor "a" only.
    let s = presence_sample(&mut core, "absent", 10_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    // Only the unprotected monitor "b" is left: the camera is suspended, so there is no
    // sampling cadence and misses cannot speed it up.
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "b", json!(70));
    enable_presence(&mut core);
    send(
        &mut core,
        json!({"type":"update_settings","settings":{"protection_disabled":["b"]}}),
    );
    let s = presence_sample(&mut core, "absent", 10_000);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["next_sample_in_ms"], Value::Null);
}

#[test]
fn insufficient_light_on_the_confirming_sample_clears_the_first_miss() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    enable_presence(&mut core);
    let s = presence_sample(&mut core, "absent", 10_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    // Low light clears the miss count and switches to light-only sampling.
    let s = presence_sample(&mut core, "insufficient_light", 20_000);
    assert_eq!(s["camera"]["mode"], "light_only");
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(10000.0));
    // Two usable-light samples switch back to face mode.
    let s = presence_sample(&mut core, "usable_light", 30_000);
    assert_eq!(s["camera"]["mode"], "light_only");
    let s = presence_sample(&mut core, "usable_light", 40_000);
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(10000.0));
    // The earlier miss was cleared, so one new absent does not dim the monitor.
    let s = presence_sample(&mut core, "absent", 50_000);
    assert_eq!(s["camera"]["next_sample_in_ms"].as_f64(), Some(1000.0));
    assert_eq!(s["monitors"][0]["presence_dimmed"], false);
    assert!(s["pending_actions"]
        .as_array()
        .unwrap()
        .iter()
        .all(|a| a["kind"] != "set_brightness"));
}

// Every monitor reference in a diagnostic line is the hashed `id-xxxxxxxx` form.
fn assert_hashed_monitor_references(lines: &[String]) {
    for line in lines {
        if let Some(reference) = line.split(" monitor=").nth(1) {
            let id = reference.split(' ').next().unwrap();
            let hex = id.strip_prefix("id-").unwrap_or_default();
            assert!(
                hex.len() == 8 && hex.chars().all(|c| c.is_ascii_hexdigit()),
                "unhashed monitor reference in {line:?}"
            );
        }
    }
}

#[test]
fn wake_restore_records_diagnostic_lines() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    // Only the wake sequence is checked below, so the setup lines are discarded here.
    core.drain_diagnostics();
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let read = pending_action(&awake, "read_brightness");
    let verified = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":true,"value":0}),
    );
    let restore = pending_action(&verified, "set_brightness");
    assert_eq!(restore["value"].as_f64(), Some(70.0));
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":restore["id"],"generation":restore["generation"],"ok":true,"value":70}),
    );

    let lines = core.drain_diagnostics();
    let events: Vec<&str> = lines
        .iter()
        .map(|line| line.split(' ').next().unwrap())
        .collect();
    let wanted = [
        "display_state",
        "read_result",
        "ownership_verified",
        "restore_enqueued",
    ];
    let mut next = 0;
    for event in events {
        if next < wanted.len() && event == wanted[next] {
            next += 1;
        }
    }
    assert_eq!(next, wanted.len(), "missing or out of order in {lines:?}");
    let has = |prefix: &str, suffix: &str| {
        lines
            .iter()
            .any(|line| line.starts_with(prefix) && line.ends_with(suffix))
    };
    assert!(has("display_state monitor=id-", " state=awake"));
    assert!(has("read_result monitor=id-", " ok=true value=0"));
    assert!(has("ownership_verified monitor=id-", " raw=0"));
    assert!(has(
        "restore_enqueued monitor=id-",
        " target=70 baseline=70"
    ));
    assert!(lines.iter().all(|line| !line.contains("screen")));
    assert_hashed_monitor_references(&lines);
}

#[test]
fn exhausted_restore_records_ownership_dropped() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"screen","name":"Display","identity_status":"stable","connection_path":"usb",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"screen","generation":generation,"state":"awake"}),
    );
    let mut read = pending_action(&awake, "read_brightness");
    // Three failed readbacks each record a restore failure while ownership is kept.
    for _ in 0..3 {
        let failed = send(
            &mut core,
            json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
        );
        read = pending_action(&failed, "read_brightness");
    }
    // The fourth failure exhausts the retries and drops ownership.
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
    );

    let lines = core.drain_diagnostics();
    let failures: Vec<&String> = lines
        .iter()
        .filter(|line| line.starts_with("restore_failure "))
        .collect();
    assert_eq!(failures.len(), 3, "{lines:?}");
    for (n, line) in failures.iter().enumerate() {
        assert!(
            line.starts_with("restore_failure monitor=id-")
                && line.ends_with(&format!(" failures={}/3", n + 1)),
            "{line}"
        );
    }
    let dropped: Vec<&String> = lines
        .iter()
        .filter(|line| line.starts_with("ownership_dropped "))
        .collect();
    assert_eq!(dropped.len(), 1, "{lines:?}");
    assert!(
        dropped[0].starts_with("ownership_dropped monitor=id-")
            && dropped[0].ends_with(" reason=restore_failed"),
        "{}",
        dropped[0]
    );
    assert!(lines
        .last()
        .is_some_and(|line| line.starts_with("ownership_dropped ")));
    assert_hashed_monitor_references(&lines);
}

#[test]
fn rediscovery_records_disconnect_and_discovery() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover_monitor(&mut core, "screen", json!(70));
    core.drain_diagnostics();
    send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"screen"}),
    );
    discover_monitor(&mut core, "screen", json!(40));

    let lines = core.drain_diagnostics();
    assert_eq!(lines.len(), 2, "{lines:?}");
    assert!(lines[0].starts_with("monitor_disconnected monitor=id-"));
    assert!(lines[1].starts_with("monitor_discovered monitor=id-"));
    assert!(lines[1].ends_with(" raw=40 baseline=40"));
    assert_hashed_monitor_references(&lines);
}

#[test]
fn diagnostic_lines_never_contain_the_monitor_id_name_or_path() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let discovered = send(
        &mut core,
        json!({"type":"monitor_discovered","monitor":{
            "id":"secret-serial-123","name":"My Secret Name","identity_status":"stable","connection_path":"usb-secret-path",
            "brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},
            "volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}
        }}),
    );
    send(&mut core, json!({"type":"power","on_battery":false}));
    let generation = discovered["monitors"][0]["generation"].clone();
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
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":dim["id"],"generation":dim["generation"],"ok":true,"value":0}),
    );
    send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"secret-serial-123","generation":generation,"state":"asleep"}),
    );
    let awake = send(
        &mut core,
        json!({"type":"monitor_display_state","monitor_id":"secret-serial-123","generation":generation,"state":"awake"}),
    );
    let read = pending_action(&awake, "read_brightness");
    let failed = send(
        &mut core,
        json!({"type":"hardware_result","action_id":read["id"],"generation":read["generation"],"ok":false,"error":"unsupported"}),
    );
    let retry = pending_action(&failed, "read_brightness");
    let verified = send(
        &mut core,
        json!({"type":"hardware_result","action_id":retry["id"],"generation":retry["generation"],"ok":true,"value":0}),
    );
    let restore = pending_action(&verified, "set_brightness");
    send(
        &mut core,
        json!({"type":"hardware_result","action_id":restore["id"],"generation":restore["generation"],"ok":true,"value":70}),
    );
    send(
        &mut core,
        json!({"type":"monitor_disconnected","monitor_id":"secret-serial-123"}),
    );

    let lines = core.drain_diagnostics();
    assert!(lines
        .iter()
        .any(|line| line.starts_with("monitor_discovered monitor=id-")));
    assert!(lines
        .iter()
        .any(|line| line.starts_with("restore_enqueued monitor=id-")));
    for line in &lines {
        for secret in ["secret-serial-123", "My Secret Name", "usb-secret-path"] {
            assert!(!line.contains(secret), "{line}");
        }
    }
    assert_hashed_monitor_references(&lines);
}
