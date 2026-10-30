//! Benchmark helpers for the occlusion fixture set. Compiled only with the `bench` feature.

/// Longest accepted fixture image side, in pixels.
const MAX_LONGEST_SIDE: u32 = 640;

/// Licenses accepted for fixture images.
const ALLOWED_LICENSES: [&str; 2] = ["CC0-1.0", "PDM-1.0"];

/// Occlusion categories accepted in the manifest.
const ALLOWED_CATEGORIES: [&str; 9] = [
    "none",
    "mask",
    "hand_mouth",
    "hand_eyes",
    "glasses",
    "tilt",
    "no_face_hand",
    "no_face_mask",
    "no_face_object",
];

/// Occlusion levels accepted in the manifest.
const ALLOWED_OCCLUSIONS: [&str; 3] = ["none", "partial", "heavy"];

/// One fixture image entry from the benchmark manifest.
#[derive(Debug, Clone)]
pub struct ManifestImage {
    /// File name of the image in the fixture directory.
    pub file: String,
    /// Occlusion category, one of `ALLOWED_CATEGORIES`.
    pub category: String,
    /// Whether the image contains a face.
    pub has_face: bool,
    /// Occlusion level, one of `ALLOWED_OCCLUSIONS`.
    pub occlusion: String,
    /// Source page the image was taken from.
    pub source_url: String,
    /// License identifier, one of `ALLOWED_LICENSES`.
    pub license: String,
    /// Author credit; may be empty.
    pub author: String,
    /// Whether the subject's rights were checked (required for face images).
    pub subject_rights_checked: bool,
    /// Free-form notes; may be empty.
    pub notes: String,
}

/// Parses the fixture manifest and validates every `[[image]]` entry.
///
/// An empty or missing `image` array yields an empty list. Errors name the file.
pub fn parse_manifest(text: &str) -> Result<Vec<ManifestImage>, String> {
    let manifest: toml::Table = text
        .parse()
        .map_err(|err| format!("invalid manifest TOML: {err}"))?;
    let Some(entries) = manifest.get("image") else {
        return Ok(Vec::new());
    };
    let entries = entries
        .as_array()
        .ok_or_else(|| "`image` must be an array of tables".to_owned())?;
    entries
        .iter()
        .enumerate()
        .map(|(index, entry)| parse_entry(index, entry))
        .collect()
}

/// Parses one `[[image]]` entry, labelling any error with its file name.
fn parse_entry(index: usize, entry: &toml::Value) -> Result<ManifestImage, String> {
    let table = entry
        .as_table()
        .ok_or_else(|| format!("image entry {}: expected a table", index + 1))?;
    let label = table
        .get("file")
        .and_then(toml::Value::as_str)
        .unwrap_or("<unnamed>");
    build_entry(table).map_err(|err| format!("{label} (image entry {}): {err}", index + 1))
}

/// Builds and validates a manifest entry from its TOML table.
fn build_entry(table: &toml::Table) -> Result<ManifestImage, String> {
    let image = ManifestImage {
        file: required_str(table, "file")?,
        category: required_str(table, "category")?,
        has_face: required_bool(table, "has_face")?,
        occlusion: required_str(table, "occlusion")?,
        source_url: required_str(table, "source_url")?,
        license: required_str(table, "license")?,
        author: optional_str(table, "author")?,
        subject_rights_checked: required_bool(table, "subject_rights_checked")?,
        notes: optional_str(table, "notes")?,
    };
    if image.source_url.is_empty() {
        return Err("`source_url` must not be empty".to_owned());
    }
    ensure_allowed("license", &image.license, &ALLOWED_LICENSES)?;
    ensure_allowed("category", &image.category, &ALLOWED_CATEGORIES)?;
    ensure_allowed("occlusion", &image.occlusion, &ALLOWED_OCCLUSIONS)?;
    if image.has_face && !image.subject_rights_checked {
        return Err("face images need `subject_rights_checked = true`".to_owned());
    }
    Ok(image)
}

