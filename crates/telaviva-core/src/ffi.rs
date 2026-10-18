use crate::camera_lifecycle::{CameraLifecycle, StepError, MAX_EVENT_BYTES};
use crate::update::{self, UpdateError};
use crate::{Core, CoreOptions};
use serde::Deserialize;
use std::{
    ffi::c_void,
    panic::{catch_unwind, AssertUnwindSafe},
    path::PathBuf,
    ptr, slice,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Mutex,
    },
};

pub const ABI_VERSION: u32 = 1;
pub const TV_OK: i32 = 0;
pub const TV_INVALID_ARGUMENT: i32 = 1;
pub const TV_ABI_MISMATCH: i32 = 2;
pub const TV_CLOSED: i32 = 3;
pub const TV_INTERNAL: i32 = 4;
pub const TV_SIZE_LIMIT: i32 = 5;

pub type TvSnapshotReady = Option<extern "C" fn(u64, *mut c_void)>;

#[repr(C)]
pub struct TvBuffer {
    pub data: *mut u8,
    pub len: usize,
}

pub struct TvCore {
    core: Mutex<Core>,
    callback: TvSnapshotReady,
    userdata: *mut c_void,
    stopped: AtomicBool,
    active_callbacks: AtomicUsize,
}
unsafe impl Send for TvCore {}
unsafe impl Sync for TvCore {}

/// Opaque handle for camera session timing. The lifecycle is owned data behind a mutex, so
/// the handle is `Send` and `Sync` on its own and needs no `unsafe impl`. The assertion below
/// keeps that true: a future raw-pointer field would stop this from compiling.
pub struct TvCameraLifecycle {
    lifecycle: Mutex<CameraLifecycle>,
}

const fn assert_send_sync<T: Send + Sync>() {}
const _: () = assert_send_sync::<TvCameraLifecycle>();

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Options {
    #[serde(default)]
    config_path: Option<PathBuf>,
    #[serde(default)]
    os_locale: Option<String>,
}

fn guarded(f: impl FnOnce() -> i32) -> i32 {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(TV_INTERNAL)
}

#[no_mangle]
/// Returns the ABI version accepted by this library.
pub extern "C" fn tv_abi_version() -> u32 {
    ABI_VERSION
}

#[no_mangle]
/// Creates a core handle and initializes its optional settings file.
///
/// # Safety
/// `options` must point to `options_len` readable bytes when non-null, and `out` must point to a writable handle pointer.
pub unsafe extern "C" fn tv_create(
    version: u32,
    options: *const u8,
    options_len: usize,
    callback: TvSnapshotReady,
    userdata: *mut c_void,
    out: *mut *mut TvCore,
) -> i32 {
    guarded(|| {
        if out.is_null() || (options_len > 0 && options.is_null()) {
            return TV_INVALID_ARGUMENT;
        }
        if version != ABI_VERSION {
            return TV_ABI_MISMATCH;
        }
        if options_len > 64 * 1024 {
            return TV_SIZE_LIMIT;
        }
        let parsed = if options_len == 0 {
            Options {
                config_path: None,
                os_locale: None,
            }
        } else {
            match serde_json::from_slice::<Options>(slice::from_raw_parts(options, options_len)) {
                Ok(v) => v,
                Err(_) => return TV_INVALID_ARGUMENT,
            }
        };
        let core = match Core::new(CoreOptions {
            config_path: parsed.config_path,
            os_locale: parsed.os_locale,
        }) {
            Ok(v) => v,
            Err(_) => return TV_INTERNAL,
        };
        *out = Box::into_raw(Box::new(TvCore {
            core: Mutex::new(core),
            callback,
            userdata,
            stopped: AtomicBool::new(false),
            active_callbacks: AtomicUsize::new(0),
        }));
        TV_OK
    })
}

