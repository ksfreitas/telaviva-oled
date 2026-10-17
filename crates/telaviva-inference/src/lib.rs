use ndarray::Array4;
use ort::{
    execution_providers::{CoreMLExecutionProvider, ExecutionProvider},
    session::{builder::GraphOptimizationLevel, Session},
};
use ort_sys as _;
use sha2::{Digest, Sha256};
use std::{
    ffi::{c_char, CStr},
    panic::{catch_unwind, AssertUnwindSafe},
    path::Path,
    ptr, slice,
    sync::{Mutex, OnceLock},
};

const MODEL_SHA256: &str = "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4";
// The pinned upstream 2023mar graph has static [1, 3, 640, 640] input dims.
const INPUT: usize = 640;
const CONFIDENCE_THRESHOLD: f32 = 0.8;
const NMS_IOU_THRESHOLD: f32 = 0.3;
const MIN_MEAN_LUMA: f32 = 18.0;
const MAX_DIMENSION: u32 = 8192;
const MAX_FRAME_BYTES: usize = 256 * 1024 * 1024;

#[repr(C)]
pub struct TvInferenceHandle {
    session: Mutex<Session>,
    selected_provider: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TvInferenceResult {
    pub status: i32,
    pub confidence: f32,
}

const ABSENT: i32 = 0;
const PRESENT: i32 = 1;
const INSUFFICIENT_LIGHT: i32 = 2;
const UNAVAILABLE: i32 = 3;
const SUCCESS: i32 = 0;
const INVALID_ARGUMENT: i32 = -1;
const RUNTIME_ERROR: i32 = -2;
const MODEL_ERROR: i32 = -3;
const PANIC_ERROR: i32 = -4;
const PROVIDER_CPU: i32 = 0;
const PROVIDER_COREML_CANDIDATE: i32 = 1;

static RUNTIME_PATH: OnceLock<String> = OnceLock::new();

#[derive(Clone, Copy, Debug)]
struct Box2d {
    x: f32,
    y: f32,
    w: f32,
    h: f32,
    score: f32,
}

/// Creates an inference session bound to the app-bundled runtime and pinned model.
///
/// # Safety
/// All pointers must be valid for reads/writes as appropriate and point to NUL-terminated UTF-8 paths.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_create(
    runtime_abs_path: *const c_char,
    model_abs_path: *const c_char,
    out_handle: *mut *mut TvInferenceHandle,
) -> i32 {
    tv_inference_create_with_provider(runtime_abs_path, model_abs_path, PROVIDER_CPU, out_handle)
}

