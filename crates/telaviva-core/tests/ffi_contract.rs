use std::{
    ffi::c_void,
    ptr,
    sync::atomic::{AtomicU64, Ordering},
};
use telaviva_core::ffi::{self, TvBuffer, TvControls, TvCore};

static CALLBACK_REVISION: AtomicU64 = AtomicU64::new(0);
extern "C" fn ready(revision: u64, _userdata: *mut c_void) {
    CALLBACK_REVISION.store(revision, Ordering::SeqCst);
}

#[test]
fn ffi_validates_version_copies_snapshot_frees_buffer_and_drains_callback() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(ffi::tv_abi_version(), 1);
        assert_eq!(
            ffi::tv_create(99, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_ABI_MISMATCH
        );
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, Some(ready), ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        let cmd = br#"{"type":"power","on_battery":false}"#;
        assert_eq!(ffi::tv_submit(core, cmd.as_ptr(), cmd.len()), ffi::TV_OK);
        assert!(CALLBACK_REVISION.load(Ordering::SeqCst) > 1);
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let bytes = std::slice::from_raw_parts(buffer.data, buffer.len);
        let snapshot: serde_json::Value = serde_json::from_slice(bytes).unwrap();
        assert_eq!(
            snapshot["revision"].as_u64(),
            Some(CALLBACK_REVISION.load(Ordering::SeqCst))
        );
        ffi::tv_buffer_free(&mut buffer);
        assert!(buffer.data.is_null());
        assert_eq!(buffer.len, 0);
        assert_eq!(ffi::tv_shutdown(core), ffi::TV_OK);
        assert_eq!(
            ffi::tv_submit(core, cmd.as_ptr(), cmd.len()),
            ffi::TV_CLOSED
        );
        let result = br#"{"type":"hardware_result","action_id":999,"generation":1,"ok":true}"#;
        assert_eq!(
            ffi::tv_submit(core, result.as_ptr(), result.len()),
            ffi::TV_OK
        );
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_rejects_oversized_and_invalid_json_without_panicking() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        let oversized = vec![b' '; 64 * 1024 + 1];
        assert_eq!(
            ffi::tv_submit(core, oversized.as_ptr(), oversized.len()),
            ffi::TV_SIZE_LIMIT
        );
        assert_eq!(
            ffi::tv_submit(core, b"{".as_ptr(), 1),
            ffi::TV_INVALID_ARGUMENT
        );
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_rejects_null_nonempty_and_malformed_utf8_inputs() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        assert_eq!(
            ffi::tv_submit(core, ptr::null(), 1),
            ffi::TV_INVALID_ARGUMENT
        );
        assert_eq!(
            ffi::tv_submit(core, ptr::null(), 0),
            ffi::TV_INVALID_ARGUMENT
        );
        assert_eq!(
            ffi::tv_submit(core, [0xff].as_ptr(), 1),
            ffi::TV_INVALID_ARGUMENT
        );
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_transports_display_state_and_always_on_setting_without_abi_change() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        for cmd in [
            br#"{"type":"monitor_discovered","monitor":{"id":"screen","name":"Display","identity_status":"stable","connection_path":"usb","brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},"volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}}}"#.as_slice(),
            br#"{"type":"power","on_battery":false}"#.as_slice(),
            br#"{"type":"update_settings","settings":{"presence_protection_enabled":true,"always_on_camera":true,"hardware_keys_enabled":true}}"#.as_slice(),
            br#"{"type":"update_settings","settings":{"presence_min_brightness_percent":35}}"#.as_slice(),
            br#"{"type":"monitor_display_state","monitor_id":"screen","generation":1,"state":"asleep"}"#.as_slice(),
        ] {
            assert_eq!(ffi::tv_submit(core, cmd.as_ptr(), cmd.len()), ffi::TV_OK);
        }
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let snapshot: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        assert_eq!(snapshot["settings"]["always_on_camera"], true);
        assert_eq!(snapshot["settings"]["hardware_keys_enabled"], true);
        assert_eq!(
            snapshot["settings"]["presence_min_brightness_percent"],
            35.0
        );
        assert_eq!(snapshot["camera"]["mode"], "suspended");
        assert_eq!(
            snapshot["camera"]["display_status"],
            "suspended_display_sleep"
        );
        assert_eq!(snapshot["monitors"][0]["display_state"], "asleep");
        assert_eq!(ffi::tv_abi_version(), 1);
        ffi::tv_buffer_free(&mut buffer);
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_builtin_display_reaches_the_snapshot() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        let cmd =
            br#"{"type":"builtin_display","available":true,"brightness":44,"calibrating":false}"#;
        assert_eq!(ffi::tv_submit(core, cmd.as_ptr(), cmd.len()), ffi::TV_OK);
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let snapshot: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(snapshot["ambient"]["available"], true);
        assert_eq!(snapshot["ambient"]["builtin_percent"].as_f64(), Some(44.0));
        assert_eq!(snapshot["ambient"]["calibrating"], false);

        // A rejected command must leave the snapshot untouched.
        let invalid =
            br#"{"type":"builtin_display","available":true,"brightness":101,"calibrating":false}"#;
        assert_eq!(
            ffi::tv_submit(core, invalid.as_ptr(), invalid.len()),
            ffi::TV_INVALID_ARGUMENT
        );
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let unchanged: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(unchanged, snapshot);
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_inference_device_setting_reaches_the_snapshot() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        let cmd =
            br#"{"type":"update_settings","settings":{"inference_device":"gpu:1002:164e:0"}}"#;
        assert_eq!(ffi::tv_submit(core, cmd.as_ptr(), cmd.len()), ffi::TV_OK);
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let snapshot: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(snapshot["settings"]["inference_device"], "gpu:1002:164e:0");
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_rejects_invalid_inference_device_and_keeps_auto() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        let invalid = br#"{"type":"update_settings","settings":{"inference_device":"GPU"}}"#;
        assert_eq!(
            ffi::tv_submit(core, invalid.as_ptr(), invalid.len()),
            ffi::TV_INVALID_ARGUMENT
        );
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let snapshot: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(snapshot["settings"]["inference_device"], "auto");
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_controls_round_trips_configure_and_frees_buffer() {
    unsafe {
        let mut controls: *mut TvControls = ptr::null_mut();
        assert_eq!(ffi::tv_controls_create(&mut controls), ffi::TV_OK);
        let event = br#"{"type":"configure","now":1.0,"brightness_step":5,"volume_step":5,"keys_enabled":true}"#;
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            ffi::tv_controls_step(controls, event.as_ptr(), event.len(), &mut buffer),
            ffi::TV_OK
        );
        let result: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert!(buffer.data.is_null());
        assert_eq!(buffer.len, 0);
        let object = result.as_object().expect("the result is a JSON object");
        for key in ["value", "effects", "next_deadline"] {
            assert!(object.contains_key(key), "missing key {key}");
        }
        ffi::tv_controls_destroy(controls);
    }
}

