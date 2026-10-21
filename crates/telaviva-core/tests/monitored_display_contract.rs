use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use telaviva_core::{Core, CoreOptions, Settings};

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

fn discover(core: &mut Core, monitor: Value) -> Value {
    send(
        core,
        json!({"type": "monitor_discovered", "monitor": monitor}),
    )
}

// `id` is a monitor id, "builtin", or null for Automatic.
fn select_monitored(core: &mut Core, id: Value) -> Value {
    send(
        core,
        json!({"type": "update_settings", "settings": {"monitored_display_id": id}}),
    )
}

fn set_builtin(core: &mut Core, available: bool, protectable: bool) -> Value {
    send(
        core,
        json!({
            "type": "builtin_display",
            "available": available,
            "brightness": 62,
            "calibrating": false,
            "protectable": protectable,
            "holding": false
        }),
    )
}

fn monitored_id(snapshot: &Value) -> Value {
    snapshot["monitored_display"]["id"].clone()
}

// Settings files are per test, so parallel tests never share a path.
fn temp_root(name: &str) -> PathBuf {
    let root =
        std::env::temp_dir().join(format!("telaviva-monitored-{name}-{}", std::process::id()));
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
fn default_monitored_display_is_automatic_with_an_empty_history() {
    let defaults = Settings::default();
    assert_eq!(defaults.monitored_display_id, None);
    assert!(defaults.monitored_display_history.is_empty());
    assert!(defaults.validate().is_ok());
    let core = Core::new(CoreOptions::default()).unwrap();
    let snapshot = core.snapshot_value().unwrap();
    assert!(snapshot["settings"]["monitored_display_id"].is_null());
    assert_eq!(snapshot["settings"]["monitored_display_history"], json!([]));
    assert_eq!(snapshot["monitored_display"]["automatic"], true);
    assert!(snapshot["monitored_display"]["id"].is_null());
}

#[test]
fn monitored_display_validation_mirrors_the_camera_history() {
    let with_history = |history: &[&str]| Settings {
        monitored_display_id: history.first().map(|id| id.to_string()),
        monitored_display_history: history.iter().map(|id| id.to_string()).collect(),
        ..Default::default()
    };
    assert!(with_history(&["m1", "builtin"]).validate().is_ok());
    assert!(with_history(&["m1", "m1"]).validate().is_err());
    assert!(with_history(&[""]).validate().is_err());
    let longest = "x".repeat(512);
    let oversized = "x".repeat(513);
    assert!(with_history(&[longest.as_str()]).validate().is_ok());
    assert!(with_history(&[oversized.as_str()]).validate().is_err());
    let ids: Vec<String> = (0..8).map(|i| format!("m{i}")).collect();
    let mut full = Settings {
        monitored_display_id: ids.first().cloned(),
        monitored_display_history: ids,
        ..Default::default()
    };
    assert!(full.validate().is_ok());
    full.monitored_display_history.push("m8".into());
    assert!(full.validate().is_err());
    let mut mismatch = with_history(&["m1", "m2"]);
    mismatch.monitored_display_id = Some("m2".into());
    assert!(mismatch.validate().is_err());
    mismatch.monitored_display_id = None;
    assert!(mismatch.validate().is_err());
}

#[test]
fn selecting_m1_then_m2_gives_history_m2_m1_with_m2_selected() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    select_monitored(&mut core, json!("m1"));
    let snapshot = select_monitored(&mut core, json!("m2"));
    assert_eq!(snapshot["settings"]["monitored_display_id"], "m2");
    assert_eq!(
        snapshot["settings"]["monitored_display_history"],
        json!(["m2", "m1"])
    );
    assert_eq!(snapshot["monitored_display"]["automatic"], false);
}

#[test]
fn reselecting_a_monitor_moves_it_to_the_front_without_duplicates() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    select_monitored(&mut core, json!("m1"));
    select_monitored(&mut core, json!("m2"));
    let snapshot = select_monitored(&mut core, json!("m1"));
    assert_eq!(snapshot["settings"]["monitored_display_id"], "m1");
    assert_eq!(
        snapshot["settings"]["monitored_display_history"],
        json!(["m1", "m2"])
    );
}