/// Creates a session with an optional CoreML provider candidate and CPU fallback.
/// The existing create function remains CPU-only; applications should request CoreML
/// only for a device profile that has measured an energy benefit.
///
/// # Safety
/// All pointers must be valid for reads/writes as appropriate and point to NUL-terminated UTF-8 paths.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_create_with_provider(
    runtime_abs_path: *const c_char,
    model_abs_path: *const c_char,
    provider: i32,
    out_handle: *mut *mut TvInferenceHandle,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if runtime_abs_path.is_null() || model_abs_path.is_null() || out_handle.is_null() {
            return INVALID_ARGUMENT;
        }
        if provider != PROVIDER_CPU && provider != PROVIDER_COREML_CANDIDATE {
            return INVALID_ARGUMENT;
        }
        *out_handle = ptr::null_mut();
        let runtime = match c_path(runtime_abs_path) {
            Some(path) => path,
            None => return INVALID_ARGUMENT,
        };
        let model = match c_path(model_abs_path) {
            Some(path) => path,
            None => return INVALID_ARGUMENT,
        };
        if !runtime.is_absolute() || !model.is_absolute() {
            return INVALID_ARGUMENT;
        }
        if !runtime.is_file() || !model.is_file() {
            return UNAVAILABLE;
        }
        let digest = match sha256_file(&model) {
            Ok(value) => value,
            Err(()) => return MODEL_ERROR,
        };
        if digest != MODEL_SHA256 {
            return MODEL_ERROR;
        }
        let runtime_text = runtime.to_string_lossy().into_owned();
        if let Some(existing) = RUNTIME_PATH.get() {
            if existing != &runtime_text {
                return RUNTIME_ERROR;
            }
        } else {
            let _ = RUNTIME_PATH.set(runtime_text.clone());
        }
        if RUNTIME_PATH.get().map(String::as_str) != Some(runtime_text.as_str()) {
            return RUNTIME_ERROR;
        }

        let init = ort::init_from(runtime_text)
            .with_name("TelaViva YuNet CPU")
            .commit();
        if init.is_err() {
            return RUNTIME_ERROR;
        }
        let mut selected_provider = PROVIDER_CPU;
        let coreml_available = provider == PROVIDER_COREML_CANDIDATE
            && CoreMLExecutionProvider::default()
                .is_available()
                .unwrap_or(false);
        let session = if coreml_available {
            match create_session(&model, true) {
                Ok(session) => {
                    selected_provider = PROVIDER_COREML_CANDIDATE;
                    Ok(session)
                }
                Err(_) => create_session(&model, false),
            }
        } else {
            create_session(&model, false)
        };
        let session = match session {
            Ok(session) => session,
            Err(_) => return MODEL_ERROR,
        };
        if !valid_model_io(&session) {
            return MODEL_ERROR;
        }
        *out_handle = Box::into_raw(Box::new(TvInferenceHandle {
            session: Mutex::new(session),
            selected_provider,
        }));
        SUCCESS
    }))
    .unwrap_or(PANIC_ERROR)
}

fn create_session(model: &Path, coreml_candidate: bool) -> ort::Result<Session> {
    let builder = Session::builder()?
        .with_intra_threads(1)?
        .with_inter_threads(1)?
        .with_intra_op_spinning(false)?
        .with_inter_op_spinning(false)?
        .with_optimization_level(GraphOptimizationLevel::Level3)?;
    let builder = if coreml_candidate {
        builder.with_execution_providers([CoreMLExecutionProvider::default().build()])?
    } else {
        builder
    };
    builder.commit_from_file(model)
}

/// Assesses an owned BGRA frame and runs YuNet unless the frame is too dark.
///
/// # Safety
/// `handle` must be a live handle from `tv_inference_create`; `bytes` must address `len`
/// readable bytes; `out_result` must point to a writable result. The caller must not
/// destroy the handle during this call.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_detect_bgra(
    handle: *mut TvInferenceHandle,
    bytes: *const u8,
    len: usize,
    width: u32,
    height: u32,
    stride: usize,
    out_result: *mut TvInferenceResult,
) -> i32 {
    let status = catch_unwind(AssertUnwindSafe(|| unsafe {
        if handle.is_null() || bytes.is_null() || out_result.is_null() {
            return INVALID_ARGUMENT;
        }
        *out_result = TvInferenceResult {
            status: RUNTIME_ERROR,
            confidence: 0.0,
        };
        let required = match validate_frame(len, width, height, stride) {
            Some(value) => value,
            None => {
                (*out_result).status = INVALID_ARGUMENT;
                return INVALID_ARGUMENT;
            }
        };
        let pixels = slice::from_raw_parts(bytes, required);
        if mean_luma(pixels, width as usize, height as usize, stride) < MIN_MEAN_LUMA {
            *out_result = TvInferenceResult {
                status: INSUFFICIENT_LIGHT,
                confidence: 0.0,
            };
            return INSUFFICIENT_LIGHT;
        }
        let session = &*handle;
        let session = match session.session.lock() {
            Ok(session) => session,
            Err(_) => return RUNTIME_ERROR,
        };
        let tensor = match preprocess_bgra(pixels, width as usize, height as usize, stride) {
            Some(tensor) => tensor,
            None => return INVALID_ARGUMENT,
        };
        let inputs = match ort::inputs!["input" => tensor] {
            Ok(inputs) => inputs,
            Err(_) => return RUNTIME_ERROR,
        };
        let outputs = match session.run(inputs) {
            Ok(outputs) => outputs,
            Err(_) => return RUNTIME_ERROR,
        };
        let detections = match decode_outputs(&outputs) {
            Some(detections) => detections,
            None => return MODEL_ERROR,
        };
        let confidence = detections
            .iter()
            .map(|detection| detection.score)
            .fold(0.0_f32, f32::max);
        let status = if confidence >= CONFIDENCE_THRESHOLD {
            PRESENT
        } else {
            ABSENT
        };
        *out_result = TvInferenceResult { status, confidence };
        status
    }))
    .unwrap_or(PANIC_ERROR);
    // Keep the out parameter consistent with the return code on every error path,
    // including a panic after the result was initialized.
    if status < 0 && !out_result.is_null() {
        unsafe {
            *out_result = TvInferenceResult {
                status,
                confidence: 0.0,
            };
        }
    }
    status
}

