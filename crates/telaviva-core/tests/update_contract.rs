use std::ptr;
use telaviva_core::ffi::{self, TvBuffer};

const MAX_BODY_BYTES: usize = 1024 * 1024;
const MAX_VERSION_BYTES: usize = 64;

fn empty_buffer() -> TvBuffer {
    TvBuffer {
        data: ptr::null_mut(),
        len: 0,
    }
}

fn assert_untouched(buffer: &TvBuffer) {
    assert!(buffer.data.is_null());
    assert_eq!(buffer.len, 0);
}

/// Reads the JSON in a buffer written by `tv_update_evaluate` and frees it.
fn take_outcome(buffer: &mut TvBuffer) -> serde_json::Value {
    let bytes = unsafe { std::slice::from_raw_parts(buffer.data, buffer.len) };
    let value = serde_json::from_slice(bytes).expect("outcome must be JSON");
    unsafe { ffi::tv_buffer_free(buffer) };
    value
}

fn call_evaluate(installed: &[u8], status: u32, body: &[u8], out: &mut TvBuffer) -> i32 {
    unsafe {
        ffi::tv_update_evaluate(
            installed.as_ptr(),
            installed.len(),
            status,
            body.as_ptr(),
            body.len(),
            out,
        )
    }
}

fn outcome(installed: &str, status: u32, body: &[u8]) -> serde_json::Value {
    let mut buffer = empty_buffer();
    let code = call_evaluate(installed.as_bytes(), status, body, &mut buffer);
    assert_eq!(code, ffi::TV_OK);
    take_outcome(&mut buffer)
}

#[test]
fn core_version_export_returns_package_version_and_buffer_free_zeroes_it() {
    let mut buffer = empty_buffer();
    let code = unsafe { ffi::tv_core_version(&mut buffer) };
    assert_eq!(code, ffi::TV_OK);
    let text = unsafe { std::slice::from_raw_parts(buffer.data, buffer.len) };
    assert_eq!(text, env!("CARGO_PKG_VERSION").as_bytes());
    unsafe { ffi::tv_buffer_free(&mut buffer) };
    assert_untouched(&buffer);
}

#[test]
fn core_version_rejects_null_out() {
    let code = unsafe { ffi::tv_core_version(ptr::null_mut()) };
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
}

#[test]
fn evaluate_reports_each_outcome() {
    let newer = br#"{"tag_name":"v0.2.0","draft":false,"prerelease":false}"#;
    let update = outcome("0.1.0", 200, newer);
    assert_eq!(update["status"], "update_available");
    assert_eq!(update["remote_version"], "0.2.0");
    assert!(update["release_url"]
        .as_str()
        .unwrap()
        .starts_with("https://github.com/ksfreitas/telaviva-oled/releases/tag/"));

    let same = outcome("0.2.0", 200, br#"{"tag_name":"v0.2.0"}"#);
    assert_eq!(same["status"], "up_to_date");
    assert_eq!(same["remote_version"], "0.2.0");

    assert_eq!(outcome("0.1.0", 404, b"")["status"], "no_release");
    assert_eq!(outcome("0.1.0", 429, b"")["status"], "rate_limited");
    assert_eq!(outcome("0.1.0", 500, b"")["status"], "http_error");
    assert_eq!(
        outcome("0.1.0", 200, b"not json")["status"],
        "invalid_response"
    );
}

#[test]
fn evaluate_rejects_null_out() {
    let installed = b"1.0.0";
    let code = unsafe {
        ffi::tv_update_evaluate(
            installed.as_ptr(),
            installed.len(),
            200,
            ptr::null(),
            0,
            ptr::null_mut(),
        )
    };
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
}

#[test]
fn evaluate_rejects_null_installed_with_length() {
    let mut buffer = empty_buffer();
    let code = unsafe { ffi::tv_update_evaluate(ptr::null(), 5, 200, ptr::null(), 0, &mut buffer) };
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
    assert_untouched(&buffer);
}

#[test]
fn evaluate_rejects_empty_installed() {
    let installed = b"1.0.0";
    let mut buffer = empty_buffer();
    let code =
        unsafe { ffi::tv_update_evaluate(installed.as_ptr(), 0, 200, ptr::null(), 0, &mut buffer) };
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
    assert_untouched(&buffer);
}

#[test]
fn evaluate_rejects_malformed_or_non_utf8_installed() {
    let mut buffer = empty_buffer();
    let code = call_evaluate(b"abc", 200, b"{}", &mut buffer);
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
    assert_untouched(&buffer);

    let code = call_evaluate(&[0xff], 200, b"{}", &mut buffer);
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
    assert_untouched(&buffer);
}

#[test]
fn evaluate_accepts_null_empty_body_when_length_is_zero() {
    let installed = b"1.0.0";
    let mut buffer = empty_buffer();
    let code = unsafe {
        ffi::tv_update_evaluate(
            installed.as_ptr(),
            installed.len(),
            404,
            ptr::null(),
            0,
            &mut buffer,
        )
    };
    assert_eq!(code, ffi::TV_OK);
    assert_eq!(take_outcome(&mut buffer)["status"], "no_release");
}

#[test]
fn evaluate_rejects_null_body_with_length() {
    let installed = b"1.0.0";
    let mut buffer = empty_buffer();
    let code = unsafe {
        ffi::tv_update_evaluate(
            installed.as_ptr(),
            installed.len(),
            200,
            ptr::null(),
            1,
            &mut buffer,
        )
    };
    assert_eq!(code, ffi::TV_INVALID_ARGUMENT);
    assert_untouched(&buffer);
}

#[test]
fn evaluate_rejects_oversized_body_with_size_limit() {
    let body = vec![b' '; MAX_BODY_BYTES + 1];
    let mut buffer = empty_buffer();
    let code = call_evaluate(b"1.0.0", 200, &body, &mut buffer);
    assert_eq!(code, ffi::TV_SIZE_LIMIT);
    assert_untouched(&buffer);
}

#[test]
fn evaluate_rejects_oversized_installed_with_size_limit() {
    let installed = vec![b'1'; MAX_VERSION_BYTES + 1];
    let mut buffer = empty_buffer();
    let code = call_evaluate(&installed, 200, b"{}", &mut buffer);
    assert_eq!(code, ffi::TV_SIZE_LIMIT);
    assert_untouched(&buffer);
}
