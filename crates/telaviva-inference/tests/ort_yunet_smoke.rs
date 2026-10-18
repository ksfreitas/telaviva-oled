#![cfg(all(target_os = "macos", target_arch = "aarch64"))]

use std::{ffi::CString, path::PathBuf};
use telaviva_inference::{
    tv_inference_assess_light_bgra, tv_inference_coreml_available, tv_inference_create,
    tv_inference_create_with_provider, tv_inference_create_with_provider_device,
    tv_inference_destroy, tv_inference_detect_bgra, tv_inference_selected_provider,
    TvInferenceHandle, TvInferenceResult,
};

#[test]
fn coreml_compute_units_match_cpu_fixture_and_report_latency_when_opted_in() {
    if std::env::var("TELAVIVA_RUN_COREML_PROBE").as_deref() != Ok("1") {
        return;
    }

    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime_path = root.join("tools/inference/runtime/macos-arm64/libonnxruntime.dylib");
    let model_path = root.join("assets/model/face_detection_yunet_2023mar.onnx");
    let frame_path = std::env::var_os("TELAVIVA_COREML_PROBE_FRAME")
        .expect("set TELAVIVA_COREML_PROBE_FRAME to the generated TVSF BGRA fixture");
    let frame = std::fs::read(frame_path).expect("read the generated self-test frame");
    assert!(frame.starts_with(b"TVSF"), "fixture has the TVSF header");
    let width = u32::from_le_bytes(frame[4..8].try_into().unwrap());
    let height = u32::from_le_bytes(frame[8..12].try_into().unwrap());
    let stride = u32::from_le_bytes(frame[12..16].try_into().unwrap()) as usize;
    let pixels = &frame[16..];
    assert_eq!(width, 320);
    assert_eq!(height, 240);
    assert_eq!(stride, width as usize * 4);
    assert_eq!(pixels.len(), stride * height as usize);

    let runtime = CString::new(runtime_path.to_string_lossy().as_bytes()).unwrap();
    let model = CString::new(model_path.to_string_lossy().as_bytes()).unwrap();
    let mut result = TvInferenceResult {
        status: -99,
        confidence: -1.0,
    };

    let mut cpu: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe { tv_inference_create(runtime.as_ptr(), model.as_ptr(), &mut cpu) },
        0
    );
    assert_eq!(unsafe { tv_inference_selected_provider(cpu) }, 0);
    let cpu_result = unsafe {
        tv_inference_detect_bgra(
            cpu,
            pixels.as_ptr(),
            pixels.len(),
            width,
            height,
            stride,
            &mut result,
        )
    };
    assert_eq!(
        cpu_result, 1,
        "the shared self-test fixture detects a face on CPU"
    );
    let cpu_confidence = result.confidence;
    let mut cpu_timings_ms = Vec::with_capacity(30);
    let mut cpu_first_ms = 0.0;
    for iteration in 0..30 {
        let started = std::time::Instant::now();
        let detected = unsafe {
            tv_inference_detect_bgra(
                cpu,
                pixels.as_ptr(),
                pixels.len(),
                width,
                height,
                stride,
                &mut result,
            )
        };
        let elapsed = started.elapsed().as_secs_f64() * 1000.0;
        if iteration == 0 {
            cpu_first_ms = elapsed;
        }
        cpu_timings_ms.push(elapsed);
        assert_eq!(detected, cpu_result);
        assert_eq!(detected, cpu_result);
        assert!((result.confidence - cpu_confidence).abs() <= f32::EPSILON);
    }
    cpu_timings_ms.sort_by(f64::total_cmp);
    eprintln!(
        "CPU: selected_provider=0, status={}, confidence={:.4}, first={:.2} ms, p50={:.2} ms, p95={:.2} ms, max={:.2} ms",
        result.status,
        result.confidence,
        cpu_first_ms,
        cpu_timings_ms[15],
        cpu_timings_ms[28],
        cpu_timings_ms[29],
    );
    assert_eq!(unsafe { tv_inference_coreml_available() }, 1);
    unsafe { tv_inference_destroy(cpu) };

    for (index, label) in [(0, "ALL"), (1, "CPUAndNeuralEngine"), (2, "CPUAndGPU")] {
        let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                tv_inference_create_with_provider_device(
                    runtime.as_ptr(),
                    model.as_ptr(),
                    1,
                    index,
                    &mut handle,
                )
            },
            0,
            "create CoreML {label} session"
        );
        assert_eq!(unsafe { tv_inference_selected_provider(handle) }, 1);

        let mut timings_ms = Vec::with_capacity(30);
        let mut first_ms = 0.0;
        for iteration in 0..30 {
            let started = std::time::Instant::now();
            let detected = unsafe {
                tv_inference_detect_bgra(
                    handle,
                    pixels.as_ptr(),
                    pixels.len(),
                    width,
                    height,
                    stride,
                    &mut result,
                )
            };
            let elapsed = started.elapsed().as_secs_f64() * 1000.0;
            if iteration == 0 {
                first_ms = elapsed;
            }
            timings_ms.push(elapsed);
            assert_eq!(
                detected, cpu_result,
                "CoreML {label} agrees with CPU status"
            );
            assert!((result.confidence - cpu_confidence).abs() <= 0.05);
        }
        timings_ms.sort_by(f64::total_cmp);
        eprintln!(
            "CoreML {label}: selected_provider=1, status={}, confidence={:.4}, first={:.2} ms, p50={:.2} ms, p95={:.2} ms, max={:.2} ms",
            result.status,
            result.confidence,
            first_ms,
            timings_ms[15],
            timings_ms[28],
            timings_ms[29],
        );
        unsafe { tv_inference_destroy(handle) };
    }
}

