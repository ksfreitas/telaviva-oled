//! Occlusion benchmark for a YuNet detector on the fixture set in `tests/fixtures/occlusion`.
//! Compiled only with the `bench` feature. Prints a markdown report to stdout.
//!
//! ```text
//! cargo run -p telaviva-inference --features bench --example occlusion_bench -- --model <path>
//! cargo run -p telaviva-inference --features bench --example occlusion_bench -- --inspect <model.onnx>
//! ```
//!
//! `--inspect` prints the file size and SHA-256, the ONNX header and the inputs and outputs that
//! ONNX Runtime reports. It reads no manifest and runs no detection.

use ort::{execution_providers::CPUExecutionProvider, session::Session, value::ValueType};
use sha2::{Digest, Sha256};
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, ExitCode},
    time::Instant,
};

use telaviva_inference::{
    bench::{self, ManifestImage, Score},
    TvInferenceHandle,
};

/// Production detection threshold; the baseline model is judged at this threshold.
const PRODUCTION_THRESHOLD: f32 = 0.80;
/// Untimed detect calls before the latency measurement.
const WARM_UP_CALLS: usize = 5;
/// Timed detect calls per model for the latency percentiles.
const LATENCY_CALLS: usize = 100;

const USAGE: &str = "usage: occlusion_bench (--model <path> | --inspect <model.onnx>) \
[--family yunet] [--runtime <path>] [--manifest <path>] [--baseline-model <path>] \
[--baseline-family yunet] [--threshold <f32>] [--per-image]";
/// Wire types of the protobuf encoding that ONNX uses.
const WIRE_VARINT: u8 = 0;
const WIRE_FIXED64: u8 = 1;
const WIRE_LEN: u8 = 2;
const WIRE_FIXED32: u8 = 5;
/// Runtime path relative to the repository root.
const DEFAULT_RUNTIME: &str = "tools/inference/runtime/macos-arm64/libonnxruntime.dylib";
/// Manifest path relative to the repository root.
const DEFAULT_MANIFEST: &str = "crates/telaviva-inference/tests/fixtures/occlusion/manifest.toml";

struct Args {
    /// Candidate model to benchmark; required unless `--inspect` is given.
    model: Option<PathBuf>,
    /// Model to inspect instead of benchmarking; when set, nothing else runs.
    inspect: Option<PathBuf>,
    family: String,
    runtime: PathBuf,
    manifest: PathBuf,
    baseline_model: Option<PathBuf>,
    baseline_family: String,
    /// Threshold given with `--threshold`; replaces the operating threshold when set.
    threshold: Option<f32>,
    /// Whether to print the per-image table after the category section.
    per_image: bool,
}

/// One decoded fixture image, ready for the detector.
struct Fixture {
    image: ManifestImage,
    width: u32,
    height: u32,
    /// Top-down BGRA pixels with stride `width * 4`.
    bgra: Vec<u8>,
}

/// One fixture's category and detector result.
struct Row {
    file: String,
    category: String,
    occlusion: String,
    /// True when the frame was too dark to run (scored 0).
    dark: bool,
    score: Score,
}