/// Destroys a handle returned by `tv_inference_create`.
///
/// # Safety
/// `handle` must be null or a live handle returned by `tv_inference_create`, and no
/// other call may be using the handle concurrently.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_destroy(handle: *mut TvInferenceHandle) {
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        if !handle.is_null() {
            drop(Box::from_raw(handle));
        }
    }));
}

/// Evaluates image illumination without loading or running the YuNet session.
///
/// # Safety
/// `bytes` must address `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_assess_light_bgra(
    bytes: *const u8,
    len: usize,
    width: u32,
    height: u32,
    stride: usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if bytes.is_null() {
            return INVALID_ARGUMENT;
        }
        let required = match validate_frame(len, width, height, stride) {
            Some(value) => value,
            None => return INVALID_ARGUMENT,
        };
        let pixels = slice::from_raw_parts(bytes, required);
        if mean_luma(pixels, width as usize, height as usize, stride) < MIN_MEAN_LUMA {
            INSUFFICIENT_LIGHT
        } else {
            SUCCESS
        }
    }))
    .unwrap_or(PANIC_ERROR)
}

/// Reports whether the currently loaded ONNX Runtime exposes CoreML on this device.
/// This is capability discovery only; the detector remains on CPU unless a separately
/// validated profile explicitly opts into CoreML.
///
/// # Safety
/// Call only after successful `tv_inference_create` initialized the runtime.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_coreml_available() -> i32 {
    catch_unwind(AssertUnwindSafe(
        || match CoreMLExecutionProvider::default().is_available() {
            Ok(true) => PRESENT,
            Ok(false) => ABSENT,
            Err(_) => RUNTIME_ERROR,
        },
    ))
    .unwrap_or(PANIC_ERROR)
}

/// Reports the provider candidate used when this session was built; CPU is 0, CoreML candidate is 1.
/// This indicates successful EP session registration, not that all model nodes run on CoreML.
///
/// # Safety
/// `handle` must be a live handle returned by a create function, and not concurrently destroyed.
#[no_mangle]
pub unsafe extern "C" fn tv_inference_selected_provider(handle: *const TvInferenceHandle) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if handle.is_null() {
            return INVALID_ARGUMENT;
        }
        (*handle).selected_provider
    }))
    .unwrap_or(PANIC_ERROR)
}

unsafe fn c_path(pointer: *const c_char) -> Option<std::path::PathBuf> {
    CStr::from_ptr(pointer)
        .to_str()
        .ok()
        .map(Path::new)
        .map(Path::to_path_buf)
}

fn valid_model_io(session: &Session) -> bool {
    if session.inputs.len() != 1 || session.inputs[0].name != "input" {
        return false;
    }
    let expected = [
        "cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32", "bbox_8", "bbox_16", "bbox_32",
        "kps_8", "kps_16", "kps_32",
    ];
    expected
        .iter()
        .all(|name| session.outputs.iter().any(|output| output.name == *name))
}

