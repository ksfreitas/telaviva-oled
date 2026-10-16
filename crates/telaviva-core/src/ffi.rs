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