#[test]
fn ffi_controls_rejects_null_arguments() {
    unsafe {
        assert_eq!(
            ffi::tv_controls_create(ptr::null_mut()),
            ffi::TV_INVALID_ARGUMENT
        );
        let mut controls: *mut TvControls = ptr::null_mut();
        assert_eq!(ffi::tv_controls_create(&mut controls), ffi::TV_OK);
        let event = br#"{"type":"configure","now":1.0,"brightness_step":5,"volume_step":5,"keys_enabled":true}"#;
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            ffi::tv_controls_step(ptr::null_mut(), event.as_ptr(), event.len(), &mut buffer),
            ffi::TV_INVALID_ARGUMENT
        );
        assert_eq!(
            ffi::tv_controls_step(controls, event.as_ptr(), event.len(), ptr::null_mut()),
            ffi::TV_INVALID_ARGUMENT
        );
        assert_eq!(
            ffi::tv_controls_step(controls, ptr::null(), 1, &mut buffer),
            ffi::TV_INVALID_ARGUMENT
        );
        assert!(buffer.data.is_null());
        assert_eq!(buffer.len, 0);
        ffi::tv_controls_destroy(controls);
    }
}

#[test]
fn ffi_controls_rejects_oversized_and_malformed_events_without_writing_out() {
    unsafe {
        let mut controls: *mut TvControls = ptr::null_mut();
        assert_eq!(ffi::tv_controls_create(&mut controls), ffi::TV_OK);
        let oversized = vec![b' '; 64 * 1024 + 1];
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            ffi::tv_controls_step(controls, oversized.as_ptr(), oversized.len(), &mut buffer),
            ffi::TV_SIZE_LIMIT
        );
        assert!(buffer.data.is_null());
        assert_eq!(buffer.len, 0);

        // A sentinel proves that a failed step does not write to the out buffer.
        let mut sentinel = TvBuffer {
            data: 1usize as *mut u8,
            len: 42,
        };
        let malformed = b"{";
        assert_eq!(
            ffi::tv_controls_step(controls, malformed.as_ptr(), malformed.len(), &mut sentinel),
            ffi::TV_INVALID_ARGUMENT
        );
        assert_eq!(sentinel.data, 1usize as *mut u8);
        assert_eq!(sentinel.len, 42);
        ffi::tv_controls_destroy(controls);
    }
}

