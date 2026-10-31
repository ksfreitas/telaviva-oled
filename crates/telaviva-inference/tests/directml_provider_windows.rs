#![cfg(all(target_os = "windows", target_arch = "x86_64"))]

use std::{
    ffi::CString,
    path::{Path, PathBuf},
};
use telaviva_inference::{
    tv_inference_create_with_provider, tv_inference_create_with_provider_device,
    tv_inference_destroy, tv_inference_detect_bgra, tv_inference_directml_available,
    tv_inference_selected_provider, TvInferenceHandle, TvInferenceResult,
};

const PROVIDER_CPU: i32 = 0;
const PROVIDER_DIRECTML_CANDIDATE: i32 = 2;
const DIRECTML_RUNTIME_ENV: &str = "TV_TEST_DIRECTML_RUNTIME";

/// The runtime path is bound once per process, so a process that tests a DirectML runtime cannot
/// also test the CPU runtime. The CPU-runtime tests skip themselves in that case. Only builds with
/// the `directml` feature honour the variable, so default builds always run the CPU tests.
fn directml_runtime_requested() -> bool {
    cfg!(feature = "directml") && std::env::var_os(DIRECTML_RUNTIME_ENV).is_some()
}

fn model_path(root: &Path) -> CString {
    CString::new(
        root.join("assets/model/face_detection_retinaface_mb025.onnx")
            .to_string_lossy()
            .as_bytes(),
    )
    .unwrap()
}

/// DirectML is not compiled into the default build, so this test binary only sees CPU-only
/// runtime behavior. It is skipped when the runtime has not been fetched, like the smoke test.
#[test]
fn directml_request_on_cpu_only_runtime_falls_back_to_cpu() {
    if directml_runtime_requested() {
        eprintln!("skipping: {DIRECTML_RUNTIME_ENV} binds the process to a DirectML runtime");
        return;
    }
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime_path = root.join("tools/inference/runtime/windows-x64/onnxruntime.dll");
    if !runtime_path.is_file() {
        eprintln!(
            "skipping: ONNX Runtime has not been fetched at {}",
            runtime_path.display()
        );
        eprintln!(
            "run tools/inference/fetch-windows-runtime.ps1 to enable directml_request_on_cpu_only_runtime_falls_back_to_cpu"
        );
        return;
    }
    let runtime = CString::new(runtime_path.to_string_lossy().as_bytes()).unwrap();
    let model = model_path(&root);

    // The DirectML request must not fail: an unavailable accelerator yields a CPU session.
    let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe {
            tv_inference_create_with_provider(
                runtime.as_ptr(),
                model.as_ptr(),
                PROVIDER_DIRECTML_CANDIDATE,
                &mut handle,
            )
        },
        0
    );
    assert!(!handle.is_null());
    assert_eq!(
        unsafe { tv_inference_selected_provider(handle) },
        PROVIDER_CPU
    );
    // The loaded CPU-only runtime does not list DmlExecutionProvider.
    assert_eq!(unsafe { tv_inference_directml_available() }, 0);

    // The CPU session still produces a result on a neutral frame.
    let neutral = vec![128_u8; 320 * 320 * 4];
    let mut result = TvInferenceResult {
        status: -99,
        confidence: -1.0,
    };
    assert_eq!(
        unsafe {
            tv_inference_detect_bgra(
                handle,
                neutral.as_ptr(),
                neutral.len(),
                320,
                320,
                320 * 4,
                &mut result,
            )
        },
        0
    );
    assert_eq!(result.status, 0);
    unsafe { tv_inference_destroy(handle) };
}

/// A device index must never turn a request into an error. On the CPU-only runtime no DirectML
/// adapter can register, so index 3 must still produce a CPU session with provider 0.
#[test]
fn directml_device_index_on_cpu_only_runtime_falls_back_to_cpu() {
    if directml_runtime_requested() {
        eprintln!("skipping: {DIRECTML_RUNTIME_ENV} binds the process to a DirectML runtime");
        return;
    }
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime_path = root.join("tools/inference/runtime/windows-x64/onnxruntime.dll");
    if !runtime_path.is_file() {
        eprintln!(
            "skipping: ONNX Runtime has not been fetched at {}",
            runtime_path.display()
        );
        return;
    }
    let runtime = CString::new(runtime_path.to_string_lossy().as_bytes()).unwrap();
    let model = model_path(&root);

    let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe {
            tv_inference_create_with_provider_device(
                runtime.as_ptr(),
                model.as_ptr(),
                PROVIDER_DIRECTML_CANDIDATE,
                3,
                &mut handle,
            )
        },
        0
    );
    assert!(!handle.is_null());
    assert_eq!(
        unsafe { tv_inference_selected_provider(handle) },
        PROVIDER_CPU
    );
    unsafe { tv_inference_destroy(handle) };
}