/// Measurements for one model over the fixture set.
struct ModelRun {
    rows: Vec<Row>,
    /// File names of fixtures that were too dark to run (scored 0).
    insufficient_light: Vec<String>,
    p50_ms: f64,
    p95_ms: f64,
    /// File name of the image the latency was measured on.
    latency_image: String,
    /// RSS increase while loading the model, in MB.
    memory_mb: f64,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("error: {message}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let args = parse_args()?;
    if let Some(model) = &args.inspect {
        return inspect_model(model, &args.runtime);
    }
    let model = args
        .model
        .ok_or_else(|| format!("--model is required\n{USAGE}"))?;
    let text = fs::read_to_string(&args.manifest)
        .map_err(|err| format!("{}: {err}", args.manifest.display()))?;
    let images = bench::parse_manifest(&text)?;
    if images.is_empty() {
        println!("No fixture images yet.");
        return Ok(());
    }
    let dir = args.manifest.parent().unwrap_or(Path::new("."));
    let fixtures = load_fixtures(dir, images)?;

    let runtime = absolute(&args.runtime)?;
    let candidate = run_model(&absolute(&model)?, &args.family, &runtime, &fixtures)?;
    let baseline = match &args.baseline_model {
        Some(path) => Some(run_model(
            &absolute(path)?,
            &args.baseline_family,
            &runtime,
            &fixtures,
        )?),
        None => None,
    };

    let candidate_scores = scores_of(&candidate.rows);
    let operating = bench::operating_threshold(&candidate_scores);
    // A threshold given on the command line replaces the operating threshold below.
    let threshold = args.threshold.or(operating);
    let given = args.threshold.is_some();
    print_summary(&model, &args.family, fixtures.len(), &candidate);
    print_sweep(&candidate_scores);
    print_categories(&candidate.rows, threshold, given);
    if args.per_image {
        print_per_image(&candidate.rows);
    }
    print_cost(&candidate);
    if let (Some(path), Some(base)) = (&args.baseline_model, &baseline) {
        print_rules(
            path,
            &args.baseline_family,
            &candidate,
            base,
            threshold,
            given,
        );
    }
    print_coverage_gaps(dir)
}

fn parse_args() -> Result<Args, String> {
    let root = repo_root();
    let mut model = None;
    let mut inspect = None;
    let mut family = "yunet".to_owned();
    let mut runtime = root.join(DEFAULT_RUNTIME);
    let mut manifest = root.join(DEFAULT_MANIFEST);
    let mut baseline_model = None;
    let mut baseline_family = "yunet".to_owned();
    let mut threshold = None;
    let mut per_image = false;
    let mut args = std::env::args().skip(1);
    while let Some(flag) = args.next() {
        match flag.as_str() {
            "--model" => model = Some(PathBuf::from(value(&mut args, &flag)?)),
            "--inspect" => inspect = Some(PathBuf::from(value(&mut args, &flag)?)),
            "--family" => family = value(&mut args, &flag)?,
            "--runtime" => runtime = PathBuf::from(value(&mut args, &flag)?),
            "--manifest" => manifest = PathBuf::from(value(&mut args, &flag)?),
            "--baseline-model" => baseline_model = Some(PathBuf::from(value(&mut args, &flag)?)),
            "--baseline-family" => baseline_family = value(&mut args, &flag)?,
            "--threshold" => threshold = Some(parse_threshold(&value(&mut args, &flag)?)?),
            "--per-image" => per_image = true,
            other => return Err(format!("unknown argument `{other}`\n{USAGE}")),
        }
    }
    if inspect.is_some() && model.is_some() {
        return Err(format!(
            "--inspect cannot be combined with --model\n{USAGE}"
        ));
    }
    Ok(Args {
        model,
        inspect,
        family,
        runtime,
        manifest,
        baseline_model,
        baseline_family,
        threshold,
        per_image,
    })
}

/// Returns the value that follows `flag`.
fn value(args: &mut impl Iterator<Item = String>, flag: &str) -> Result<String, String> {
    args.next()
        .ok_or_else(|| format!("{flag} needs a value\n{USAGE}"))
}

/// Parses the `--threshold` value, which must be a finite number.
fn parse_threshold(text: &str) -> Result<f32, String> {
    text.parse::<f32>()
        .ok()
        .filter(|threshold| threshold.is_finite())
        .ok_or_else(|| format!("--threshold needs a finite number, got `{text}`\n{USAGE}"))
}

/// The repository root: CARGO_MANIFEST_DIR is crates/telaviva-inference, two levels below it.
fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(2)
        .map_or_else(|| PathBuf::from("."), Path::to_path_buf)
}

/// Resolves `path` to an absolute, existing path as text, as the detector loader requires.
fn absolute(path: &Path) -> Result<String, String> {
    let canonical = fs::canonicalize(path).map_err(|err| format!("{}: {err}", path.display()))?;
    canonical
        .into_os_string()
        .into_string()
        .map_err(|_| format!("path is not valid UTF-8: {}", path.display()))
}