#[test]
fn ninth_distinct_selection_truncates_the_history_to_eight() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let mut last = Value::Null;
    for i in 0..9 {
        last = select_monitored(&mut core, json!(format!("m{i}")));
    }
    let expected: Vec<String> = (1..=8).rev().map(|i| format!("m{i}")).collect();
    assert_eq!(
        last["settings"]["monitored_display_history"],
        json!(expected)
    );
    assert_eq!(last["settings"]["monitored_display_id"], "m8");
}

#[test]
fn null_clears_the_selection_and_history_and_resolves_automatic() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_object("m1"));
    select_monitored(&mut core, json!("m1"));
    select_monitored(&mut core, json!("m2"));
    let cleared = select_monitored(&mut core, Value::Null);
    assert!(cleared["settings"]["monitored_display_id"].is_null());
    assert_eq!(cleared["settings"]["monitored_display_history"], json!([]));
    assert_eq!(cleared["monitored_display"]["automatic"], true);
    // Automatic still resolves to the connected external monitor.
    assert_eq!(monitored_id(&cleared), "m1");
}

#[test]
fn monitored_display_history_payload_is_read_only_and_leaves_the_selection_unchanged() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    select_monitored(&mut core, json!("m1"));
    let before = select_monitored(&mut core, json!("m2"));
    assert!(try_send(
        &mut core,
        json!({"type": "update_settings", "settings": {"monitored_display_history": ["x"]}})
    )
    .is_err());
    assert!(try_send(
        &mut core,
        json!({"type": "update_settings", "settings": {"monitored_display_id": "m3", "monitored_display_history": ["x"]}})
    )
    .is_err());
    let after = core.snapshot_value().unwrap();
    assert_eq!(
        after["settings"]["monitored_display_history"],
        json!(["m2", "m1"])
    );
    assert_eq!(after["settings"]["monitored_display_id"], "m2");
    assert_eq!(after["revision"], before["revision"]);
}

#[test]
fn monitored_display_id_rejects_non_strings_and_invalid_ids_without_changing_state() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    select_monitored(&mut core, json!("m1"));
    let before = core.snapshot_value().unwrap();
    let oversized = "x".repeat(513);
    for invalid in [json!(5), json!(""), json!(oversized)] {
        assert!(try_send(
            &mut core,
            json!({"type": "update_settings", "settings": {"monitored_display_id": invalid}})
        )
        .is_err());
    }
    let after = core.snapshot_value().unwrap();
    assert_eq!(after["settings"], before["settings"]);
    assert_eq!(after["revision"], before["revision"]);
}

#[test]
fn monitored_selection_does_not_bump_the_camera_generation() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_object("m1"));
    let base = send(
        &mut core,
        json!({"type": "update_settings", "settings": {"presence_protection_enabled": true}}),
    );
    let generation = base["camera"]["generation"].clone();
    let selected = select_monitored(&mut core, json!("m1"));
    assert_eq!(selected["camera"]["generation"], generation);
    let reselected = select_monitored(&mut core, json!("builtin"));
    assert_eq!(reselected["camera"]["generation"], generation);
    let cleared = select_monitored(&mut core, Value::Null);
    assert_eq!(cleared["camera"]["generation"], generation);
    // A camera setting still bumps the generation, so the exclusion is specific to this key.
    let camera_change = send(
        &mut core,
        json!({"type": "update_settings", "settings": {"presence_min_brightness_percent": 30}}),
    );
    assert_ne!(camera_change["camera"]["generation"], generation);
}

