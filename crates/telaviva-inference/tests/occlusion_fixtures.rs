#![cfg(feature = "bench")]
//! Checks the production RetinaFace model against the occlusion benchmark fixtures.
//!
//! The test runs only when the bundled macOS ONNX Runtime is present and otherwise
//! prints a message and returns. The recorded results it guards are in
//! `specs/archived/masked-face-detection-and-lock-on-absence/validation/benchmark-retinaface.md`.

use std::{ffi::CString, path::PathBuf};
use telaviva_inference::{
    bench::parse_manifest, tv_inference_create, tv_inference_destroy, tv_inference_detect_bgra,
    TvInferenceHandle, TvInferenceResult,
};

/// Status returned by `tv_inference_detect_bgra` when a face is present.
const STATUS_PRESENT: i32 = 1;
/// The only face-free fixture allowed to be reported present: a recorded false positive.
const KNOWN_FALSE_POSITIVE: &str = "neg-hand-06.jpg";

#[test]
fn production_model_matches_the_recorded_fixture_results() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let runtime_path = root.join("tools/inference/runtime/macos-arm64/libonnxruntime.dylib");
    if !runtime_path.exists() {
        println!("skipped: {} does not exist", runtime_path.display());
        return;
    }
    let fixtures = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/occlusion");
    let model_path = root.join("assets/model/face_detection_retinaface_mb025.onnx");
    let runtime = CString::new(runtime_path.to_string_lossy().as_bytes()).unwrap();
    let model = CString::new(model_path.to_string_lossy().as_bytes()).unwrap();

    let mut handle: *mut TvInferenceHandle = std::ptr::null_mut();
    assert_eq!(
        unsafe { tv_inference_create(runtime.as_ptr(), model.as_ptr(), &mut handle) },
        0
    );
    assert!(!handle.is_null());

    let manifest_text =
        std::fs::read_to_string(fixtures.join("manifest.toml")).expect("read occlusion manifest");
    let entries = parse_manifest(&manifest_text).expect("parse occlusion manifest");

    let mut occluded_total = 0_u32;
    let mut occluded_present = 0_u32;
    let mut unoccluded_total = 0_u32;
    let mut unoccluded_present = 0_u32;
    let mut face_free_total = 0_u32;
    let mut face_free_present: Vec<String> = Vec::new();

    for entry in &entries {
        let rgba = image::open(fixtures.join(&entry.file))
            .unwrap_or_else(|err| panic!("decode {}: {err}", entry.file))
            .to_rgba8();
        let (width, height) = rgba.dimensions();
        let stride = width as usize * 4;
        // The image crate yields RGBA rows top-down; the detector takes BGRA.
        let bgra: Vec<u8> = rgba
            .pixels()
            .flat_map(|pixel| [pixel[2], pixel[1], pixel[0], pixel[3]])
            .collect();

        let mut result = TvInferenceResult::default();
        let status = unsafe {
            tv_inference_detect_bgra(
                handle,
                bgra.as_ptr(),
                bgra.len(),
                width,
                height,
                stride,
                &mut result,
            )
        };
        assert!(status >= 0, "{} returned error status {status}", entry.file);
        let present = status == STATUS_PRESENT;

        if !entry.has_face {
            face_free_total += 1;
            if present {
                face_free_present.push(entry.file.clone());
            }
        } else if entry.occlusion == "none" {
            unoccluded_total += 1;
            if present {
                unoccluded_present += 1;
            }
        } else {
            occluded_total += 1;
            if present {
                occluded_present += 1;
            }
        }
    }
    unsafe { tv_inference_destroy(handle) };

    println!("occluded faces present: {occluded_present} of {occluded_total}");
    println!("unoccluded faces present: {unoccluded_present} of {unoccluded_total}");
    println!(
        "face-free images present: {} of {face_free_total} {face_free_present:?}",
        face_free_present.len()
    );

    assert!(
        occluded_present + 1 >= occluded_total,
        "occluded faces present: {occluded_present} of {occluded_total}; at most one may be missed"
    );
    assert_eq!(
        unoccluded_present, unoccluded_total,
        "every unoccluded face must be present"
    );
    assert!(
        face_free_present.len() <= 1,
        "at most one face-free image may be present: {face_free_present:?}"
    );
    if let Some(file) = face_free_present.first() {
        assert_eq!(
            file.as_str(),
            KNOWN_FALSE_POSITIVE,
            "the only allowed face-free false positive is the recorded one"
        );
    }
}