/// Decodes every manifest image in `dir`. Any image above 640 px aborts the run.
fn load_fixtures(dir: &Path, images: Vec<ManifestImage>) -> Result<Vec<Fixture>, String> {
    images
        .into_iter()
        .map(|image| load_fixture(dir, image))
        .collect()
}

fn load_fixture(dir: &Path, image: ManifestImage) -> Result<Fixture, String> {
    let path = dir.join(&image.file);
    let decoded = image::open(&path)
        .map_err(|err| format!("{}: {err}", path.display()))?
        .to_rgba8();
    let (width, height) = decoded.dimensions();
    bench::validate_image_size(width, height).map_err(|err| format!("{}: {err}", image.file))?;
    // RGBA to BGRA; the decoded buffer is already top-down.
    let bgra: Vec<u8> = decoded
        .into_raw()
        .chunks_exact(4)
        .flat_map(|px| [px[2], px[1], px[0], px[3]])
        .collect();
    Ok(Fixture {
        image,
        width,
        height,
        bgra,
    })
}

/// Loads one model, scores every fixture and measures its latency and memory.
fn run_model(
    model: &str,
    family: &str,
    runtime: &str,
    fixtures: &[Fixture],
) -> Result<ModelRun, String> {
    let before = rss_mb()?;
    let handle = bench::open_unpinned(runtime, model, family)?;
    let after = rss_mb();
    let result = after.and_then(|after| measure(handle, fixtures, after - before));
    // SAFETY: `handle` came from `open_unpinned`, is not used after this call, and was never shared.
    unsafe { bench::destroy(handle) };
    result
}

/// Scores each fixture once, then times detect calls on the first fixture.
fn measure(
    handle: *mut TvInferenceHandle,
    fixtures: &[Fixture],
    memory_mb: f64,
) -> Result<ModelRun, String> {
    let Some(first) = fixtures.first() else {
        return Err("no fixtures to measure".to_owned());
    };
    let mut rows = Vec::with_capacity(fixtures.len());
    let mut insufficient_light = Vec::new();
    for fixture in fixtures {
        let (max_score, dark) = detect(handle, fixture)?;
        if dark {
            insufficient_light.push(fixture.image.file.clone());
        }
        rows.push(Row {
            file: fixture.image.file.clone(),
            category: fixture.image.category.clone(),
            occlusion: fixture.image.occlusion.clone(),
            dark,
            score: Score {
                has_face: fixture.image.has_face,
                occluded: fixture.image.has_face && fixture.image.occlusion != "none",
                max_score,
            },
        });
    }
    for _ in 0..WARM_UP_CALLS {
        detect(handle, first)?;
    }
    let mut timings = Vec::with_capacity(LATENCY_CALLS);
    for _ in 0..LATENCY_CALLS {
        let start = Instant::now();
        detect(handle, first)?;
        timings.push(start.elapsed().as_secs_f64() * 1000.0);
    }
    timings.sort_by(f64::total_cmp);
    Ok(ModelRun {
        rows,
        insufficient_light,
        p50_ms: percentile(&timings, 0.50),
        p95_ms: percentile(&timings, 0.95),
        latency_image: first.image.file.clone(),
        memory_mb,
    })
}

/// Runs the detector on one fixture. Returns the max score and whether the frame was too dark.
fn detect(handle: *mut TvInferenceHandle, fixture: &Fixture) -> Result<(f32, bool), String> {
    // SAFETY: `handle` is live from `open_unpinned` and is not destroyed until `run_model` returns.
    // `bgra` holds width * height * 4 bytes, the frame size the detector expects.
    unsafe { bench::detect_max_score(handle, &fixture.bgra, fixture.width, fixture.height) }
}

/// Nearest-rank percentile of ascending `sorted` values; `p` is in 0..=1.
fn percentile(sorted: &[f64], p: f64) -> f64 {
    let rank = (p * sorted.len() as f64).ceil() as usize;
    sorted[rank.clamp(1, sorted.len()) - 1]
}