#[test]
fn settings_file_without_the_monitored_keys_loads_as_automatic() {
    let root = temp_root("old-file");
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"presence_protection_enabled":true}"#,
    )
    .unwrap();
    let core = persisted_core(&path);
    let settings = core.settings().unwrap();
    assert_eq!(settings.monitored_display_id, None);
    assert!(settings.monitored_display_history.is_empty());
    assert!(settings.presence_protection_enabled);
    assert!(path.exists());
    assert!(!root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn settings_history_front_must_equal_the_monitored_display_id() {
    assert!(Settings::from_json(
        br#"{"schema_version":1,"monitored_display_id":"m2","monitored_display_history":["m1","m2"]}"#
    )
    .is_err());
    assert!(Settings::from_json(
        br#"{"schema_version":1,"monitored_display_id":"m2","monitored_display_history":["m2","m1"]}"#
    )
    .is_ok());
    // Through the load path the invalid file is preserved as corrupt and defaults apply.
    let root = temp_root("front-mismatch");
    let path = root.join("settings.json");
    std::fs::write(
        &path,
        br#"{"schema_version":1,"monitored_display_id":"m2","monitored_display_history":["m1","m2"]}"#,
    )
    .unwrap();
    let core = persisted_core(&path);
    assert_eq!(core.settings().unwrap(), Settings::default());
    assert!(root.join("settings.json.corrupt").exists());
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn monitored_selection_round_trips_through_the_persisted_settings_file() {
    let root = temp_root("round-trip");
    let path = root.join("settings.json");
    let mut core = persisted_core(&path);
    select_monitored(&mut core, json!("m1"));
    select_monitored(&mut core, json!("m2"));
    let persisted: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
    assert_eq!(persisted["monitored_display_id"], "m2");
    assert_eq!(persisted["monitored_display_history"], json!(["m2", "m1"]));
    let reloaded = persisted_core(&path);
    assert_eq!(
        reloaded.settings().unwrap().monitored_display_history,
        vec!["m2", "m1"]
    );
    assert_eq!(
        reloaded.settings().unwrap().monitored_display_id.as_deref(),
        Some("m2")
    );
    let _ = std::fs::remove_dir_all(root);
}

#[test]
fn history_resolves_the_first_connected_entry_in_selection_order() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_object("m1"));
    select_monitored(&mut core, json!("m1"));
    // m2 is selected but not connected, so the history falls through to m1.
    let only_m1 = select_monitored(&mut core, json!("m2"));
    assert_eq!(monitored_id(&only_m1), "m1");
    assert_eq!(only_m1["monitored_display"]["automatic"], false);
    let both = discover(&mut core, monitor_object("m2"));
    assert_eq!(monitored_id(&both), "m2");
}

#[test]
fn disconnected_monitor_is_skipped_and_the_next_entry_resolves() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_object("m1"));
    discover(&mut core, monitor_object("m2"));
    select_monitored(&mut core, json!("m1"));
    let selected = select_monitored(&mut core, json!("m2"));
    assert_eq!(monitored_id(&selected), "m2");
    let after = send(
        &mut core,
        json!({"type": "monitor_disconnected", "monitor_id": "m2"}),
    );
    assert_eq!(monitored_id(&after), "m1");
    assert_eq!(after["settings"]["monitored_display_id"], "m2");
}

#[test]
fn builtin_entry_resolves_only_while_protectable_and_available() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_object("m1"));
    select_monitored(&mut core, json!("m1"));
    let before_report = select_monitored(&mut core, json!("builtin"));
    // The shell has not reported the built-in screen yet, so it is not usable.
    assert_eq!(monitored_id(&before_report), "m1");
    let usable = set_builtin(&mut core, true, true);
    assert_eq!(monitored_id(&usable), "builtin");
    assert_eq!(usable["monitored_display"]["automatic"], false);
    // A closed lid makes the built-in unavailable, so the next entry takes over.
    let lid_closed = set_builtin(&mut core, false, true);
    assert_eq!(monitored_id(&lid_closed), "m1");
    let not_protectable = set_builtin(&mut core, true, false);
    assert_eq!(monitored_id(&not_protectable), "m1");
    let back = set_builtin(&mut core, true, true);
    assert_eq!(monitored_id(&back), "builtin");
}

#[test]
fn builtin_only_selection_is_null_while_the_lid_is_closed() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    select_monitored(&mut core, json!("builtin"));
    let closed = set_builtin(&mut core, false, true);
    assert!(monitored_id(&closed).is_null());
    assert_eq!(closed["monitored_display"]["automatic"], false);
    let open = set_builtin(&mut core, true, true);
    assert_eq!(monitored_id(&open), "builtin");
}

#[test]
fn automatic_prefers_primary_then_first_external_then_builtin_then_none() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let empty = core.snapshot_value().unwrap();
    assert!(monitored_id(&empty).is_null());
    let builtin_only = set_builtin(&mut core, true, true);
    assert_eq!(monitored_id(&builtin_only), "builtin");
    // The first external by id, since none is flagged primary.
    discover(&mut core, monitor_object("m2"));
    let first_external = discover(&mut core, monitor_object("m1"));
    assert_eq!(monitored_id(&first_external), "m1");
    // The primary external wins even though m1 sorts first and the built-in is usable.
    let mut main = monitor_object("m2");
    main["primary"] = json!(true);
    let primary = discover(&mut core, main);
    assert_eq!(monitored_id(&primary), "m2");
    let without_primary = send(
        &mut core,
        json!({"type": "monitor_disconnected", "monitor_id": "m2"}),
    );
    assert_eq!(monitored_id(&without_primary), "m1");
    let without_externals = send(
        &mut core,
        json!({"type": "monitor_disconnected", "monitor_id": "m1"}),
    );
    assert_eq!(monitored_id(&without_externals), "builtin");
    let none = set_builtin(&mut core, true, false);
    assert!(monitored_id(&none).is_null());
}