/// Reads a required string key.
fn required_str(table: &toml::Table, key: &str) -> Result<String, String> {
    table
        .get(key)
        .and_then(toml::Value::as_str)
        .map(str::to_owned)
        .ok_or_else(|| format!("missing or non-string `{key}`"))
}

/// Reads an optional string key; a missing key is treated as empty.
fn optional_str(table: &toml::Table, key: &str) -> Result<String, String> {
    match table.get(key) {
        None => Ok(String::new()),
        Some(value) => value
            .as_str()
            .map(str::to_owned)
            .ok_or_else(|| format!("`{key}` must be a string")),
    }
}

/// Reads a required boolean key.
fn required_bool(table: &toml::Table, key: &str) -> Result<bool, String> {
    table
        .get(key)
        .and_then(toml::Value::as_bool)
        .ok_or_else(|| format!("missing or non-boolean `{key}`"))
}

/// Checks that `value` is one of `allowed`.
fn ensure_allowed(key: &str, value: &str, allowed: &[&str]) -> Result<(), String> {
    if allowed.contains(&value) {
        Ok(())
    } else {
        Err(format!(
            "`{key}` `{value}` is not one of: {}",
            allowed.join(", ")
        ))
    }
}

/// Checks that the longest image side is at most 640 pixels.
pub fn validate_image_size(width: u32, height: u32) -> Result<(), String> {
    let longest = width.max(height);
    if longest > MAX_LONGEST_SIDE {
        return Err(format!(
            "longest side {longest} px exceeds {MAX_LONGEST_SIDE} px"
        ));
    }
    Ok(())
}

/// Returns the 95% Wilson score interval for `successes` out of `total`.
///
/// Returns `(0.0, 1.0)` when `total` is zero.
pub fn wilson_interval(successes: u32, total: u32) -> (f64, f64) {
    if total == 0 {
        return (0.0, 1.0);
    }
    const Z: f64 = 1.96;
    let n = f64::from(total);
    let p = f64::from(successes) / n;
    let z2 = Z * Z;
    let denom = 1.0 + z2 / n;
    let centre = (p + z2 / (2.0 * n)) / denom;
    let half = Z * (p * (1.0 - p) / n + z2 / (4.0 * n * n)).sqrt() / denom;
    ((centre - half).max(0.0), (centre + half).min(1.0))
}

/// Returns the candidate thresholds: 0.30 to 0.90 in steps of 0.05.
pub fn threshold_grid() -> Vec<f32> {
    (0..13).map(|i| 0.30 + 0.05 * i as f32).collect()
}

/// One image's detector result for the benchmark.
#[derive(Debug, Clone, Copy)]
pub struct Score {
    /// Whether the image contains a face.
    pub has_face: bool,
    /// Whether the face is occluded; only meaningful when `has_face` is true.
    pub occluded: bool,
    /// Highest face detection score in the image.
    pub max_score: f32,
}

/// Recall and false-positive counts at one threshold.
#[derive(Debug, Clone, Copy)]
pub struct Metrics {
    /// Fraction of occluded faces detected.
    pub occluded_recall: f64,
    /// Fraction of unoccluded faces detected.
    pub unoccluded_recall: f64,
    /// Number of face-free images with a detection at or above the threshold.
    pub face_free_false_positives: u32,
}

/// Returns `(successes, total)` for occluded faces, unoccluded faces and face-free images.
///
/// For face-free images the pair counts false positives and the total face-free images.
pub fn recall_summary(scores: &[Score], threshold: f32) -> ((u32, u32), (u32, u32), (u32, u32)) {
    let mut occluded: (u32, u32) = (0, 0);
    let mut unoccluded: (u32, u32) = (0, 0);
    let mut face_free: (u32, u32) = (0, 0);
    for score in scores {
        let hit = u32::from(score.max_score >= threshold);
        let bucket = match (score.has_face, score.occluded) {
            (true, true) => &mut occluded,
            (true, false) => &mut unoccluded,
            (false, _) => &mut face_free,
        };
        bucket.0 += hit;
        bucket.1 += 1;
    }
    (occluded, unoccluded, face_free)
}