/// Resident set size of this process in MB, read from `ps` (which reports KB).
#[cfg(not(windows))]
fn rss_mb() -> Result<f64, String> {
    let pid = std::process::id().to_string();
    let output = Command::new("ps")
        .args(["-o", "rss=", "-p", pid.as_str()])
        .output()
        .map_err(|err| format!("ps failed: {err}"))?;
    let text = String::from_utf8_lossy(&output.stdout);
    let kb: f64 = text
        .trim()
        .parse()
        .map_err(|_| format!("could not read RSS from ps output `{}`", text.trim()))?;
    Ok(kb / 1024.0)
}

/// Working set of this process in MB. On Windows the working set is read through PowerShell,
/// which reports bytes.
#[cfg(windows)]
fn rss_mb() -> Result<f64, String> {
    let pid = std::process::id().to_string();
    let command = format!("(Get-Process -Id {pid}).WorkingSet64");
    let output = Command::new("powershell.exe")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            command.as_str(),
        ])
        .output()
        .map_err(|err| format!("powershell failed: {err}"))?;
    let text = String::from_utf8_lossy(&output.stdout);
    let bytes: u64 = text.trim().parse().map_err(|_| {
        format!(
            "could not read working set from PowerShell output `{}`",
            text.trim()
        )
    })?;
    Ok(bytes as f64 / (1024.0 * 1024.0))
}

fn scores_of(rows: &[Row]) -> Vec<Score> {
    rows.iter().map(|row| row.score).collect()
}

/// Formats `successes/total` with a 95% Wilson interval.
fn fraction((successes, total): (u32, u32)) -> String {
    let (low, high) = bench::wilson_interval(successes, total);
    format!("{successes}/{total} ({low:.2}–{high:.2})")
}

fn fraction_or_dash(pair: (u32, u32)) -> String {
    if pair.1 == 0 {
        "-".to_owned()
    } else {
        fraction(pair)
    }
}

fn verdict(ok: bool) -> &'static str {
    if ok {
        "PASS"
    } else {
        "FAIL"
    }
}

fn print_summary(model: &Path, family: &str, images: usize, candidate: &ModelRun) {
    let faces = candidate
        .rows
        .iter()
        .filter(|row| row.score.has_face)
        .count();
    println!("# Occlusion benchmark\n");
    println!("- Model: `{}`", model.display());
    println!("- Family: `{family}`");
    println!(
        "- Fixture images: {images} ({faces} with a face, {} face-free)",
        images - faces
    );
    if candidate.insufficient_light.is_empty() {
        println!("- Insufficient light (scored 0): none");
    } else {
        println!(
            "- Insufficient light (scored 0): {}",
            candidate.insufficient_light.join(", ")
        );
    }
}

fn print_sweep(scores: &[Score]) {
    println!("\n## Threshold sweep\n");
    println!("| Threshold | Occluded recall | Unoccluded recall | Face-free false positives |");
    println!("|---|---|---|---|");
    for threshold in bench::threshold_grid() {
        let (occluded, unoccluded, face_free) = bench::recall_summary(scores, threshold);
        println!(
            "| {threshold:.2} | {} | {} | {} |",
            fraction_or_dash(occluded),
            fraction_or_dash(unoccluded),
            fraction_or_dash(face_free)
        );
    }
}

fn print_categories(rows: &[Row], threshold: Option<f32>, given: bool) {
    println!("\n## Results by category\n");
    let Some(threshold) = threshold else {
        println!("Not computed: there is no operating threshold.");
        return;
    };
    if given {
        println!("At threshold {threshold:.2} (given on the command line).\n");
    } else {
        println!("At threshold {threshold:.2}.\n");
    }
    println!("| Category | Faces | Recall | Face-free images | False positives |");
    println!("|---|---|---|---|---|");
    let mut categories: Vec<&str> = Vec::new();
    for row in rows {
        if !categories.contains(&row.category.as_str()) {
            categories.push(&row.category);
        }
    }
    for category in categories {
        let members: Vec<Score> = rows
            .iter()
            .filter(|row| row.category == category)
            .map(|row| row.score)
            .collect();
        let faces = members.iter().filter(|s| s.has_face).count() as u32;
        let detected = members
            .iter()
            .filter(|s| s.has_face && s.max_score >= threshold)
            .count() as u32;
        let free = members.iter().filter(|s| !s.has_face).count() as u32;
        let false_positives = members
            .iter()
            .filter(|s| !s.has_face && s.max_score >= threshold)
            .count() as u32;
        println!(
            "| {category} | {faces} | {} | {free} | {} |",
            fraction_or_dash((detected, faces)),
            fraction_or_dash((false_positives, free))
        );
    }
}