#[test]
fn automatic_skips_a_primary_monitor_that_is_ambiguous_or_unsupported() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let mut ambiguous = monitor_object("a1");
    ambiguous["identity_status"] = json!("ambiguous");
    ambiguous["primary"] = json!(true);
    discover(&mut core, ambiguous);
    let mut unsupported = monitor_object("u1");
    unsupported["brightness"] =
        json!({"supported": false, "value": null, "raw": null, "min": 0, "max": 100});
    unsupported["primary"] = json!(true);
    discover(&mut core, unsupported);
    let snapshot = discover(&mut core, monitor_object("m1"));
    assert_eq!(monitored_id(&snapshot), "m1");
}

#[test]
fn monitor_discovered_primary_is_an_optional_boolean_and_reaches_the_snapshot() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let absent = discover(&mut core, monitor_object("m1"));
    assert_eq!(absent["monitors"][0]["primary"], false);
    let mut yes = monitor_object("m2");
    yes["primary"] = json!("yes");
    assert!(try_send(
        &mut core,
        json!({"type": "monitor_discovered", "monitor": yes})
    )
    .is_err());
    let mut null_flag = monitor_object("m2");
    null_flag["primary"] = Value::Null;
    assert!(try_send(
        &mut core,
        json!({"type": "monitor_discovered", "monitor": null_flag})
    )
    .is_err());
    // A rejected discovery leaves no monitor behind.
    let snapshot = core.snapshot_value().unwrap();
    assert_eq!(snapshot["monitors"].as_array().unwrap().len(), 1);
    let mut main = monitor_object("m1");
    main["primary"] = json!(true);
    let flagged = discover(&mut core, main);
    assert_eq!(flagged["monitors"][0]["primary"], true);
    // Re-discovery replaces the flag, so an absent key reads as false again.
    let replaced = discover(&mut core, monitor_object("m1"));
    assert_eq!(replaced["monitors"][0]["primary"], false);
}

#[test]
fn snapshot_reports_automatic_false_only_while_a_selection_exists() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let initial = core.snapshot_value().unwrap();
    assert_eq!(initial["monitored_display"]["automatic"], true);
    let selected = select_monitored(&mut core, json!("m1"));
    assert_eq!(selected["monitored_display"]["automatic"], false);
    let cleared = select_monitored(&mut core, Value::Null);
    assert_eq!(cleared["monitored_display"]["automatic"], true);
}

// Like monitor_object, at the given brightness level (0..=100).
fn monitor_at(id: &str, level: u64) -> Value {
    let mut monitor = monitor_object(id);
    monitor["brightness"]["value"] = json!(level);
    monitor["brightness"]["raw"] = json!(level);
    monitor
}

// Turns presence protection on with AC power, as the shells report it.
fn enable_camera(core: &mut Core) -> Value {
    send(core, json!({"type": "power", "on_battery": false}));
    send(
        core,
        json!({"type": "update_settings", "settings": {"presence_protection_enabled": true}}),
    )
}

fn set_threshold(core: &mut Core, percent: u64) -> Value {
    send(
        core,
        json!({"type": "update_settings", "settings": {"presence_min_brightness_percent": percent}}),
    )
}