/// Returns the metrics for `scores` at `threshold`; empty denominators give 0.0.
pub fn metrics_at(scores: &[Score], threshold: f32) -> Metrics {
    let (occluded, unoccluded, face_free) = recall_summary(scores, threshold);
    Metrics {
        occluded_recall: ratio(occluded),
        unoccluded_recall: ratio(unoccluded),
        face_free_false_positives: face_free.0,
    }
}

/// Returns `successes / total` as a fraction, or 0.0 when `total` is zero.
fn ratio((successes, total): (u32, u32)) -> f64 {
    if total == 0 {
        0.0
    } else {
        f64::from(successes) / f64::from(total)
    }
}

/// Picks the operating threshold: the highest clean threshold with the best occluded recall,
/// plus one grid step, capped at 0.90. Returns `None` when every threshold has false positives.
pub fn operating_threshold(scores: &[Score]) -> Option<f32> {
    const EPSILON: f64 = 1e-9;
    // Thresholds without face-free false positives, in ascending order.
    let clean: Vec<(f32, f64)> = threshold_grid()
        .into_iter()
        .filter_map(|threshold| {
            let metrics = metrics_at(scores, threshold);
            let clean = metrics.face_free_false_positives == 0;
            clean.then_some((threshold, metrics.occluded_recall))
        })
        .collect();
    let best = clean.iter().map(|&(_, recall)| recall).reduce(f64::max)?;
    // The grid is ascending, so the last match is the highest threshold.
    let (chosen, _) = clean
        .iter()
        .filter(|&&(_, recall)| recall + EPSILON >= best)
        .copied()
        .next_back()?;
    Some((chosen + 0.05).min(0.90))
}

/// Outcome of each acceptance rule for a candidate against the baseline.
#[derive(Debug, Clone, Copy)]
pub struct RuleReport {
    /// Occluded recall rises by at least 0.10.
    pub rule1_occluded_gain: bool,
    /// Unoccluded recall does not drop.
    pub rule2_no_regression: bool,
    /// Face-free false positives do not increase.
    pub rule3_no_more_false_positives: bool,
    /// Latency and memory stay within budget.
    pub rule4_budget: bool,
}

/// Evaluates the four acceptance rules for `candidate` against `baseline`.
pub fn evaluate_rules(
    baseline: &Metrics,
    candidate: &Metrics,
    baseline_p50_ms: f64,
    candidate_p50_ms: f64,
    memory_delta_mb: f64,
) -> RuleReport {
    const EPSILON: f64 = 1e-9;
    let occluded_gain = candidate.occluded_recall + EPSILON >= baseline.occluded_recall + 0.10;
    let no_regression = candidate.unoccluded_recall >= baseline.unoccluded_recall;
    let no_new_fp = candidate.face_free_false_positives <= baseline.face_free_false_positives;
    let budget_ok = candidate_p50_ms <= 3.0 * baseline_p50_ms
        && candidate_p50_ms <= 60.0
        && memory_delta_mb <= 10.0;
    RuleReport {
        rule1_occluded_gain: occluded_gain,
        rule2_no_regression: no_regression,
        rule3_no_more_false_positives: no_new_fp,
        rule4_budget: budget_ok,
    }
}

