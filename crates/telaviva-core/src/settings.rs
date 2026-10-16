use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Settings {
    #[serde(default = "schema_version")]
    pub schema_version: u32,
    #[serde(default = "default_locale")]
    pub locale: String,
    #[serde(default = "default_presence_interval")]
    pub presence_sampling_interval_s: f64,
    #[serde(default = "default_return_interval")]
    pub return_sampling_interval_s: f64,
    #[serde(default = "yes")]
    pub suspend_presence_on_battery: bool,
    #[serde(default)]
    pub presence_protection_enabled: bool,
    #[serde(default)]
    pub always_on_camera: bool,
    #[serde(default)]
    pub screensaver_protection_enabled: bool,
    #[serde(default)]
    pub launch_at_login: bool,
    #[serde(default)]
    pub hardware_keys_enabled: bool,
    #[serde(default)]
    pub selected_camera_id: Option<String>,
    #[serde(default)]
    pub screensaver_targets: std::collections::BTreeMap<String, f64>,
}

fn schema_version() -> u32 {
    1
}
fn default_locale() -> String {
    "auto".into()
}
fn default_presence_interval() -> f64 {
    10.0
}
fn default_return_interval() -> f64 {
    1.0
}
fn yes() -> bool {
    true
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            schema_version: 1,
            locale: "auto".into(),
            presence_sampling_interval_s: 10.0,
            return_sampling_interval_s: 1.0,
            suspend_presence_on_battery: true,
            presence_protection_enabled: false,
            always_on_camera: false,
            screensaver_protection_enabled: false,
            launch_at_login: false,
            hardware_keys_enabled: false,
            selected_camera_id: None,
            screensaver_targets: Default::default(),
        }
    }
}

impl Settings {
    pub fn from_json(bytes: &[u8]) -> Result<Self, String> {
        let value: Self = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
        value.validate()?;
        Ok(value)
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.schema_version != 1 {
            return Err("unsupported settings schema_version".into());
        }
        if !["auto", "en-US", "pt-BR"].contains(&self.locale.as_str()) {
            return Err("unsupported locale".into());
        }
        if self
            .selected_camera_id
            .as_ref()
            .map(|id| id.is_empty() || id.len() > 512)
            .unwrap_or(false)
        {
            return Err("invalid selected camera ID".into());
        }
        if !self.presence_sampling_interval_s.is_finite()
            || !(5.0..=300.0).contains(&self.presence_sampling_interval_s)
        {
            return Err("presence interval out of range".into());
        }
        if !self.return_sampling_interval_s.is_finite()
            || !(0.25..=5.0).contains(&self.return_sampling_interval_s)
        {
            return Err("return interval out of range".into());
        }
        if self
            .screensaver_targets
            .iter()
            .any(|(k, v)| k.is_empty() || !v.is_finite() || !(0.0..=100.0).contains(v))
        {
            return Err("invalid screensaver target".into());
        }
        Ok(())
    }
}

pub(crate) fn load(path: &Path) -> Result<Settings, String> {
    match fs::read(path) {
        Ok(bytes) => {
            if serde_json::from_slice::<serde_json::Value>(&bytes)
                .ok()
                .and_then(|v| v.get("schema_version").and_then(serde_json::Value::as_u64))
                .map(|v| v > 1)
                .unwrap_or(false)
            {
                return Err("settings use a newer schema version; source preserved".into());
            }
            match serde_json::from_slice::<Settings>(&bytes) {
                Ok(mut s) => {
                    // Schema 1 predates the tighter UI cadence bounds. Normalize only these
                    // fields so valid saved preferences survive the bounds change.
                    let old_ranges_valid = s.presence_sampling_interval_s.is_finite()
                        && (1.0..=3600.0).contains(&s.presence_sampling_interval_s)
                        && s.return_sampling_interval_s.is_finite()
                        && (0.25..=60.0).contains(&s.return_sampling_interval_s);
                    if old_ranges_valid {
                        s.presence_sampling_interval_s =
                            s.presence_sampling_interval_s.clamp(5.0, 300.0);
                        s.return_sampling_interval_s =
                            s.return_sampling_interval_s.clamp(0.25, 5.0);
                    }
                    if s.validate().is_ok() {
                        Ok(s)
                    } else {
                        backup_corrupt(path)?;
                        Ok(Settings::default())
                    }
                }
                Err(_) => {
                    backup_corrupt(path)?;
                    Ok(Settings::default())
                }
            }
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Settings::default()),
        Err(e) => Err(format!("read settings: {e}")),
    }
}

fn backup_corrupt(path: &Path) -> Result<(), String> {
    let mut backup = PathBuf::from(path);
    backup.set_extension("json.corrupt");
    if backup.exists() {
        backup = PathBuf::from(format!("{}.{}.corrupt", path.display(), std::process::id()));
    }
    fs::rename(path, backup).map_err(|e| format!("preserve corrupt settings: {e}"))
}

pub(crate) fn store(path: &Path, settings: &Settings) -> Result<(), String> {
    settings.validate()?;
    let parent = path.parent().ok_or("settings path has no parent")?;
    fs::create_dir_all(parent).map_err(|e| format!("create settings directory: {e}"))?;
    let tmp = path.with_extension(format!("tmp.{}", std::process::id()));
    let bytes = serde_json::to_vec_pretty(settings).map_err(|e| e.to_string())?;
    let mut f = fs::File::create(&tmp).map_err(|e| format!("create settings temp file: {e}"))?;
    f.write_all(&bytes)
        .and_then(|_| f.sync_all())
        .map_err(|e| format!("write settings temp file: {e}"))?;
    replace_file(&tmp, path).map_err(|e| format!("replace settings: {e}"))
}

#[cfg(not(target_os = "windows"))]
fn replace_file(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::rename(from, to)
}

#[cfg(target_os = "windows")]
fn replace_file(from: &Path, to: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "Kernel32")]
    #[allow(non_snake_case)]
    extern "system" {
        fn MoveFileExW(existing: *const u16, new: *const u16, flags: u32) -> i32;
    }
    const MOVEFILE_REPLACE_EXISTING: u32 = 0x1;
    const MOVEFILE_WRITE_THROUGH: u32 = 0x8;
    let from: Vec<u16> = from
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let to: Vec<u16> = to
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let ok = unsafe {
        MoveFileExW(
            from.as_ptr(),
            to.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if ok == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}
