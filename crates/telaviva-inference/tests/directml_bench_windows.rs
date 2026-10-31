#![cfg(all(target_os = "windows", target_arch = "x86_64"))]
//! Latency measurement for spike S4: the CPU baseline and DirectML on each adapter index.
//!
//! These tests never fail a default CI run. Each one returns early with a message unless its
//! environment variable is set, and the output lines are printed to stdout, so run them with
//! `--nocapture`:
//!
//! - `TV_TEST_DIRECTML_RUNTIME=<path>` with `--features directml` runs `bench_directml_adapters`
//!   on the DirectML runtime, for device indexes 0 to 3.
//! - `TV_TEST_CPU_BASELINE=1` runs `bench_cpu_baseline` on the bundled CPU runtime.
//!
//! ONNX Runtime binds one runtime per process, so run the two in separate processes, for example
//! `cargo test --features directml --test directml_bench_windows -- --nocapture` for each variable.
//! Setting both variables in one process makes both tests skip.

use std::{
    ffi::CString,
    path::{Path, PathBuf},
    ptr,
    time::Instant,
};
use telaviva_inference::{
    tv_inference_create_with_provider_device, tv_inference_destroy, tv_inference_detect_bgra,
    tv_inference_selected_provider, TvInferenceHandle, TvInferenceResult,
};

const DIRECTML_RUNTIME_ENV: &str = "TV_TEST_DIRECTML_RUNTIME";
const CPU_BASELINE_ENV: &str = "TV_TEST_CPU_BASELINE";
const CPU_RUNTIME: &str = "tools/inference/runtime/windows-x64/onnxruntime.dll";
const MODEL: &str = "assets/model/face_detection_retinaface_mb025.onnx";
const FIXTURE: &str = "crates/telaviva-inference/tests/fixtures/cc0-face.ppm";
const PPM_HEADER: &[u8] = b"P6\n320 320\n255\n";
const WARMUP_RUNS: usize = 3;
const TIMED_RUNS: usize = 50;

/// Timing and result of one detector on the CC0 fixture.
struct Stats {
    mean_ms: f64,
    p95_ms: f64,
    status: i32,
    confidence: f32,
}

fn repository_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn c_path(path: &Path) -> CString {
    CString::new(path.to_string_lossy().as_bytes()).expect("paths contain no NUL bytes")
}

/// Decodes the CC0 fixture to top-down BGRA, as the smoke test does.
fn fixture_frame(root: &Path) -> Vec<u8> {
    let ppm = std::fs::read(root.join(FIXTURE)).expect("CC0 face fixture is readable");
    assert!(ppm.starts_with(PPM_HEADER));
    let rgb = &ppm[PPM_HEADER.len()..];
    assert_eq!(rgb.len(), 320 * 320 * 3);
    let mut bgra = Vec::with_capacity(320 * 320 * 4);
    for pixel in rgb.chunks_exact(3) {
        bgra.extend_from_slice(&[pixel[2], pixel[1], pixel[0], 255]);
    }
    bgra
}

/// Runs warm-up detections, then timed ones, and returns the mean and nearest-rank p95 in
/// milliseconds. Every call must return a non-negative status, so a failing detector is reported
/// as a panic rather than as a fast time.
fn measure(handle: *mut TvInferenceHandle, frame: &[u8]) -> Stats {
    let mut result = TvInferenceResult {
        status: -99,
        confidence: -1.0,
    };
    let detect = |result: &mut TvInferenceResult| unsafe {
        tv_inference_detect_bgra(
            handle,
            frame.as_ptr(),
            frame.len(),
            320,
            320,
            320 * 4,
            result,
        )
    };
    for _ in 0..WARMUP_RUNS {
        let code = detect(&mut result);
        assert!(code >= 0, "warm-up detection failed with {code}");
    }
    let mut samples = Vec::with_capacity(TIMED_RUNS);
    for _ in 0..TIMED_RUNS {
        let start = Instant::now();
        let code = detect(&mut result);
        samples.push(start.elapsed().as_secs_f64() * 1000.0);
        assert!(code >= 0, "timed detection failed with {code}");
        assert_eq!(code, result.status);
    }
    samples.sort_by(f64::total_cmp);
    let mean_ms = samples.iter().sum::<f64>() / samples.len() as f64;
    let rank = (samples.len() as f64 * 0.95).ceil() as usize;
    Stats {
        mean_ms,
        p95_ms: samples[rank - 1],
        status: result.status,
        confidence: result.confidence,
    }
}