#[test]
fn bundled_runtime_executes_pinned_yunet_model_on_cpu() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime = CString::new(
        root.join("tools/inference/runtime/macos-arm64/libonnxruntime.dylib")
            .to_string_lossy()
            .as_bytes(),
    )
    .unwrap();
    let model = CString::new(
        root.join("assets/model/face_detection_yunet_2023mar.onnx")
            .to_string_lossy()
            .as_bytes(),
    )
    .unwrap();
    let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe { tv_inference_create(runtime.as_ptr(), model.as_ptr(), &mut handle) },
        0
    );
    assert!(!handle.is_null());
    let coreml_available = unsafe { tv_inference_coreml_available() };
    assert!((0..=1).contains(&coreml_available));

    let dark = vec![0_u8; 320 * 320 * 4];
    assert_eq!(
        unsafe { tv_inference_assess_light_bgra(dark.as_ptr(), dark.len(), 320, 320, 320 * 4) },
        2
    );
    let mut result = TvInferenceResult {
        status: -99,
        confidence: -1.0,
    };
    assert_eq!(
        unsafe {
            tv_inference_detect_bgra(
                handle,
                dark.as_ptr(),
                dark.len(),
                0,
                320,
                320 * 4,
                &mut result,
            )
        },
        -1
    );
    assert_eq!(result.status, -1);
    assert_eq!(result.confidence, 0.0);
    assert_eq!(
        unsafe {
            tv_inference_detect_bgra(
                handle,
                dark.as_ptr(),
                dark.len(),
                320,
                320,
                320 * 4,
                &mut result,
            )
        },
        2
    );
    assert_eq!(result.status, 2);

    let mut neutral = vec![128_u8; 320 * 320 * 4];
    for alpha in neutral.iter_mut().skip(3).step_by(4) {
        *alpha = 255;
    }
    assert_eq!(
        unsafe {
            tv_inference_assess_light_bgra(neutral.as_ptr(), neutral.len(), 320, 320, 320 * 4)
        },
        0
    );
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
    assert_eq!(result.confidence, 0.0);

    // A pinned, openly licensed portrait exercises the positive detector path with
    // the real ONNX graph and runtime. The fixture is a 320x320 PPM derived from
    // Wikimedia Commons' CC0 portrait documented alongside the fixture.
    let ppm = include_bytes!("fixtures/cc0-face.ppm");
    assert!(ppm.starts_with(b"P6\n320 320\n255\n"));
    let rgb = &ppm[b"P6\n320 320\n255\n".len()..];
    assert_eq!(rgb.len(), 320 * 320 * 3);
    let mut face = Vec::with_capacity(320 * 320 * 4);
    for pixel in rgb.chunks_exact(3) {
        face.extend_from_slice(&[pixel[2], pixel[1], pixel[0], 255]);
    }
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
    assert!(result.confidence >= 0.8);
    unsafe { tv_inference_destroy(handle) };

    let mut candidate: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe {
            tv_inference_create_with_provider(runtime.as_ptr(), model.as_ptr(), 1, &mut candidate)
        },
        0
    );
    let selected_provider = unsafe { tv_inference_selected_provider(candidate) };
    assert!((0..=1).contains(&selected_provider));
    println!(
        "CoreML available: {coreml_available}; selected provider candidate: {selected_provider}"
    );
    assert_eq!(
        unsafe {
            tv_inference_detect_bgra(
                candidate,
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
    unsafe { tv_inference_destroy(candidate) };
}