// Presence samples carry the current camera generation, as the shell tags them.
fn presence_sample(core: &mut Core, status: &str, offset_ms: u64) -> Value {
    let generation = core.snapshot_value().unwrap()["camera"]["generation"].clone();
    send(
        core,
        json!({"type": "presence_observation", "status": status, "captured_at_ms": capture_time(offset_ms), "camera_generation": generation}),
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

// The camera follows only the monitored display: with Automatic it is the first monitor by id,
// so a low first monitor suspends the camera even while a second monitor is bright.
#[test]
fn automatic_resolves_the_first_monitor_by_id_and_selecting_the_other_moves_the_camera() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 10));
    discover(&mut core, monitor_at("m2", 50));
    set_threshold(&mut core, 15);
    let s = enable_camera(&mut core);
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = select_monitored(&mut core, json!("m2"));
    assert_eq!(monitored_id(&s), "m2");
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn a_primary_external_monitor_is_automatic_and_its_level_runs_the_camera() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 10));
    let mut primary = monitor_at("m2", 50);
    primary["primary"] = json!(true);
    discover(&mut core, primary);
    set_threshold(&mut core, 15);
    let s = enable_camera(&mut core);
    // Automatic prefers the flagged main display over the lower id, so m1 at 10 is not monitored.
    assert_eq!(monitored_id(&s), "m2");
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn an_external_below_the_threshold_suspends_the_camera_even_with_a_bright_built_in() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("dell", 14));
    // The built-in screen reports 62 percent here, above the default threshold of 20.
    set_builtin(&mut core, true, true);
    let s = enable_camera(&mut core);
    // Automatic picks the external monitor before the built-in, and 14 is at or below 20.
    assert_eq!(monitored_id(&s), "dell");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = select_monitored(&mut core, json!("builtin"));
    assert_eq!(monitored_id(&s), "builtin");
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn disconnecting_the_selected_monitor_hands_the_camera_to_the_next_remembered_one() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 10));
    discover(&mut core, monitor_at("m2", 50));
    set_threshold(&mut core, 15);
    enable_camera(&mut core);
    select_monitored(&mut core, json!("m1"));
    let s = select_monitored(&mut core, json!("m2"));
    assert_eq!(
        s["settings"]["monitored_display_history"],
        json!(["m2", "m1"])
    );
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type": "monitor_disconnected", "monitor_id": "m2"}),
    );
    // m2 is skipped, and the remembered m1 takes over at 10, which is below the threshold.
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(
        s["settings"]["monitored_display_history"],
        json!(["m2", "m1"])
    );
    assert_eq!(s["camera"]["mode"], "suspended");
}

#[test]
fn threshold_zero_suspends_a_monitored_display_at_level_zero_and_runs_it_at_level_one() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    set_threshold(&mut core, 0);
    enable_camera(&mut core);
    let s = discover(&mut core, monitor_at("m1", 0));
    // The comparison is inclusive, so zero is still below a zero threshold.
    assert_eq!(s["camera"]["mode"], "suspended");
    let generation = s["monitors"][0]["generation"].clone();
    let s = send(
        &mut core,
        json!({"type": "monitor_observed", "monitor_id": "m1", "generation": generation, "brightness": {"raw": 1}}),
    );
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn absence_dims_the_built_in_screen_while_an_external_monitor_keeps_the_camera_running() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 50));
    set_builtin(&mut core, true, true);
    let s = enable_camera(&mut core);
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "face");
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    // Absence counts and dims every protected screen, not only the monitored one.
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["monitors"][0]["presence_dimmed"], true);
    assert_eq!(s["builtin_protection"]["reasons"], json!(["presence"]));
}

#[test]
fn with_no_protected_display_the_camera_stays_off() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    let s = enable_camera(&mut core);
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_no_monitor");
    discover(&mut core, monitor_at("m1", 50));
    // Positive control: a bright, protected monitor runs the camera.
    let s = set_builtin(&mut core, true, true);
    assert_eq!(s["camera"]["mode"], "face");
    let s = send(
        &mut core,
        json!({"type": "update_settings", "settings": {"protection_disabled": ["m1", "builtin"]}}),
    );
    // Both screens are opted out, so the bright monitored display alone must not run the camera.
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_no_monitor");
}

// Reports a display state the way the shell does, tagged with the monitor's current generation.
fn report_display_state(core: &mut Core, id: &str, state: &str) -> Value {
    let snapshot = core.snapshot_value().unwrap();
    let generation = snapshot["monitors"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["id"] == id)
        .unwrap()["generation"]
        .clone();
    send(
        core,
        json!({"type": "monitor_display_state", "monitor_id": id, "generation": generation, "state": state}),
    )
}

// The camera follows the first awake candidate: a sleeping Automatic display hands the gate to the
// awake one, and the camera keeps running while that display is above the threshold.
#[test]
fn a_sleeping_automatic_display_hands_the_camera_to_the_awake_one_above_the_threshold() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 10));
    discover(&mut core, monitor_at("m2", 50));
    set_threshold(&mut core, 15);
    let s = enable_camera(&mut core);
    // Automatic selects m1, which is below the threshold, so the camera starts suspended.
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "suspended");
    let s = report_display_state(&mut core, "m1", "asleep");
    // The selection stays m1, but m2 (awake, 50) now gates the camera.
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "face");
}

