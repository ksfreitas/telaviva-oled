use std::{
    ffi::c_void,
    ptr,
    sync::atomic::{AtomicU64, Ordering},
};
use telaviva_core::ffi::{self, TvBuffer, TvCore};

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