#[no_mangle]
/// Submits one UTF-8 JSON command to a live core.
///
/// # Safety
/// `handle` must be live; `bytes` must point to `len` readable bytes when non-null.
pub unsafe extern "C" fn tv_submit(handle: *mut TvCore, bytes: *const u8, len: usize) -> i32 {
    guarded(|| {
        if handle.is_null() || (len > 0 && bytes.is_null()) {
            return TV_INVALID_ARGUMENT;
        }
        if len > 64 * 1024 {
            return TV_SIZE_LIMIT;
        }
        let h = &*handle;
        let input = if len == 0 {
            &[][..]
        } else {
            slice::from_raw_parts(bytes, len)
        };
        if h.stopped.load(Ordering::Acquire) {
            let is_result = serde_json::from_slice::<serde_json::Value>(input)
                .ok()
                .and_then(|v| {
                    v.get("type")
                        .and_then(serde_json::Value::as_str)
                        .map(|s| s == "hardware_result")
                })
                .unwrap_or(false);
            if !is_result {
                return TV_CLOSED;
            }
        }
        let result = {
            let Ok(mut core) = h.core.lock() else {
                return TV_INTERNAL;
            };
            core.submit_json(input)
        };
        if result.is_err() {
            return TV_INVALID_ARGUMENT;
        }
        if let Some(cb) = h.callback {
            if !h.stopped.load(Ordering::Acquire) {
                h.active_callbacks.fetch_add(1, Ordering::AcqRel);
                if !h.stopped.load(Ordering::Acquire) {
                    let revision = h
                        .core
                        .lock()
                        .map(|c| {
                            c.snapshot_value()
                                .ok()
                                .and_then(|v| v["revision"].as_u64())
                                .unwrap_or(0)
                        })
                        .unwrap_or(0);
                    cb(revision, h.userdata);
                }
                h.active_callbacks.fetch_sub(1, Ordering::AcqRel);
            }
        }
        TV_OK
    })
}

#[no_mangle]
/// Copies the current snapshot into a Rust-owned buffer.
///
/// # Safety
/// `handle` must be live and `out` must point to a writable `TvBuffer`.
pub unsafe extern "C" fn tv_snapshot(handle: *mut TvCore, out: *mut TvBuffer) -> i32 {
    guarded(|| {
        if handle.is_null() || out.is_null() {
            return TV_INVALID_ARGUMENT;
        }
        let h = &*handle;
        let Ok(core) = h.core.lock() else {
            return TV_INTERNAL;
        };
        let value = match core.snapshot_value() {
            Ok(v) => v,
            Err(_) => return TV_INTERNAL,
        };
        let bytes = match serde_json::to_vec(&value) {
            Ok(v) => v,
            Err(_) => return TV_INTERNAL,
        };
        if bytes.len() > 8 * 1024 * 1024 {
            return TV_SIZE_LIMIT;
        }
        let bytes = bytes.into_boxed_slice();
        let len = bytes.len();
        let ptr = Box::into_raw(bytes) as *mut u8;
        (*out).data = ptr;
        (*out).len = len;
        TV_OK
    })
}

#[no_mangle]
/// Stops callbacks and requests best-effort restoration actions.
///
/// # Safety
/// `handle` must be live and must not be destroyed concurrently.
pub unsafe extern "C" fn tv_shutdown(handle: *mut TvCore) -> i32 {
    guarded(|| {
        if handle.is_null() {
            return TV_INVALID_ARGUMENT;
        }
        let h = &*handle;
        h.stopped.store(true, Ordering::Release);
        {
            let Ok(mut core) = h.core.lock() else {
                return TV_INTERNAL;
            };
            let _ = core.submit_json(br#"{"type":"shutdown"}"#);
        }
        while h.active_callbacks.load(Ordering::Acquire) != 0 {
            std::thread::yield_now()
        }
        TV_OK
    })
}

#[no_mangle]
/// Destroys a core after the host has handled any shutdown actions.
///
/// # Safety
/// `handle` must be null or a live handle with no concurrent calls.
pub unsafe extern "C" fn tv_destroy(handle: *mut TvCore) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !handle.is_null() {
            let _ = tv_shutdown(handle);
            drop(Box::from_raw(handle));
        }
    }));
}

#[no_mangle]
/// Frees a snapshot buffer and clears its fields.
///
/// # Safety
/// `buffer` must be null or point to an initialized `TvBuffer` returned by this library.
pub unsafe extern "C" fn tv_buffer_free(buffer: *mut TvBuffer) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if buffer.is_null() {
            return;
        }
        let b = &mut *buffer;
        if !b.data.is_null() {
            let slice = ptr::slice_from_raw_parts_mut(b.data, b.len);
            drop(Box::from_raw(slice));
            b.data = ptr::null_mut();
            b.len = 0
        }
    }));
}

#[no_mangle]
/// Creates a camera lifecycle handle in the inactive state (mode `disabled`, no timers).
///
/// # Safety
/// `out` must point to a writable handle pointer.
pub unsafe extern "C" fn tv_camera_lifecycle_create(out: *mut *mut TvCameraLifecycle) -> i32 {
    guarded(|| {
        if out.is_null() {
            return TV_INVALID_ARGUMENT;
        }
        *out = Box::into_raw(Box::new(TvCameraLifecycle {
            lifecycle: Mutex::new(CameraLifecycle::new()),
        }));
        TV_OK
    })
}