/// Opens a detector without the pinned-digest check. For benchmarking candidate models only.
///
/// The spec uses a low confidence floor so the max score of faces below the production threshold
/// is still reported. The paths must be absolute and point to existing files. Runs on CPU, device 0.
pub fn open_unpinned(
    runtime_abs_path: &str,
    model_abs_path: &str,
    family: &str,
) -> Result<*mut super::TvInferenceHandle, String> {
    use super::{ModelFamily, ModelSpec, ProviderKind, INPUT, NMS_IOU_THRESHOLD};
    use std::path::Path;

    let (family_kind, mean_bgr, nms_iou) = match family {
        "yunet" => (ModelFamily::YuNet, [0.0; 3], NMS_IOU_THRESHOLD),
        "retinaface" => (ModelFamily::RetinaFace, [104.0, 117.0, 123.0], 0.4),
        other => {
            return Err(format!(
                "unsupported family `{other}`; expected `yunet` or `retinaface`"
            ));
        }
    };
    let runtime = Path::new(runtime_abs_path);
    let model = Path::new(model_abs_path);
    if !runtime.is_absolute() || !model.is_absolute() {
        return Err("runtime and model paths must be absolute".to_owned());
    }
    if !runtime.is_file() {
        return Err(format!("runtime not found: {runtime_abs_path}"));
    }
    if !model.is_file() {
        return Err(format!("model not found: {model_abs_path}"));
    }
    let spec: &'static ModelSpec = Box::leak(Box::new(ModelSpec {
        family: family_kind,
        input: INPUT,
        mean_bgr,
        confidence: 0.05,
        nms_iou,
    }));
    super::create_with_spec(runtime, model, ProviderKind::Cpu, 0, spec)
        .map_err(|code| format!("session creation failed with status {code}"))
}

/// Runs the detector on a top-down BGRA frame of `width` x `height` (stride `width * 4`).
///
/// Returns the highest face score and whether the frame was too dark to run. A dark frame gives
/// `(0.0, true)`. Any other negative or unavailable status is an error.
///
/// # Safety
/// `handle` must be a live handle from `open_unpinned`, and it must not be used concurrently or
/// destroyed during the call.
pub unsafe fn detect_max_score(
    handle: *mut super::TvInferenceHandle,
    bgra: &[u8],
    width: u32,
    height: u32,
) -> Result<(f32, bool), String> {
    use super::{TvInferenceResult, ABSENT, INSUFFICIENT_LIGHT, PRESENT};

    let stride = width as usize * 4;
    let mut result = TvInferenceResult::default();
    // SAFETY: the caller guarantees `handle` is live. `bgra` addresses `bgra.len()` readable bytes,
    // and `result` is a writable out value.
    let status = unsafe {
        super::tv_inference_detect_bgra(
            handle,
            bgra.as_ptr(),
            bgra.len(),
            width,
            height,
            stride,
            &mut result,
        )
    };
    match status {
        ABSENT | PRESENT => Ok((result.confidence, false)),
        INSUFFICIENT_LIGHT => Ok((0.0, true)),
        _ => Err(format!("detection failed with status {status}")),
    }
}