/// Only builds with the `directml` feature and a DirectML runtime can show that an index the
/// adapter list does not contain fails registration and falls back to CPU without an error.
#[cfg(feature = "directml")]
#[test]
fn directml_out_of_range_device_index_falls_back_to_cpu() {
    let runtime_var = match std::env::var_os(DIRECTML_RUNTIME_ENV) {
        Some(value) => value,
        None => {
            eprintln!("skipping: set {DIRECTML_RUNTIME_ENV} to a DirectML onnxruntime.dll");
            return;
        }
    };
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime = CString::new(PathBuf::from(runtime_var).to_string_lossy().as_bytes()).unwrap();
    let model = model_path(&root);

    let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe {
            tv_inference_create_with_provider_device(
                runtime.as_ptr(),
                model.as_ptr(),
                PROVIDER_DIRECTML_CANDIDATE,
                99,
                &mut handle,
            )
        },
        0
    );
    assert!(!handle.is_null());
    assert_eq!(
        unsafe { tv_inference_selected_provider(handle) },
        PROVIDER_CPU
    );
    unsafe { tv_inference_destroy(handle) };
}

/// Does not load a runtime or create a detector, so it runs without the fetched runtime. Before any
/// successful create, the runtime is not initialised and availability must report 0.
#[test]
fn directml_availability_is_zero_before_runtime_is_loaded() {
    if directml_runtime_requested() {
        eprintln!("skipping: {DIRECTML_RUNTIME_ENV} binds the process to a DirectML runtime");
        return;
    }
    assert_eq!(unsafe { tv_inference_directml_available() }, 0);
}

/// Runs only with the `directml` feature and a DirectML runtime named by TV_TEST_DIRECTML_RUNTIME,
/// for example tools/inference/runtime/windows-x64-directml/onnxruntime.dll. Provider 2 must give
/// either a DirectML session or a CPU session, and the detection on the CC0 fixture must still
/// be a positive result. Compare the printed confidence with the CPU runtime run of the smoke test.
#[cfg(feature = "directml")]
#[test]
fn directml_runtime_detects_face_like_cpu_runtime() {
    let runtime_var = match std::env::var_os(DIRECTML_RUNTIME_ENV) {
        Some(value) => value,
        None => {
            eprintln!("skipping: set {DIRECTML_RUNTIME_ENV} to a DirectML onnxruntime.dll");
            return;
        }
    };
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime_path = PathBuf::from(runtime_var);
    assert!(
        runtime_path.is_file(),
        "{DIRECTML_RUNTIME_ENV} does not point to a file: {}",
        runtime_path.display()
    );
    let runtime = CString::new(runtime_path.to_string_lossy().as_bytes()).unwrap();
    let model = model_path(&root);

    // Nothing is loaded before the first create, so availability must be 0 here.
    assert_eq!(unsafe { tv_inference_directml_available() }, 0);

    let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe {
            tv_inference_create_with_provider(
                runtime.as_ptr(),
                model.as_ptr(),
                PROVIDER_DIRECTML_CANDIDATE,
                &mut handle,
            )
        },
        0
    );
    assert!(!handle.is_null());
    let selected = unsafe { tv_inference_selected_provider(handle) };
    let available = unsafe { tv_inference_directml_available() };
    eprintln!("directml runtime: selected_provider={selected} directml_available={available}");
    assert!(
        selected == PROVIDER_CPU || selected == PROVIDER_DIRECTML_CANDIDATE,
        "unexpected selected provider {selected}"
    );
    // A DirectML session can only exist when the runtime reports DirectML.
    assert!(available == 1 || selected == PROVIDER_CPU);

    // Same CC0 portrait and checks as the CPU smoke test, decoded from the same PPM fixture.
    let ppm = std::fs::read(root.join("crates/telaviva-inference/tests/fixtures/cc0-face.ppm"))
        .expect("CC0 face fixture is readable");
    assert!(ppm.starts_with(b"P6\n320 320\n255\n"));
    let rgb = &ppm[b"P6\n320 320\n255\n".len()..];
    assert_eq!(rgb.len(), 320 * 320 * 3);
    let mut face = Vec::with_capacity(320 * 320 * 4);
    for pixel in rgb.chunks_exact(3) {
        face.extend_from_slice(&[pixel[2], pixel[1], pixel[0], 255]);
    }
    let mut result = TvInferenceResult {
        status: -99,
        confidence: -1.0,
    };
    assert_eq!(
        unsafe {
            tv_inference_detect_bgra(
                handle,
                face.as_ptr(),
                face.len(),
                320,
                320,
                320 * 4,
                &mut result,
            )
        },
        1
    );
    assert_eq!(result.status, 1);
    assert!(result.confidence >= 0.5);
    eprintln!(
        "directml runtime: fixture status={} confidence={:.6} provider={selected}",
        result.status, result.confidence
    );
    unsafe { tv_inference_destroy(handle) };
}