fn validate_frame(len: usize, width: u32, height: u32, stride: usize) -> Option<usize> {
    if width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION {
        return None;
    }
    let row_bytes = (width as usize).checked_mul(4)?;
    if stride < row_bytes {
        return None;
    }
    let required = stride.checked_mul(height as usize)?;
    if required > MAX_FRAME_BYTES || len < required {
        return None;
    }
    Some(required)
}

fn mean_luma(bgra: &[u8], width: usize, height: usize, stride: usize) -> f32 {
    let sample_x = width.div_ceil(64).max(1);
    let sample_y = height.div_ceil(64).max(1);
    let mut total = 0.0_f64;
    let mut samples = 0_u64;
    for y in (0..height).step_by(sample_y) {
        for x in (0..width).step_by(sample_x) {
            let pixel = y * stride + x * 4;
            let b = bgra[pixel] as f64;
            let g = bgra[pixel + 1] as f64;
            let r = bgra[pixel + 2] as f64;
            total += 0.0722 * b + 0.7152 * g + 0.2126 * r;
            samples += 1;
        }
    }
    (total / samples.max(1) as f64) as f32
}

/// Bilinear sample positions along one axis as (lower, upper, fraction), shared by rows and columns.
fn axis_samples(resized: usize, source: usize) -> Vec<(usize, usize, f32)> {
    (0..resized)
        .map(|index| {
            let position = ((index as f32 + 0.5) * source as f32 / resized as f32 - 0.5)
                .clamp(0.0, (source - 1) as f32);
            let lower = position.floor() as usize;
            let upper = (lower + 1).min(source - 1);
            (lower, upper, position - lower as f32)
        })
        .collect()
}

fn preprocess_bgra(bgra: &[u8], width: usize, height: usize, stride: usize) -> Option<Array4<f32>> {
    let scale = (INPUT as f32 / width as f32).min(INPUT as f32 / height as f32);
    let resized_w = ((width as f32 * scale).round() as usize).clamp(1, INPUT);
    let resized_h = ((height as f32 * scale).round() as usize).clamp(1, INPUT);
    let pad_x = (INPUT - resized_w) / 2;
    let pad_y = (INPUT - resized_h) / 2;
    let mut data = vec![0.0_f32; 3 * INPUT * INPUT];
    if resized_w == width && resized_h == height {
        // Identity letterbox: each output pixel is exactly one source pixel.
        for y in 0..height {
            for x in 0..width {
                let src = y * stride + x * 4;
                let dst = (y + pad_y) * INPUT + x + pad_x;
                for (channel, bgra_index) in [0, 1, 2].into_iter().enumerate() {
                    data[channel * INPUT * INPUT + dst] = bgra[src + bgra_index] as f32;
                }
            }
        }
    } else {
        let rows = axis_samples(resized_h, height);
        let columns = axis_samples(resized_w, width);
        for (y, &(y0, y1, fy)) in rows.iter().enumerate() {
            for (x, &(x0, x1, fx)) in columns.iter().enumerate() {
                let dst = (y + pad_y) * INPUT + x + pad_x;
                for (channel, bgra_index) in [0, 1, 2].into_iter().enumerate() {
                    let p00 = bgra[y0 * stride + x0 * 4 + bgra_index] as f32;
                    let p01 = bgra[y0 * stride + x1 * 4 + bgra_index] as f32;
                    let p10 = bgra[y1 * stride + x0 * 4 + bgra_index] as f32;
                    let p11 = bgra[y1 * stride + x1 * 4 + bgra_index] as f32;
                    let top = p00 + (p01 - p00) * fx;
                    let bottom = p10 + (p11 - p10) * fx;
                    data[channel * INPUT * INPUT + dst] = top + (bottom - top) * fy;
                }
            }
        }
    }
    Array4::from_shape_vec((1, 3, INPUT, INPUT), data).ok()
}

