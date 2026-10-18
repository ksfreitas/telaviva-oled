use serde::Deserialize;
use serde_json::{json, Value};
use std::cmp::Ordering;

pub const MAX_BODY_BYTES: usize = 1024 * 1024;
pub const MAX_VERSION_BYTES: usize = 64;

const RELEASE_URL_PREFIX: &str = "https://github.com/ksfreitas/telaviva-oled/releases/tag/";

/// Returns the version of this core, as declared in its crate manifest.
pub fn core_version() -> &'static str {
    env!("CARGO_PKG_VERSION")
}

/// Reasons the update check cannot be evaluated. The response itself is still reported as an
/// outcome when one of these is not returned.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpdateError {
    InvalidInstalled,
    BodyTooLarge,
    InstalledTooLarge,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Version {
    major: u64,
    minor: u64,
    patch: u64,
    prerelease: bool,
}

impl Version {
    /// Strict semver: MAJOR.MINOR.PATCH, optional -prerelease, optional +build (ignored).
    /// A leading `v` is not accepted here; tags strip it before calling this.
    fn parse(text: &str) -> Option<Self> {
        let (rest, build) = match text.split_once('+') {
            Some((rest, build)) => (rest, Some(build)),
            None => (text, None),
        };
        if let Some(build) = build {
            if !valid_identifiers(build) {
                return None;
            }
        }
        let (core, prerelease) = match rest.split_once('-') {
            Some((core, prerelease)) => (core, Some(prerelease)),
            None => (rest, None),
        };
        if let Some(prerelease) = prerelease {
            if !valid_identifiers(prerelease) {
                return None;
            }
        }
        let mut parts = core.split('.');
        let major = parse_number(parts.next()?)?;
        let minor = parse_number(parts.next()?)?;
        let patch = parse_number(parts.next()?)?;
        if parts.next().is_some() {
            return None;
        }
        Some(Self {
            major,
            minor,
            patch,
            prerelease: prerelease.is_some(),
        })
    }
}

impl Ord for Version {
    fn cmp(&self, other: &Self) -> Ordering {
        (self.major, self.minor, self.patch)
            .cmp(&(other.major, other.minor, other.patch))
            // With equal numbers a pre-release sorts below the release.
            .then_with(|| other.prerelease.cmp(&self.prerelease))
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// Dot-separated, non-empty identifiers of [0-9A-Za-z-].
fn valid_identifiers(text: &str) -> bool {
    text.split('.').all(|identifier| {
        !identifier.is_empty()
            && identifier
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    })
}

/// Unsigned decimal without leading zeros (a lone "0" is allowed) that fits in u64.
fn parse_number(text: &str) -> Option<u64> {
    let digits_only = !text.is_empty() && text.bytes().all(|byte| byte.is_ascii_digit());
    if !digits_only || (text.len() > 1 && text.starts_with('0')) {
        return None;
    }
    text.parse::<u64>().ok()
}

/// Shape check applied to the trimmed tag before semver parsing: `^[vV]?[0-9A-Za-z.+-]+$`.
fn tag_chars_valid(tag: &str) -> bool {
    !tag.is_empty()
        && tag
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'+' | b'-'))
}

fn strip_v(tag: &str) -> &str {
    tag.strip_prefix(['v', 'V']).unwrap_or(tag)
}

#[derive(Deserialize)]
struct ReleaseBody {
    tag_name: String,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    prerelease: bool,
}

/// Compares the installed version with a GitHub releases/latest answer and returns the
/// UpdateOutcome JSON text. Pure: no I/O, no network, no state.
pub fn evaluate(installed: &str, http_status: u32, body: &[u8]) -> Result<String, UpdateError> {
    if installed.len() > MAX_VERSION_BYTES {
        return Err(UpdateError::InstalledTooLarge);
    }
    let installed_version = Version::parse(installed).ok_or(UpdateError::InvalidInstalled)?;
    if body.len() > MAX_BODY_BYTES {
        return Err(UpdateError::BodyTooLarge);
    }
    let outcome = match http_status {
        200 => evaluate_release(&installed_version, body),
        404 => json!({"status": "no_release"}),
        403 | 429 => json!({"status": "rate_limited"}),
        _ => json!({"status": "http_error"}),
    };
    Ok(outcome.to_string())
}

