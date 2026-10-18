//! Version consistency test for the application's version copies.
//!
//! The version of this crate (`telaviva-core`) is the source of truth. The inference crate's
//! `Cargo.toml`, the macOS `Info.plist` (`CFBundleShortVersionString` and `CFBundleVersion`) and
//! the Windows application manifest must carry the same version. The files are read as text and
//! searched with plain string operations, so no extra dependency is needed.

const CORE_VERSION: &str = env!("CARGO_PKG_VERSION");
const INFERENCE_CARGO_TOML: &str = include_str!("../../telaviva-inference/Cargo.toml");
const MACOS_INFO_PLIST: &str = include_str!("../../../native/macos/TelaViva/Info.plist");
const WINDOWS_APP_MANIFEST: &str = include_str!("../../../native/windows/TelaViva/app.manifest");

/// Returns the value of the first `version = "..."` line inside the `[package]` table.
fn package_version(cargo_toml: &str) -> Option<&str> {
    let mut in_package = false;
    for line in cargo_toml.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') {
            in_package = trimmed == "[package]";
            continue;
        }
        if in_package {
            if let Some(rest) = trimmed.strip_prefix("version = \"") {
                return rest.split('"').next();
            }
        }
    }
    None
}

/// Returns the text of the `<string>` element that follows `<key>{key}</key>`. Only whitespace
/// may separate the key from the element.
fn plist_string_value<'a>(plist: &'a str, key: &str) -> Option<&'a str> {
    let key_tag = format!("<key>{key}</key>");
    let after_key = plist.find(&key_tag)? + key_tag.len();
    let rest = &plist[after_key..];
    let open_tag = "<string>";
    let open = rest.find(open_tag)?;
    if !rest[..open].trim().is_empty() {
        return None;
    }
    let value = &rest[open + open_tag.len()..];
    let close = value.find("</string>")?;
    Some(value[..close].trim())
}

/// Returns the `version` attribute of the `assemblyIdentity` element.
fn manifest_assembly_version(manifest: &str) -> Option<&str> {
    let start = manifest.find("<assemblyIdentity")?;
    let element = &manifest[start..];
    let element = &element[..element.find('>')?];
    let attr_tag = " version=\"";
    let attr_start = element.find(attr_tag)? + attr_tag.len();
    let value = &element[attr_start..];
    Some(&value[..value.find('"')?])
}

#[test]
fn inference_crate_version_matches_core() {
    assert_eq!(
        package_version(INFERENCE_CARGO_TOML),
        Some(CORE_VERSION),
        "crates/telaviva-inference/Cargo.toml [package] version must be \"{CORE_VERSION}\" \
         (the telaviva-core version); update the `version` line in that file"
    );
}

#[test]
fn macos_info_plist_versions_match_core() {
    for key in ["CFBundleShortVersionString", "CFBundleVersion"] {
        assert_eq!(
            plist_string_value(MACOS_INFO_PLIST, key),
            Some(CORE_VERSION),
            "native/macos/TelaViva/Info.plist <key>{key}</key> must be followed by \
             <string>{CORE_VERSION}</string>; update that value in the plist"
        );
    }
}

#[test]
fn windows_manifest_version_matches_core() {
    let expected = format!("{CORE_VERSION}.0");
    assert_eq!(
        manifest_assembly_version(WINDOWS_APP_MANIFEST),
        Some(expected.as_str()),
        "native/windows/TelaViva/app.manifest assemblyIdentity version must be \"{expected}\" \
         (four-part form of the telaviva-core version); update the version attribute in that file"
    );
}
