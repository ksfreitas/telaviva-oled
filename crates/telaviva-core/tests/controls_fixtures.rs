//! Replays the scenario fixtures in `tests/fixtures/controls` through `telaviva_core::controls`.
//!
//! Each fixture is one scenario: a list of steps, each with a Controls `event` and an optional
//! `expect` holding the `value`, `effects` and `next_deadline` of the result. Numbers may differ
//! by `TOLERANCE`, because deadlines add 20 ms, which is not exact in binary. With
//! `TELAVIVA_UPDATE_FIXTURES=1` the test rewrites every fixture's `expect` members from the actual
//! output.

use serde_json::Value;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use telaviva_core::controls::Controls;

const TOLERANCE: f64 = 1e-9;
const UPDATE_VARIABLE: &str = "TELAVIVA_UPDATE_FIXTURES";

/// The fixture directory, relative to this crate.
fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/controls")
}

/// Every `*.json` file in the fixture directory. All files share one directory, so the full path
/// order is the file name order.
fn fixture_files() -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = fs::read_dir(fixture_dir())
        .expect("the fixture directory exists")
        .map(|entry| entry.expect("a readable directory entry").path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "json"))
        .collect();
    files.sort();
    files
}

/// Equality where numbers may differ by `TOLERANCE`. Everything else must match exactly.
fn same(expected: &Value, actual: &Value) -> bool {
    match (expected, actual) {
        (Value::Number(a), Value::Number(b)) => match (a.as_f64(), b.as_f64()) {
            (Some(x), Some(y)) => x == y || (x - y).abs() <= TOLERANCE,
            _ => a == b,
        },
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b).all(|(x, y)| same(x, y))
        }
        (Value::Object(a), Value::Object(b)) => {
            a.len() == b.len()
                && a.iter()
                    .all(|(key, x)| b.get(key).is_some_and(|y| same(x, y)))
        }
        _ => expected == actual,
    }
}

/// Replays one fixture on a fresh machine. In update mode, rewrites the file with the actual
/// results as `expect`.
fn replay(path: &Path, update: bool) {
    let name = path.to_string_lossy().into_owned();
    let text = fs::read_to_string(path).expect("readable");
    let mut fixture: Value = serde_json::from_str(&text).expect("JSON");
    let steps = fixture["steps"]
        .as_array_mut()
        .expect("the fixture has a steps array");
    assert!(!steps.is_empty(), "{name} has no steps");

    let mut controls = Controls::new();
    for (index, step) in steps.iter_mut().enumerate() {
        let label = format!("{name} step {index}");
        let bytes = serde_json::to_vec(&step["event"]).expect("event");
        let output = controls
            .step(&bytes)
            .unwrap_or_else(|error| panic!("{label}: rejected: {error:?}"));
        let actual: Value = serde_json::from_str(&output).expect("JSON");

        if update {
            step["expect"] = actual;
        } else {
            let expected = step.get("expect").unwrap_or_else(|| {
                panic!("{label}: no expect; run with {UPDATE_VARIABLE}=1 to generate it")
            });
            assert!(
                same(expected, &actual),
                "{label}: expected {expected}, got {actual}"
            );
        }
    }

    if update {
        let mut out = serde_json::to_string_pretty(&fixture).expect("serializes");
        out.push('\n');
        fs::write(path, out).expect("writable");
    }
}

#[test]
fn controls_fixtures_replay() {
    let update = env::var(UPDATE_VARIABLE).is_ok_and(|v| v == "1");
    let files = fixture_files();
    assert!(!files.is_empty(), "no controls fixtures found");
    for path in files {
        replay(&path, update);
    }
}