/// Prints one line per candidate fixture, sorted by category then file name.
fn print_per_image(rows: &[Row]) {
    let mut sorted: Vec<&Row> = rows.iter().collect();
    sorted.sort_by(|a, b| {
        a.category
            .cmp(&b.category)
            .then_with(|| a.file.cmp(&b.file))
    });
    println!("\n## Per-image scores\n");
    println!("| File | Category | Has face | Occlusion | Max score |");
    println!("|---|---|---|---|---|");
    for row in sorted {
        let has_face = if row.score.has_face { "yes" } else { "no" };
        let score = if row.dark {
            "insufficient light".to_owned()
        } else {
            format!("{:.3}", row.score.max_score)
        };
        println!(
            "| {} | {} | {has_face} | {} | {score} |",
            row.file, row.category, row.occlusion
        );
    }
}

fn print_cost(candidate: &ModelRun) {
    println!("\n## Cost\n");
    println!(
        "- Latency: p50 {:.1} ms, p95 {:.1} ms ({LATENCY_CALLS} detect calls on `{}` after {WARM_UP_CALLS} warm-up calls)",
        candidate.p50_ms, candidate.p95_ms, candidate.latency_image
    );
    println!(
        "- Memory increase on model load (RSS): {:.1} MB",
        candidate.memory_mb
    );
}

fn print_rules(
    path: &Path,
    family: &str,
    candidate: &ModelRun,
    baseline: &ModelRun,
    threshold: Option<f32>,
    given: bool,
) {
    println!("\n## Acceptance rules\n");
    println!(
        "Baseline `{}` ({family}) is judged at the production threshold {PRODUCTION_THRESHOLD:.2}: p50 {:.1} ms.",
        path.display(),
        baseline.p50_ms
    );
    let Some(threshold) = threshold else {
        println!("Not evaluated: the candidate has no operating threshold.");
        return;
    };
    if given {
        println!(
            "The candidate is judged at threshold {threshold:.2} (given on the command line) instead of its operating threshold."
        );
    }
    let base = bench::metrics_at(&scores_of(&baseline.rows), PRODUCTION_THRESHOLD);
    let cand = bench::metrics_at(&scores_of(&candidate.rows), threshold);
    let report = bench::evaluate_rules(
        &base,
        &cand,
        baseline.p50_ms,
        candidate.p50_ms,
        candidate.memory_mb,
    );
    println!(
        "- Rule 1, occluded recall rises by at least 0.10: {} (baseline {:.2}, candidate {:.2})",
        verdict(report.rule1_occluded_gain),
        base.occluded_recall,
        cand.occluded_recall
    );
    println!(
        "- Rule 2, unoccluded recall does not drop: {} (baseline {:.2}, candidate {:.2})",
        verdict(report.rule2_no_regression),
        base.unoccluded_recall,
        cand.unoccluded_recall
    );
    println!(
        "- Rule 3, face-free false positives do not increase: {} (baseline {}, candidate {})",
        verdict(report.rule3_no_more_false_positives),
        base.face_free_false_positives,
        cand.face_free_false_positives
    );
    println!(
        "- Rule 4, p50 at most 3x baseline and 60 ms, memory increase at most 10 MB: {} (p50 {:.1} ms vs {:.1} ms, memory {:.1} MB)",
        verdict(report.rule4_budget),
        candidate.p50_ms,
        baseline.p50_ms,
        candidate.memory_mb
    );
}

