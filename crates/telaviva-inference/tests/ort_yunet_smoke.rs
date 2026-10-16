#![cfg(all(target_os = "macos", target_arch = "aarch64"))]

use std::{ffi::CString, path::PathBuf};
use telaviva_inference::{
    tv_inference_assess_light_bgra, tv_inference_coreml_available, tv_inference_create,
    tv_inference_create_with_provider, tv_inference_destroy, tv_inference_detect_bgra,
    tv_inference_selected_provider, TvInferenceHandle, TvInferenceResult,
};

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