#[test]
fn ffi_controls_destroy_ignores_null() {
    unsafe {
        ffi::tv_controls_destroy(ptr::null_mut());
    }
}

#[test]
fn ffi_presence_observation_reaches_the_snapshot() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        for cmd in [
            br#"{"type":"monitor_discovered","monitor":{"id":"screen","name":"Display","identity_status":"stable","connection_path":"usb","brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},"volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}}}"#.as_slice(),
            br#"{"type":"power","on_battery":false}"#.as_slice(),
            br#"{"type":"update_settings","settings":{"presence_protection_enabled":true}}"#.as_slice(),
        ] {
            assert_eq!(ffi::tv_submit(core, cmd.as_ptr(), cmd.len()), ffi::TV_OK);
        }
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let enabled: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(enabled["camera"]["mode"], "face");
        assert_eq!(enabled["camera"]["presence"], serde_json::Value::Null);

        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64;
        let observation = format!(
            r#"{{"type":"presence_observation","status":"present","captured_at_ms":{},"camera_generation":{}}}"#,
            now - 10_000,
            enabled["camera"]["generation"]
        );
        assert_eq!(
            ffi::tv_submit(core, observation.as_ptr(), observation.len()),
            ffi::TV_OK
        );
        assert_eq!(ffi::tv_snapshot(core, &mut buffer), ffi::TV_OK);
        let snapshot: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(snapshot["camera"]["presence"], "present");
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_diagnostics_drain_rejects_null_arguments() {
    unsafe {
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            ffi::tv_diagnostics_drain(ptr::null_mut(), &mut buffer),
            ffi::TV_INVALID_ARGUMENT
        );
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        assert_eq!(
            ffi::tv_diagnostics_drain(core, ptr::null_mut()),
            ffi::TV_INVALID_ARGUMENT
        );
        assert!(buffer.data.is_null());
        assert_eq!(buffer.len, 0);
        ffi::tv_destroy(core);
    }
}

#[test]
fn ffi_diagnostics_drain_returns_an_empty_array_then_empties_the_ring() {
    unsafe {
        let mut core: *mut TvCore = ptr::null_mut();
        assert_eq!(
            ffi::tv_create(1, ptr::null(), 0, None, ptr::null_mut(), &mut core),
            ffi::TV_OK
        );
        let mut buffer = TvBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(ffi::tv_diagnostics_drain(core, &mut buffer), ffi::TV_OK);
        let empty: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(empty, serde_json::json!([]));

        let cmd = br#"{"type":"monitor_discovered","monitor":{"id":"screen","name":"Display","identity_status":"stable","connection_path":"usb","brightness":{"supported":true,"value":70,"raw":70,"min":0,"max":100},"volume":{"supported":false,"value":null,"raw":null,"min":0,"max":100}}}"#;
        assert_eq!(ffi::tv_submit(core, cmd.as_ptr(), cmd.len()), ffi::TV_OK);
        assert_eq!(ffi::tv_diagnostics_drain(core, &mut buffer), ffi::TV_OK);
        let lines: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(lines.as_array().unwrap().len(), 1);
        let first = lines[0].as_str().unwrap();
        assert!(
            first.starts_with("monitor_discovered monitor=id-"),
            "{first}"
        );
        assert!(first.ends_with(" raw=70 baseline=70"), "{first}");

        assert_eq!(ffi::tv_diagnostics_drain(core, &mut buffer), ffi::TV_OK);
        let drained: serde_json::Value =
            serde_json::from_slice(std::slice::from_raw_parts(buffer.data, buffer.len)).unwrap();
        ffi::tv_buffer_free(&mut buffer);
        assert_eq!(drained, serde_json::json!([]));
        ffi::tv_destroy(core);
    }
}