// Output names indexed by position in [8, 16, 32].
const CLS_OUTPUTS: [&str; 3] = ["cls_8", "cls_16", "cls_32"];
const OBJ_OUTPUTS: [&str; 3] = ["obj_8", "obj_16", "obj_32"];
const BBOX_OUTPUTS: [&str; 3] = ["bbox_8", "bbox_16", "bbox_32"];

fn decode_outputs(outputs: &ort::session::SessionOutputs<'_, '_>) -> Option<Vec<Box2d>> {
    let mut detections = Vec::new();
    for (level, stride) in [8_usize, 16, 32].into_iter().enumerate() {
        let side = INPUT / stride;
        let count = side * side;
        let cls_view = outputs
            .get(CLS_OUTPUTS[level])?
            .try_extract_tensor::<f32>()
            .ok()?;
        let obj_view = outputs
            .get(OBJ_OUTPUTS[level])?
            .try_extract_tensor::<f32>()
            .ok()?;
        let bbox_view = outputs
            .get(BBOX_OUTPUTS[level])?
            .try_extract_tensor::<f32>()
            .ok()?;
        let cls = cls_view.as_slice()?;
        let obj = obj_view.as_slice()?;
        let bbox = bbox_view.as_slice()?;
        if cls.len() != count || obj.len() != count || bbox.len() != count * 4 {
            return None;
        }
        for index in 0..count {
            let score = (cls[index].clamp(0.0, 1.0) * obj[index].clamp(0.0, 1.0)).sqrt();
            if !score.is_finite() || score < CONFIDENCE_THRESHOLD {
                continue;
            }
            let row = index / side;
            let column = index % side;
            if let Some(detection) = decode_candidate(
                stride,
                row,
                column,
                score,
                [
                    bbox[index * 4],
                    bbox[index * 4 + 1],
                    bbox[index * 4 + 2],
                    bbox[index * 4 + 3],
                ],
            ) {
                detections.push(detection);
            }
        }
    }
    Some(non_maximum_suppression(detections))
}

fn decode_candidate(
    stride: usize,
    row: usize,
    column: usize,
    score: f32,
    bbox: [f32; 4],
) -> Option<Box2d> {
    let scale = stride as f32;
    let cx = (column as f32 + bbox[0]) * scale;
    let cy = (row as f32 + bbox[1]) * scale;
    let w = bbox[2].exp() * scale;
    let h = bbox[3].exp() * scale;
    if ![cx, cy, w, h, score].iter().all(|value| value.is_finite()) || w <= 0.0 || h <= 0.0 {
        return None;
    }
    Some(Box2d {
        x: cx - w / 2.0,
        y: cy - h / 2.0,
        w,
        h,
        score,
    })
}

fn non_maximum_suppression(mut boxes: Vec<Box2d>) -> Vec<Box2d> {
    boxes.sort_by(|left, right| right.score.total_cmp(&left.score));
    boxes.truncate(5000);
    let mut kept: Vec<Box2d> = Vec::new();
    'candidate: for candidate in boxes {
        for accepted in &kept {
            if iou(candidate, *accepted) > NMS_IOU_THRESHOLD {
                continue 'candidate;
            }
        }
        kept.push(candidate);
        if kept.len() == 5000 {
            break;
        }
    }
    kept
}

fn iou(a: Box2d, b: Box2d) -> f32 {
    let left = a.x.max(b.x);
    let top = a.y.max(b.y);
    let right = (a.x + a.w).min(b.x + b.w);
    let bottom = (a.y + a.h).min(b.y + b.h);
    let intersection = (right - left).max(0.0) * (bottom - top).max(0.0);
    intersection / (a.w * a.h + b.w * b.h - intersection).max(f32::EPSILON)
}

fn sha256_file(path: &Path) -> Result<String, ()> {
    use std::io::Read;
    let file = std::fs::File::open(path).map_err(|_| ())?;
    let size = file.metadata().map_err(|_| ())?.len();
    if size == 0 || size > 16 * 1024 * 1024 {
        return Err(());
    }
    let mut data = Vec::with_capacity(size as usize);
    file.take(size).read_to_end(&mut data).map_err(|_| ())?;
    if data.len() as u64 != size {
        return Err(());
    }
    Ok(sha256(&data))
}