/// CPU baseline on the bundled CPU runtime. Runs in its own process, see the module comment.
#[test]
fn bench_cpu_baseline() {
    if std::env::var(CPU_BASELINE_ENV).as_deref() != Ok("1") {
        eprintln!("skipping: set {CPU_BASELINE_ENV}=1 to run the CPU baseline");
        return;
    }
    if cfg!(feature = "directml") && std::env::var_os(DIRECTML_RUNTIME_ENV).is_some() {
        eprintln!("skipping: {DIRECTML_RUNTIME_ENV} binds the process to a DirectML runtime");
        return;
    }
    let root = repository_root();
    let runtime_path = root.join(CPU_RUNTIME);
    if !runtime_path.is_file() {
        eprintln!(
            "skipping: ONNX Runtime has not been fetched at {}",
            runtime_path.display()
        );
        return;
    }
    let runtime = c_path(&runtime_path);
    let model = c_path(&root.join(MODEL));
    let frame = fixture_frame(&root);

    let mut handle: *mut TvInferenceHandle = ptr::null_mut();
    assert_eq!(
        unsafe {
            tv_inference_create_with_provider_device(
                runtime.as_ptr(),
                model.as_ptr(),
                0,
                0,
                &mut handle,
            )
        },
        0
    );
    assert!(!handle.is_null());
    assert_eq!(unsafe { tv_inference_selected_provider(handle) }, 0);
    let stats = measure(handle, &frame);
    println!(
        "bench provider=0 mean_ms={:.3} p95_ms={:.3} status={} confidence={:.6}",
        stats.mean_ms, stats.p95_ms, stats.status, stats.confidence
    );
    unsafe { tv_inference_destroy(handle) };
}

/// DirectML on device indexes 0 to 3 of the DirectML runtime. The loop stops at the first index
/// that reports selected provider 0, which means registration failed or the index does not exist.
/// That index is still printed, with selected=0, so the cause is visible in the output.
#[cfg(feature = "directml")]
#[test]
fn bench_directml_adapters() {
    const PROVIDER_DIRECTML_CANDIDATE: i32 = 2;
    const PROVIDER_CPU: i32 = 0;
    const DEVICE_INDEXES: std::ops::Range<u32> = 0..4;

    let runtime_var = match std::env::var_os(DIRECTML_RUNTIME_ENV) {
        Some(value) => value,
        None => {
            eprintln!("skipping: set {DIRECTML_RUNTIME_ENV} to a DirectML onnxruntime.dll");
            return;
        }
    };
    if std::env::var_os(CPU_BASELINE_ENV).is_some() {
        eprintln!("skipping: {CPU_BASELINE_ENV} binds the process to the CPU runtime");
        return;
    }
    let root = repository_root();
    let runtime_path = PathBuf::from(runtime_var);
    assert!(
        runtime_path.is_file(),
        "{DIRECTML_RUNTIME_ENV} does not point to a file: {}",
        runtime_path.display()
    );
    let runtime = c_path(&runtime_path);
    let model = c_path(&root.join(MODEL));
    let frame = fixture_frame(&root);

    for device_index in DEVICE_INDEXES {
        let mut handle: *mut TvInferenceHandle = ptr::null_mut();
        assert_eq!(
            unsafe {
                tv_inference_create_with_provider_device(
                    runtime.as_ptr(),
                    model.as_ptr(),
                    PROVIDER_DIRECTML_CANDIDATE,
                    device_index,
                    &mut handle,
                )
            },
            0,
            "create must not fail for device index {device_index}"
        );
        assert!(!handle.is_null());
        let selected = unsafe { tv_inference_selected_provider(handle) };
        let stats = measure(handle, &frame);
        println!(
            "bench provider=2 device={device_index} selected={selected} mean_ms={:.3} p95_ms={:.3} status={} confidence={:.6}",
            stats.mean_ms, stats.p95_ms, stats.status, stats.confidence
        );
        unsafe { tv_inference_destroy(handle) };
        if selected == PROVIDER_CPU {
            break;
        }
    }
}