#[no_mangle]
/// Applies one UTF-8 JSON event and copies the JSON result into a Rust-owned buffer.
///
/// A rejected event returns `TV_INVALID_ARGUMENT` (or `TV_SIZE_LIMIT` when it is longer than
/// 64 KiB), leaves the lifecycle unchanged and writes no buffer.
///
/// # Safety
/// `handle` must be live; `json` must point to `len` readable bytes when non-null; `out` must
/// point to a writable `TvBuffer`.
pub unsafe extern "C" fn tv_camera_lifecycle_step(
    handle: *mut TvCameraLifecycle,
    json: *const u8,
    len: usize,
    out: *mut TvBuffer,
) -> i32 {
    guarded(|| {
        if handle.is_null() || out.is_null() || (len > 0 && json.is_null()) {
            return TV_INVALID_ARGUMENT;
        }
        if len > MAX_EVENT_BYTES {
            return TV_SIZE_LIMIT;
        }
        let h = &*handle;
        let input = if len == 0 {
            &[][..]
        } else {
            slice::from_raw_parts(json, len)
        };
        let result = {
            let Ok(mut lifecycle) = h.lifecycle.lock() else {
                return TV_INTERNAL;
            };
            lifecycle.step(input)
        };
        let text = match result {
            Ok(text) => text,
            Err(StepError::InputTooLarge { .. }) => return TV_SIZE_LIMIT,
            Err(_) => return TV_INVALID_ARGUMENT,
        };
        let bytes = text.into_bytes().into_boxed_slice();
        let written = bytes.len();
        let ptr = Box::into_raw(bytes) as *mut u8;
        (*out).data = ptr;
        (*out).len = written;
        TV_OK
    })
}

#[no_mangle]
/// Destroys a camera lifecycle handle. A null handle is ignored.
///
/// # Safety
/// `handle` must be null or a live handle with no concurrent calls.
pub unsafe extern "C" fn tv_camera_lifecycle_destroy(handle: *mut TvCameraLifecycle) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !handle.is_null() {
            drop(Box::from_raw(handle));
        }
    }));
}

#[no_mangle]
/// Writes the core version (UTF-8, no terminator) into a Rust-owned buffer.
///
/// # Safety
/// `out` must point to a writable `TvBuffer`.
pub unsafe extern "C" fn tv_core_version(out: *mut TvBuffer) -> i32 {
    guarded(|| {
        if out.is_null() {
            return TV_INVALID_ARGUMENT;
        }
        let bytes = update::core_version()
            .as_bytes()
            .to_vec()
            .into_boxed_slice();
        let written = bytes.len();
        let ptr = Box::into_raw(bytes) as *mut u8;
        (*out).data = ptr;
        (*out).len = written;
        TV_OK
    })
}

#[no_mangle]
/// Compares the installed version with a GitHub releases/latest answer and writes the
/// UpdateOutcome JSON into a Rust-owned buffer. Pure: no network, no state.
///
/// A null `body` is accepted only when `body_len` is zero.
///
/// # Safety
/// `installed` must point to `installed_len` readable bytes; `body` must point to `body_len`
/// readable bytes when non-null; `out` must point to a writable `TvBuffer`.
pub unsafe extern "C" fn tv_update_evaluate(
    installed: *const u8,
    installed_len: usize,
    http_status: u32,
    body: *const u8,
    body_len: usize,
    out: *mut TvBuffer,
) -> i32 {
    guarded(|| {
        if out.is_null() || installed.is_null() || installed_len == 0 {
            return TV_INVALID_ARGUMENT;
        }
        if body_len > 0 && body.is_null() {
            return TV_INVALID_ARGUMENT;
        }
        // Lengths are checked before any slice is built.
        if installed_len > update::MAX_VERSION_BYTES || body_len > update::MAX_BODY_BYTES {
            return TV_SIZE_LIMIT;
        }
        let installed_bytes = slice::from_raw_parts(installed, installed_len);
        let Ok(installed_text) = std::str::from_utf8(installed_bytes) else {
            return TV_INVALID_ARGUMENT;
        };
        let body_bytes = if body_len == 0 {
            &[][..]
        } else {
            slice::from_raw_parts(body, body_len)
        };
        let text = match update::evaluate(installed_text, http_status, body_bytes) {
            Ok(text) => text,
            Err(UpdateError::InvalidInstalled) => return TV_INVALID_ARGUMENT,
            Err(UpdateError::BodyTooLarge | UpdateError::InstalledTooLarge) => {
                return TV_SIZE_LIMIT
            }
        };
        let bytes = text.into_bytes().into_boxed_slice();
        let written = bytes.len();
        let ptr = Box::into_raw(bytes) as *mut u8;
        (*out).data = ptr;
        (*out).len = written;
        TV_OK
    })
}