fn evaluate_release(installed: &Version, body: &[u8]) -> Value {
    let release: ReleaseBody = match serde_json::from_slice(body) {
        Ok(release) => release,
        Err(_) => return json!({"status": "invalid_response"}),
    };
    if release.draft || release.prerelease {
        return json!({"status": "no_release"});
    }
    let tag = release.tag_name.trim();
    if !tag_chars_valid(tag) {
        return json!({"status": "invalid_response"});
    }
    let bare_tag = strip_v(tag);
    let remote = match Version::parse(bare_tag) {
        Some(remote) => remote,
        None => return json!({"status": "invalid_response"}),
    };
    if remote.prerelease {
        return json!({"status": "no_release"});
    }
    let remote_version = bare_tag
        .split_once('+')
        .map_or(bare_tag, |(version, _)| version);
    if remote > *installed {
        json!({
            "status": "update_available",
            "remote_version": remote_version,
            "release_url": format!("{RELEASE_URL_PREFIX}{tag}"),
        })
    } else {
        json!({
            "status": "up_to_date",
            "remote_version": remote_version,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(installed: &str, status: u32, body: &str) -> Value {
        let text = evaluate(installed, status, body.as_bytes())
            .expect("evaluate should accept these inputs");
        serde_json::from_str(&text).expect("outcome must be JSON")
    }

    fn tag_body(tag: &str) -> String {
        json!({ "tag_name": tag }).to_string()
    }

    #[test]
    fn equal_version_is_up_to_date() {
        let outcome = run("1.2.3", 200, &tag_body("v1.2.3"));
        assert_eq!(outcome["status"], "up_to_date");
        assert_eq!(outcome["remote_version"], "1.2.3");
    }

    #[test]
    fn lower_remote_is_up_to_date() {
        assert_eq!(
            run("1.2.3", 200, &tag_body("v1.2.2"))["status"],
            "up_to_date"
        );
        assert_eq!(
            run("1.2.3", 200, &tag_body("v1.1.9"))["status"],
            "up_to_date"
        );
        assert_eq!(
            run("1.2.3", 200, &tag_body("v0.9.9"))["status"],
            "up_to_date"
        );
    }

    #[test]
    fn higher_patch_minor_or_major_is_update_available() {
        assert_eq!(
            run("1.2.3", 200, &tag_body("v1.2.4"))["status"],
            "update_available"
        );
        assert_eq!(
            run("1.2.3", 200, &tag_body("v1.3.0"))["status"],
            "update_available"
        );
        assert_eq!(
            run("1.2.3", 200, &tag_body("v2.0.0"))["status"],
            "update_available"
        );
    }

    #[test]
    fn minor_compares_numerically() {
        assert_eq!(
            run("0.9.0", 200, &tag_body("v0.10.0"))["status"],
            "update_available"
        );
        assert_eq!(
            run("0.10.0", 200, &tag_body("v0.9.0"))["status"],
            "up_to_date"
        );
    }

    #[test]
    fn leading_v_and_capital_v_are_accepted_in_tags() {
        let lower = run("0.1.0", 200, &tag_body("v0.2.0"));
        assert_eq!(lower["status"], "update_available");
        assert_eq!(lower["remote_version"], "0.2.0");
        let upper = run("0.1.0", 200, &tag_body("V0.2.0"));
        assert_eq!(upper["status"], "update_available");
        assert_eq!(upper["remote_version"], "0.2.0");
    }

    #[test]
    fn installed_prerelease_is_older_than_release() {
        assert_eq!(
            run("1.0.0-rc.1", 200, &tag_body("v1.0.0"))["status"],
            "update_available"
        );
    }

    #[test]
    fn remote_prerelease_is_never_offered() {
        assert_eq!(
            run("1.0.0", 200, &tag_body("v1.0.0-rc.1"))["status"],
            "no_release"
        );
    }

    #[test]
    fn prerelease_orders_below_release_only_with_equal_numbers() {
        let rc = Version::parse("1.0.0-rc.1").expect("valid");
        let release = Version::parse("1.0.0").expect("valid");
        let next = Version::parse("1.0.1-rc.1").expect("valid");
        assert!(rc < release);
        assert!(rc < next);
        assert!(release < next);
    }

    #[test]
    fn draft_or_prerelease_flag_is_no_release() {
        let draft = json!({ "tag_name": "v9.0.0", "draft": true }).to_string();
        assert_eq!(run("1.0.0", 200, &draft)["status"], "no_release");
        let prerelease = json!({ "tag_name": "v9.0.0", "prerelease": true }).to_string();
        assert_eq!(run("1.0.0", 200, &prerelease)["status"], "no_release");
        let both = json!({ "tag_name": "v9.0.0", "draft": true, "prerelease": true }).to_string();
        assert_eq!(run("1.0.0", 200, &both)["status"], "no_release");
    }

    #[test]
    fn malformed_tags_are_invalid_response() {
        for tag in ["1.2", "1.2.3.4", "01.2.3", "v", "", "1.2.x", "../x"] {
            assert_eq!(
                run("1.0.0", 200, &tag_body(tag))["status"],
                "invalid_response",
                "tag {tag:?} should be rejected"
            );
        }
    }

    #[test]
    fn unusable_200_bodies_are_invalid_response() {
        assert_eq!(run("1.0.0", 200, "{}")["status"], "invalid_response");
        assert_eq!(
            run("1.0.0", 200, r#"{"tag_name": 100}"#)["status"],
            "invalid_response"
        );
        assert_eq!(run("1.0.0", 200, "{not json")["status"], "invalid_response");
        assert_eq!(run("1.0.0", 200, "")["status"], "invalid_response");
    }

    #[test]
    fn http_statuses_map_to_outcomes() {
        assert_eq!(run("1.0.0", 404, "")["status"], "no_release");
        assert_eq!(run("1.0.0", 403, "")["status"], "rate_limited");
        assert_eq!(run("1.0.0", 429, "")["status"], "rate_limited");
        assert_eq!(run("1.0.0", 500, "")["status"], "http_error");
        assert_eq!(run("1.0.0", 304, "")["status"], "http_error");
    }

    #[test]
    fn release_url_is_built_from_tag_and_ignores_html_url() {
        let body = json!({
            "tag_name": "v0.2.0",
            "html_url": "https://evil.example/phish",
        })
        .to_string();
        let outcome = run("0.1.0", 200, &body);
        assert_eq!(outcome["status"], "update_available");
        assert_eq!(
            outcome["release_url"],
            "https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0"
        );
    }

    #[test]
    fn build_metadata_in_tag_is_ignored_for_version_but_not_url() {
        let outcome = run("1.0.0", 200, &tag_body("v1.0.0+build.5"));
        assert_eq!(outcome["status"], "up_to_date");
        assert_eq!(outcome["remote_version"], "1.0.0");
    }

    #[test]
    fn malformed_installed_is_rejected() {
        for installed in ["abc", "", "1.2", "v1.0.0", "1.0.0 ", "01.0.0"] {
            assert_eq!(
                evaluate(installed, 200, b"{}"),
                Err(UpdateError::InvalidInstalled),
                "installed {installed:?} should be rejected"
            );
        }
    }

    #[test]
    fn oversized_installed_is_rejected() {
        let installed = format!("1.0.0-{}", "a".repeat(MAX_VERSION_BYTES));
        assert_eq!(
            evaluate(&installed, 200, b"{}"),
            Err(UpdateError::InstalledTooLarge)
        );
    }

    #[test]
    fn oversized_body_is_rejected() {
        let body = vec![b' '; MAX_BODY_BYTES + 1];
        assert_eq!(
            evaluate("1.0.0", 200, &body),
            Err(UpdateError::BodyTooLarge)
        );
    }

    #[test]
    fn core_version_is_strict_semver() {
        assert!(Version::parse(core_version()).is_some());
    }
}