#[test]
fn an_awake_gate_display_below_the_threshold_suspends_while_a_bright_display_sleeps() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 50));
    discover(&mut core, monitor_at("m2", 10));
    set_threshold(&mut core, 15);
    let s = enable_camera(&mut core);
    assert_eq!(s["camera"]["mode"], "face");
    let s = report_display_state(&mut core, "m1", "asleep");
    // m1 is asleep and no longer counts; the awake m2 at 10 is at or below 15.
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
}

#[test]
fn all_displays_asleep_suspends_the_camera_for_display_sleep() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 50));
    discover(&mut core, monitor_at("m2", 50));
    let s = enable_camera(&mut core);
    assert_eq!(s["camera"]["mode"], "face");
    report_display_state(&mut core, "m1", "asleep");
    let s = report_display_state(&mut core, "m2", "asleep");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_display_sleep");
    assert!(s["camera"]["next_sample_in_ms"].is_null());
}

#[test]
fn a_sleeping_selected_display_falls_through_to_the_next_awake_candidate() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 10));
    discover(&mut core, monitor_at("m2", 50));
    set_threshold(&mut core, 15);
    enable_camera(&mut core);
    let s = select_monitored(&mut core, json!("m2"));
    assert_eq!(monitored_id(&s), "m2");
    assert_eq!(s["camera"]["mode"], "face");
    let s = report_display_state(&mut core, "m2", "asleep");
    // The selection stays m2, but the next candidate, Automatic's m1 (awake, 10), gates the camera.
    assert_eq!(monitored_id(&s), "m2");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
}

// Replaces the whole per-display presence threshold map, as the Settings screen does.
fn set_display_thresholds(core: &mut Core, map: Value) -> Value {
    send(
        core,
        json!({"type": "update_settings", "settings": {"presence_min_brightness_by_display": map}}),
    )
}

// One monitor at `level`, with the global threshold at 15 and the given map, after the camera is on.
// Each call builds its own core, so the cases share no state.
fn single_monitor_snapshot(level: u64, thresholds: Value) -> Value {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", level));
    set_threshold(&mut core, 15);
    set_display_thresholds(&mut core, thresholds);
    enable_camera(&mut core)
}

// A display without an entry uses the global threshold. Only m2 has an entry here, so m1 is compared
// with 15: 10 suspends the camera and 16 runs it.
#[test]
fn a_display_without_an_entry_uses_the_global_threshold() {
    let s = single_monitor_snapshot(10, json!({"m2": 5}));
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = single_monitor_snapshot(16, json!({"m2": 5}));
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "face");
}

// An entry replaces the global threshold for its own display only. With the global 15, an entry of 5
// lets m1 at 10 run the camera, and an entry of 30 suspends m1 at 20, which the global value allows.
#[test]
fn an_entry_replaces_the_global_threshold_for_its_display() {
    let s = single_monitor_snapshot(10, json!({"m1": 5}));
    assert_eq!(s["camera"]["mode"], "face");
    let s = single_monitor_snapshot(10, json!({}));
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = single_monitor_snapshot(20, json!({"m1": 30}));
    assert_eq!(s["camera"]["mode"], "suspended");
    let s = single_monitor_snapshot(20, json!({}));
    assert_eq!(s["camera"]["mode"], "face");
}

// The reserved "builtin" id keys the built-in screen's entry. Its level is 62 here, above the global
// 15, so only its own entry can suspend the camera.
#[test]
fn the_builtin_entry_gates_the_built_in_screen() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    set_threshold(&mut core, 15);
    set_builtin(&mut core, true, true);
    let s = enable_camera(&mut core);
    assert_eq!(monitored_id(&s), "builtin");
    assert_eq!(s["camera"]["mode"], "face");
    let s = set_display_thresholds(&mut core, json!({"builtin": 70}));
    // 62 is at or below the built-in's own entry of 70, although it is above the global 15.
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = set_display_thresholds(&mut core, json!({"builtin": 50}));
    assert_eq!(s["camera"]["mode"], "face");
}