fn sha256(input: &[u8]) -> String {
    Sha256::digest(input)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

#[cfg(test)]
mod header_contract_tests;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_frame_lengths_dimensions_stride_and_caps() {
        assert_eq!(validate_frame(32, 2, 2, 8), Some(16));
        assert_eq!(validate_frame(15, 2, 2, 8), None);
        assert_eq!(validate_frame(16, 2, 2, 7), None);
        assert_eq!(validate_frame(16, 0, 2, 8), None);
        assert_eq!(
            validate_frame(usize::MAX, MAX_DIMENSION + 1, 2, usize::MAX),
            None
        );
    }

    #[test]
    fn decodes_yunet_grid_predictions_and_applies_nms() {
        let decoded =
            decode_candidate(8, 2, 3, 0.81, [0.25, 0.5, 2.0_f32.ln(), 4.0_f32.ln()]).unwrap();
        assert!((decoded.x - 18.0).abs() < 1e-5);
        assert!((decoded.y - 4.0).abs() < 1e-5);
        assert!((decoded.w - 16.0).abs() < 1e-5);
        assert!((decoded.h - 32.0).abs() < 1e-5);
        assert_eq!(decoded.score, 0.81);
        let mut detections = vec![
            Box2d {
                x: 10.0,
                y: 10.0,
                w: 40.0,
                h: 40.0,
                score: 0.95,
            },
            Box2d {
                x: 11.0,
                y: 11.0,
                w: 40.0,
                h: 40.0,
                score: 0.90,
            },
            Box2d {
                x: 100.0,
                y: 100.0,
                w: 20.0,
                h: 20.0,
                score: 0.85,
            },
        ];
        detections.sort_by(|a, b| b.score.total_cmp(&a.score));
        let kept = non_maximum_suppression(detections);
        assert_eq!(kept.len(), 2);
        assert_eq!(kept[0].score, 0.95);
        assert_eq!(kept[1].score, 0.85);
        assert!((iou(kept[0], kept[1]) - 0.0).abs() < f32::EPSILON);
    }

    #[test]
    fn preprocesses_bgra_to_bgr_nchw_and_letterboxes() {
        let pixels = [10_u8, 20, 30, 255, 10, 20, 30, 255];
        let tensor = preprocess_bgra(&pixels, 2, 1, 8).unwrap();
        assert_eq!(tensor.shape(), &[1, 3, 640, 640]);
        let blue_plane = tensor.slice(ndarray::s![0, 0, 160, 100]);
        let red_plane = tensor.slice(ndarray::s![0, 2, 160, 100]);
        assert!((red_plane[()] - 30.0).abs() < 1e-6);
        assert!((blue_plane[()] - 10.0).abs() < 1e-6);
        assert_eq!(tensor[[0, 0, 0, 0]], 0.0);
    }

    /// Verbatim copy of the pre-optimization per-pixel letterbox, used as the oracle.
    fn preprocess_reference(
        bgra: &[u8],
        width: usize,
        height: usize,
        stride: usize,
    ) -> Option<Array4<f32>> {
        let scale = (INPUT as f32 / width as f32).min(INPUT as f32 / height as f32);
        let resized_w = ((width as f32 * scale).round() as usize).clamp(1, INPUT);
        let resized_h = ((height as f32 * scale).round() as usize).clamp(1, INPUT);
        let pad_x = (INPUT - resized_w) / 2;
        let pad_y = (INPUT - resized_h) / 2;
        let mut data = vec![0.0_f32; 3 * INPUT * INPUT];
        for y in 0..resized_h {
            let source_y = ((y as f32 + 0.5) * height as f32 / resized_h as f32 - 0.5)
                .clamp(0.0, (height - 1) as f32);
            let y0 = source_y.floor() as usize;
            let y1 = (y0 + 1).min(height - 1);
            let fy = source_y - y0 as f32;
            for x in 0..resized_w {
                let source_x = ((x as f32 + 0.5) * width as f32 / resized_w as f32 - 0.5)
                    .clamp(0.0, (width - 1) as f32);
                let x0 = source_x.floor() as usize;
                let x1 = (x0 + 1).min(width - 1);
                let fx = source_x - x0 as f32;
                let dst = (y + pad_y) * INPUT + x + pad_x;
                for (channel, bgra_index) in [0, 1, 2].into_iter().enumerate() {
                    let p00 = bgra[y0 * stride + x0 * 4 + bgra_index] as f32;
                    let p01 = bgra[y0 * stride + x1 * 4 + bgra_index] as f32;
                    let p10 = bgra[y1 * stride + x0 * 4 + bgra_index] as f32;
                    let p11 = bgra[y1 * stride + x1 * 4 + bgra_index] as f32;
                    let top = p00 + (p01 - p00) * fx;
                    let bottom = p10 + (p11 - p10) * fx;
                    data[channel * INPUT * INPUT + dst] = top + (bottom - top) * fy;
                }
            }
        }
        Array4::from_shape_vec((1, 3, INPUT, INPUT), data).ok()
    }

    fn lcg_bytes(len: usize, seed: u32) -> Vec<u8> {
        let mut state = seed;
        (0..len)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (state >> 24) as u8
            })
            .collect()
    }

    fn assert_matches_reference(bgra: &[u8], width: usize, height: usize, stride: usize) {
        let actual = preprocess_bgra(bgra, width, height, stride).unwrap();
        let expected = preprocess_reference(bgra, width, height, stride).unwrap();
        assert_eq!(actual.shape(), expected.shape());
        assert_eq!(actual.as_slice().unwrap(), expected.as_slice().unwrap());
    }

    #[test]
    fn preprocess_matches_reference_for_640x480_with_row_padding() {
        let bgra = lcg_bytes(2576 * 480, 1);
        assert_matches_reference(&bgra, 640, 480, 2576);
    }

    #[test]
    fn preprocess_matches_reference_for_1280x720_downscale() {
        let bgra = lcg_bytes(5120 * 720, 2);
        assert_matches_reference(&bgra, 1280, 720, 5120);
    }

    #[test]
    fn preprocess_matches_reference_for_2x1_frame() {
        let bgra = lcg_bytes(8, 3);
        assert_matches_reference(&bgra, 2, 1, 8);
    }

    #[test]
    fn preprocess_matches_reference_for_320x240_upscale() {
        let bgra = lcg_bytes(1280 * 240, 4);
        assert_matches_reference(&bgra, 320, 240, 1280);
    }

    #[test]
    fn preprocess_640x480_keeps_letterbox_padding_and_maps_first_pixel() {
        let bgra = lcg_bytes(2576 * 480, 1);
        let tensor = preprocess_bgra(&bgra, 640, 480, 2576).unwrap();
        for channel in 0..3 {
            for row in (0..80).chain(560..640) {
                for column in 0..640 {
                    assert_eq!(tensor[[0, channel, row, column]], 0.0);
                }
            }
        }
        assert_eq!(tensor[[0, 0, 80, 0]], bgra[0] as f32);
    }

    #[test]
    fn output_names_are_distinct_and_match_model_layout() {
        let mut names: Vec<&str> = CLS_OUTPUTS
            .iter()
            .chain(OBJ_OUTPUTS.iter())
            .chain(BBOX_OUTPUTS.iter())
            .copied()
            .collect();
        assert_eq!(CLS_OUTPUTS, ["cls_8", "cls_16", "cls_32"]);
        assert_eq!(OBJ_OUTPUTS, ["obj_8", "obj_16", "obj_32"]);
        assert_eq!(BBOX_OUTPUTS, ["bbox_8", "bbox_16", "bbox_32"]);
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), 9);
    }

    #[test]
    fn sha256_matches_known_vector() {
        assert_eq!(
            sha256(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}