/// Prints the text under the `Coverage gaps` heading of the README next to the manifest.
fn print_coverage_gaps(dir: &Path) -> Result<(), String> {
    let path = dir.join("README.md");
    let text = fs::read_to_string(&path).map_err(|err| format!("{}: {err}", path.display()))?;
    let mut in_section = false;
    let mut found = false;
    let mut body = Vec::new();
    for line in text.lines() {
        if line.starts_with('#') {
            in_section = line.trim_start_matches('#').trim() == "Coverage gaps";
            found |= in_section;
        } else if in_section {
            body.push(line);
        }
    }
    if !found {
        return Err(format!("no `Coverage gaps` heading in {}", path.display()));
    }
    println!("\n## Coverage gaps\n");
    println!("{}", body.join("\n").trim());
    Ok(())
}

/// Prints a markdown report on one ONNX model: its file size and SHA-256, the ONNX header fields
/// read by `read_onnx_header`, and the inputs and outputs that ONNX Runtime reports for it.
fn inspect_model(model: &Path, runtime: &Path) -> Result<(), String> {
    let model_path = absolute(model)?;
    let bytes = fs::read(&model_path).map_err(|err| format!("{model_path}: {err}"))?;
    let header = read_onnx_header(&bytes).map_err(|err| format!("{model_path}: {err}"))?;
    let session = open_inspection_session(&absolute(runtime)?, &model_path)?;

    println!("# Model inspection\n");
    println!("- File: `{model_path}`");
    println!("- Size: {} bytes", bytes.len());
    println!("- SHA-256: `{}`", sha256_hex(&bytes));

    println!("\n## ONNX header\n");
    match header.ir_version {
        Some(version) => println!("- IR version: {version}"),
        None => println!("- IR version: not set"),
    }
    if header.opsets.is_empty() {
        println!("- Opset imports: none");
    } else {
        println!("- Opset imports:");
        for (domain, version) in &header.opsets {
            if domain.is_empty() {
                println!("  - default ONNX domain, version {version}");
            } else {
                println!("  - `{domain}`, version {version}");
            }
        }
    }

    println!("\n## Inputs\n");
    println!("| Name | Element type | Shape |");
    println!("|---|---|---|");
    for input in &session.inputs {
        let (element, shape) = describe_type(&input.input_type);
        println!("| `{}` | {element} | {shape} |", input.name);
    }

    println!("\n## Outputs\n");
    println!("| Name | Element type | Shape |");
    println!("|---|---|---|");
    for output in &session.outputs {
        let (element, shape) = describe_type(&output.output_type);
        println!("| `{}` | {element} | {shape} |", output.name);
    }
    Ok(())
}

/// Initialises ONNX Runtime from `runtime` and opens a CPU-only session on `model`. Neither the
/// known-model table nor the digest check is involved.
fn open_inspection_session(runtime: &str, model: &str) -> Result<Session, String> {
    ort::init_from(runtime)
        .with_name("TelaViva model inspection")
        .commit()
        .map_err(|err| format!("ONNX Runtime initialisation failed: {err}"))?;
    let builder = Session::builder()
        .map_err(|err| format!("session builder failed: {err}"))?
        .with_execution_providers([CPUExecutionProvider::default().build()])
        .map_err(|err| format!("CPU provider failed: {err}"))?;
    builder
        .commit_from_file(model)
        .map_err(|err| format!("session creation failed: {err}"))
}

/// Element type and shape of one input or output, as ONNX Runtime reports them. Dynamic
/// dimensions are -1. A non-tensor value shows its runtime description and no shape.
fn describe_type(value_type: &ValueType) -> (String, String) {
    match (value_type.tensor_type(), value_type.tensor_dimensions()) {
        (Some(element), Some(dimensions)) => {
            let shape: Vec<String> = dimensions.iter().map(i64::to_string).collect();
            (element.to_string(), format!("[{}]", shape.join(", ")))
        }
        _ => (value_type.to_string(), "-".to_owned()),
    }
}