// An entry for a display that is not connected stays in the settings and applies again when the
// display returns. The entry 60 is above the level 50, so the camera is suspended before and after.
#[test]
fn an_entry_for_a_disconnected_display_is_kept_and_applies_when_it_returns() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 50));
    set_threshold(&mut core, 15);
    set_display_thresholds(&mut core, json!({"m1": 60}));
    let s = enable_camera(&mut core);
    assert_eq!(s["camera"]["mode"], "suspended");
    let s = send(
        &mut core,
        json!({"type": "monitor_disconnected", "monitor_id": "m1"}),
    );
    assert_eq!(s["camera"]["display_status"], "suspended_no_monitor");
    // The entry survives the disconnect, so it is still there when m1 comes back.
    assert_eq!(
        s["settings"]["presence_min_brightness_by_display"],
        json!({"m1": 60.0})
    );
    let s = discover(&mut core, monitor_at("m1", 50));
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
}

// Switching the monitored display switches which entry gates the camera. Both monitors are at 10:
// m1's entry of 5 lets the camera run, and m2's entry of 60 suspends it while m2 is monitored.
#[test]
fn switching_the_monitored_display_switches_which_entry_gates_the_camera() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("m1", 10));
    discover(&mut core, monitor_at("m2", 10));
    set_threshold(&mut core, 15);
    set_display_thresholds(&mut core, json!({"m1": 5, "m2": 60}));
    let s = enable_camera(&mut core);
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "face");
    let s = select_monitored(&mut core, json!("m2"));
    assert_eq!(monitored_id(&s), "m2");
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
    let s = select_monitored(&mut core, json!("m1"));
    assert_eq!(monitored_id(&s), "m1");
    assert_eq!(s["camera"]["mode"], "face");
}

// The snapshot entry for one monitor, found by id.
fn monitor_entry<'a>(snapshot: &'a Value, id: &str) -> &'a Value {
    snapshot["monitors"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["id"] == id)
        .unwrap()
}

// Reports a monitor's brightness level the way the shell does, tagged with its generation.
fn observe_brightness(core: &mut Core, id: &str, level: u64) -> Value {
    let snapshot = core.snapshot_value().unwrap();
    let generation = monitor_entry(&snapshot, id)["generation"].clone();
    send(
        core,
        json!({"type": "monitor_observed", "monitor_id": id, "generation": generation, "brightness": {"raw": level}}),
    )
}

// "a" is the selected gate at 60 and is not protected; "b" is protected and gets dimmed by absence.
// When the gate drops to 10 the camera must keep running, because a dimmed display needs samples to
// return. A present sample is then accepted and restores "b", and only after that does the low gate
// suspend the camera.
#[test]
fn a_dimmed_display_keeps_the_camera_running_while_the_gate_is_below_the_threshold() {
    let mut core = Core::new(CoreOptions::default()).unwrap();
    discover(&mut core, monitor_at("a", 60));
    discover(&mut core, monitor_at("b", 50));
    set_threshold(&mut core, 15);
    send(
        &mut core,
        json!({"type": "update_settings", "settings": {"protection_disabled": ["a"]}}),
    );
    select_monitored(&mut core, json!("a"));
    let s = enable_camera(&mut core);
    assert_eq!(monitored_id(&s), "a");
    assert_eq!(s["camera"]["mode"], "face");
    presence_sample(&mut core, "absent", 1_000);
    let s = presence_sample(&mut core, "absent", 2_000);
    assert_eq!(monitor_entry(&s, "b")["presence_dimmed"], true);
    assert_eq!(monitor_entry(&s, "a")["presence_dimmed"], false);
    // "a" is still the gate and is now below the threshold, but "b" is dimmed, so the camera runs.
    let s = observe_brightness(&mut core, "a", 10);
    assert_eq!(monitored_id(&s), "a");
    assert_eq!(s["camera"]["mode"], "face");
    assert_eq!(s["camera"]["capture_mode"], "continuous_return");
    assert_eq!(monitor_entry(&s, "b")["presence_dimmed"], true);
    // The present sample is accepted because the camera runs, and it restores "b".
    let s = presence_sample(&mut core, "present", 3_000);
    assert_eq!(monitor_entry(&s, "b")["presence_dimmed"], false);
    // Nothing is dimmed now, so the low gate suspends the camera.
    assert_eq!(s["camera"]["mode"], "suspended");
    assert_eq!(s["camera"]["display_status"], "suspended_low_brightness");
}