/// Destroys a handle returned by `open_unpinned`.
///
/// # Safety
/// `handle` must be null or a live handle from `open_unpinned`, and no other call may be using it.
pub unsafe fn destroy(handle: *mut super::TvInferenceHandle) {
    // SAFETY: the caller guarantees `handle` is null or live and not used by other calls.
    unsafe { super::tv_inference_destroy(handle) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Returns true when `a` and `b` differ by at most `tol`.
    fn approx(a: f64, b: f64, tol: f64) -> bool {
        (a - b).abs() <= tol
    }

    /// A single well-formed manifest entry; tests override one field at a time.
    const VALID_ENTRY: &str = r#"[[image]]
file = "a.jpg"
category = "mask"
has_face = true
occlusion = "partial"
source_url = "https://commons.wikimedia.org/wiki/File:A.jpg"
license = "CC0-1.0"
author = "x"
subject_rights_checked = true
notes = ""
"#;

    /// Returns `VALID_ENTRY` with every line starting with `key` replaced by `line`.
    fn entry_with(key: &str, line: &str) -> String {
        let mut text = String::new();
        for existing in VALID_ENTRY.lines() {
            if existing.starts_with(key) {
                text.push_str(line);
            } else {
                text.push_str(existing);
            }
            text.push('\n');
        }
        text
    }

    #[test]
    fn parses_valid_manifest_entry() {
        let images = parse_manifest(VALID_ENTRY).expect("valid entry should parse");
        assert_eq!(images.len(), 1);
        let image = &images[0];
        assert_eq!(image.file, "a.jpg");
        assert_eq!(image.category, "mask");
        assert!(image.has_face);
        assert_eq!(image.occlusion, "partial");
        assert_eq!(
            image.source_url,
            "https://commons.wikimedia.org/wiki/File:A.jpg"
        );
        assert_eq!(image.license, "CC0-1.0");
        assert_eq!(image.author, "x");
        assert!(image.subject_rights_checked);
        assert_eq!(image.notes, "");
    }

    #[test]
    fn rejects_empty_source_url() {
        let text = entry_with("source_url", "source_url = \"\"");
        assert!(parse_manifest(&text).is_err());
    }

    #[test]
    fn rejects_license_outside_the_allowed_list() {
        let text = entry_with("license", "license = \"CC-BY-4.0\"");
        assert!(parse_manifest(&text).is_err());
    }

    #[test]
    fn accepts_pdm_license() {
        let text = entry_with("license", "license = \"PDM-1.0\"");
        assert!(parse_manifest(&text).is_ok());
    }

    #[test]
    fn rejects_face_image_without_subject_rights_check() {
        let text = entry_with("subject_rights_checked", "subject_rights_checked = false");
        assert!(parse_manifest(&text).is_err());
    }

    #[test]
    fn rejects_unknown_category() {
        let text = entry_with("category", "category = \"banana\"");
        assert!(parse_manifest(&text).is_err());
    }

    #[test]
    fn accepts_manifest_without_images() {
        let images = parse_manifest("").expect("empty manifest should parse");
        assert!(images.is_empty());
    }

    #[test]
    fn accepts_longest_side_up_to_640() {
        assert!(validate_image_size(640, 480).is_ok());
        assert!(validate_image_size(480, 640).is_ok());
    }

    #[test]
    fn rejects_longest_side_above_640() {
        assert!(validate_image_size(641, 480).is_err());
    }

    #[test]
    fn wilson_interval_for_half_successes() {
        let (low, high) = wilson_interval(5, 10);
        assert!(approx(low, 0.2366, 1e-3), "low was {low}");
        assert!(approx(high, 0.7634, 1e-3), "high was {high}");
    }

    #[test]
    fn wilson_interval_for_all_successes() {
        let (low, high) = wilson_interval(10, 10);
        assert!(approx(low, 0.7225, 1e-3), "low was {low}");
        assert!(approx(high, 1.0, 1e-3), "high was {high}");
    }

    #[test]
    fn wilson_interval_without_trials_is_full_range() {
        assert_eq!(wilson_interval(0, 0), (0.0, 1.0));
    }

    #[test]
    fn threshold_grid_runs_from_0_30_to_0_90() {
        let grid = threshold_grid();
        assert_eq!(grid.len(), 13);
        assert!(approx(f64::from(grid[0]), 0.30, 1e-4));
        assert!(approx(f64::from(grid[12]), 0.90, 1e-4));
    }

    /// Builds a score record for the metric tests.
    fn score(has_face: bool, occluded: bool, max_score: f32) -> Score {
        Score {
            has_face,
            occluded,
            max_score,
        }
    }

    /// Fixture: four occluded faces, one clear face, three face-free images.
    fn fixture() -> Vec<Score> {
        vec![
            score(true, true, 0.95),
            score(true, true, 0.62),
            score(true, true, 0.45),
            score(true, true, 0.35),
            score(true, false, 0.99),
            score(false, false, 0.52),
            score(false, false, 0.20),
            score(false, false, 0.10),
        ]
    }

    #[test]
    fn metrics_at_half_threshold_counts_recall_and_false_positives() {
        let scores = fixture();
        let metrics = metrics_at(&scores, 0.50);
        assert!(approx(metrics.occluded_recall, 0.5, 1e-9));
        assert!(approx(metrics.unoccluded_recall, 1.0, 1e-9));
        assert_eq!(metrics.face_free_false_positives, 1);
    }

    #[test]
    fn metrics_at_higher_threshold_clears_false_positives() {
        let scores = fixture();
        let metrics = metrics_at(&scores, 0.55);
        assert_eq!(metrics.face_free_false_positives, 0);
    }

    #[test]
    fn metrics_at_empty_scores_are_zero() {
        let metrics = metrics_at(&[], 0.5);
        assert!(approx(metrics.occluded_recall, 0.0, 1e-9));
        assert!(approx(metrics.unoccluded_recall, 0.0, 1e-9));
        assert_eq!(metrics.face_free_false_positives, 0);
    }

    #[test]
    fn operating_threshold_is_one_step_above_best_clean_threshold() {
        let scores = fixture();
        let threshold = operating_threshold(&scores).expect("fixture has a clean threshold");
        assert!(approx(f64::from(threshold), 0.65, 1e-4));
    }

    #[test]
    fn operating_threshold_is_none_when_every_threshold_has_false_positives() {
        let mut scores = fixture();
        scores.push(score(false, false, 0.95));
        assert!(operating_threshold(&scores).is_none());
    }

    /// Builds a `Metrics` value for the rule tests.
    fn metrics(occluded_recall: f64, unoccluded_recall: f64, fp: u32) -> Metrics {
        Metrics {
            occluded_recall,
            unoccluded_recall,
            face_free_false_positives: fp,
        }
    }

    #[test]
    fn evaluate_rules_passes_all_four_rules_for_a_clear_gain() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.50, 1.0, 0);
        let report = evaluate_rules(&baseline, &candidate, 10.0, 25.0, 5.0);
        assert!(report.rule1_occluded_gain);
        assert!(report.rule2_no_regression);
        assert!(report.rule3_no_more_false_positives);
        assert!(report.rule4_budget);
    }

    #[test]
    fn evaluate_rules_requires_ten_point_occluded_gain() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.35, 1.0, 0);
        let report = evaluate_rules(&baseline, &candidate, 10.0, 25.0, 5.0);
        assert!(!report.rule1_occluded_gain);
    }

    #[test]
    fn evaluate_rules_rejects_unoccluded_regression() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.50, 0.9, 0);
        let report = evaluate_rules(&baseline, &candidate, 10.0, 25.0, 5.0);
        assert!(!report.rule2_no_regression);
    }

    #[test]
    fn evaluate_rules_rejects_additional_false_positives() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.50, 1.0, 1);
        let report = evaluate_rules(&baseline, &candidate, 10.0, 25.0, 5.0);
        assert!(!report.rule3_no_more_false_positives);
    }

    #[test]
    fn evaluate_rules_rejects_p50_above_three_times_baseline() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.50, 1.0, 0);
        let report = evaluate_rules(&baseline, &candidate, 10.0, 31.0, 5.0);
        assert!(!report.rule4_budget);
    }

    #[test]
    fn evaluate_rules_rejects_p50_above_60_ms() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.50, 1.0, 0);
        let report = evaluate_rules(&baseline, &candidate, 30.0, 70.0, 5.0);
        assert!(!report.rule4_budget);
    }

    #[test]
    fn evaluate_rules_rejects_memory_above_10_mb() {
        let baseline = metrics(0.30, 1.0, 0);
        let candidate = metrics(0.50, 1.0, 0);
        let report = evaluate_rules(&baseline, &candidate, 10.0, 25.0, 11.0);
        assert!(!report.rule4_budget);
    }
}