/// The ONNX `ModelProto` fields that the inspection report prints.
struct OnnxHeader {
    /// Field 1, `ir_version`. None when the model does not set it.
    ir_version: Option<u64>,
    /// Field 8, `opset_import`, as (domain, version) pairs in file order.
    opsets: Vec<(String, u64)>,
}

/// Reads the top-level `ModelProto` fields `ir_version` (field 1, varint) and `opset_import`
/// (field 8, length-delimited `OperatorSetIdProto`). Every other field is skipped by its length.
fn read_onnx_header(bytes: &[u8]) -> Result<OnnxHeader, String> {
    let mut header = OnnxHeader {
        ir_version: None,
        opsets: Vec::new(),
    };
    let mut reader = ProtoReader::new(bytes);
    while !reader.at_end() {
        let (field, wire) = reader.key()?;
        match (field, wire) {
            (1, WIRE_VARINT) => header.ir_version = Some(reader.varint()?),
            (8, WIRE_LEN) => header.opsets.push(read_opset(reader.length_delimited()?)?),
            _ => reader.skip(wire)?,
        }
    }
    Ok(header)
}

/// Reads an `OperatorSetIdProto`: `domain` (field 1, string; empty for the default ONNX domain)
/// and `version` (field 2, varint). Other fields are skipped.
fn read_opset(bytes: &[u8]) -> Result<(String, u64), String> {
    let mut reader = ProtoReader::new(bytes);
    let mut domain = String::new();
    let mut version = 0;
    while !reader.at_end() {
        let (field, wire) = reader.key()?;
        match (field, wire) {
            (1, WIRE_LEN) => domain = reader.string()?,
            (2, WIRE_VARINT) => version = reader.varint()?,
            _ => reader.skip(wire)?,
        }
    }
    Ok((domain, version))
}

/// Forward-only protobuf reader over a byte slice, covering the wire types ONNX uses.
struct ProtoReader<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> ProtoReader<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, pos: 0 }
    }

    fn at_end(&self) -> bool {
        self.pos >= self.bytes.len()
    }

    /// Reads a field key and returns (field number, wire type).
    fn key(&mut self) -> Result<(u64, u8), String> {
        let key = self.varint()?;
        Ok((key >> 3, (key & 0x7) as u8))
    }

    /// Reads a base-128 varint of at most ten bytes.
    fn varint(&mut self) -> Result<u64, String> {
        let mut value = 0_u64;
        for shift in (0_u32..64).step_by(7) {
            let byte = self.take(1)?[0];
            value |= u64::from(byte & 0x7f) << shift;
            if byte & 0x80 == 0 {
                return Ok(value);
            }
        }
        Err("varint longer than ten bytes".to_owned())
    }

    /// Reads a length prefix and the payload that follows it.
    fn length_delimited(&mut self) -> Result<&'a [u8], String> {
        let len = usize::try_from(self.varint()?)
            .map_err(|_| "length does not fit in memory".to_owned())?;
        self.take(len)
    }

    fn string(&mut self) -> Result<String, String> {
        let bytes = self.length_delimited()?;
        String::from_utf8(bytes.to_vec()).map_err(|_| "string field is not UTF-8".to_owned())
    }

    /// Skips the value of a field with the given wire type.
    fn skip(&mut self, wire: u8) -> Result<(), String> {
        match wire {
            WIRE_VARINT => self.varint().map(|_| ()),
            WIRE_FIXED64 => self.take(8).map(|_| ()),
            WIRE_LEN => self.length_delimited().map(|_| ()),
            WIRE_FIXED32 => self.take(4).map(|_| ()),
            other => Err(format!("unsupported protobuf wire type {other}")),
        }
    }

    /// Returns the next `len` bytes and advances past them.
    fn take(&mut self, len: usize) -> Result<&'a [u8], String> {
        let truncated = || "truncated protobuf data".to_owned();
        let bytes = self.bytes;
        let end = self.pos.checked_add(len).ok_or_else(truncated)?;
        let slice = bytes.get(self.pos..end).ok_or_else(truncated)?;
        self.pos = end;
        Ok(slice)
    }
}

/// Lowercase hex SHA-256 digest of `bytes`.
fn sha256_hex(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}
