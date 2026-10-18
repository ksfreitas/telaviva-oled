use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

pub const MAX_CAMERA_HISTORY: usize = 8;
const MAX_MONITOR_ENTRIES: usize = 128;
const MAX_MONITOR_ID_BYTES: usize = 512;
pub const MIN_CURVE_POINTS: usize = 2;
pub const MAX_CURVE_POINTS: usize = 32;

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
    #[serde(default = "default_presence_min_brightness")]
    pub presence_min_brightness_percent: f64,
    #[serde(default)]
    pub always_on_camera: bool,
    #[serde(default)]
    pub screensaver_protection_enabled: bool,
    #[serde(default)]
    pub launch_at_login: bool,
    #[serde(default)]
    pub hardware_keys_enabled: bool,
    #[serde(default = "default_key_step")]
    pub brightness_key_step: u8,
    #[serde(default = "default_key_step")]
    pub volume_key_step: u8,
    #[serde(default = "default_inference_device")]
    pub inference_device: String,
    #[serde(default)]
    pub selected_camera_id: Option<String>,
    // Most recent first. The front entry is always selected_camera_id; clients cannot set it.
    #[serde(default)]
    pub camera_history: Vec<String>,
    #[serde(default)]
    pub screensaver_targets: std::collections::BTreeMap<String, f64>,
    #[serde(default)]
    pub ambient_follow_enabled: bool,
    #[serde(default)]
    pub ambient_curves: std::collections::BTreeMap<String, AmbientCurve>,
    #[serde(default)]
    pub protection_disabled: std::collections::BTreeSet<String>,
}

// One monitor's calibration. Each point pairs a monitor brightness percent (`monitor`) with the
// MacBook level percent (`builtin`) at which the built-in screen looks equal to the monitor at that
// brightness. Points are strictly ascending by `monitor` and never decrease in `builtin`. A curve is
// complete with at least MIN_CURVE_POINTS points. Legacy p1..p100 keys are read as points.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(from = "AmbientCurveRepr")]
pub struct AmbientCurve {
    pub points: Vec<AmbientPoint>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AmbientPoint {
    pub monitor: f64,
    pub builtin: f64,
}

// Accepted input shapes. `Points` is tried first; `Legacy` reads the fixed pN keys of older files.
#[derive(Deserialize)]
#[serde(untagged, deny_unknown_fields)]
enum AmbientCurveRepr {
    Points {
        points: Vec<AmbientPoint>,
    },
    Legacy {
        #[serde(default)]
        p1: Option<f64>,
        #[serde(default)]
        p5: Option<f64>,
        #[serde(default)]
        p10: Option<f64>,
        #[serde(default)]
        p15: Option<f64>,
        #[serde(default)]
        p20: Option<f64>,
        #[serde(default)]
        p25: Option<f64>,
        #[serde(default)]
        p50: Option<f64>,
        #[serde(default)]
        p75: Option<f64>,
        #[serde(default)]
        p100: Option<f64>,
    },
}

impl From<AmbientCurveRepr> for AmbientCurve {
    fn from(repr: AmbientCurveRepr) -> Self {
        let points = match repr {
            AmbientCurveRepr::Points { points } => points,
            // Ascending N, so the legacy points keep the strict monitor order.
            AmbientCurveRepr::Legacy {
                p1,
                p5,
                p10,
                p15,
                p20,
                p25,
                p50,
                p75,
                p100,
            } => [
                (1.0, p1),
                (5.0, p5),
                (10.0, p10),
                (15.0, p15),
                (20.0, p20),
                (25.0, p25),
                (50.0, p50),
                (75.0, p75),
                (100.0, p100),
            ]
            .into_iter()
            .filter_map(|(monitor, builtin)| {
                builtin.map(|builtin| AmbientPoint { monitor, builtin })
            })
            .collect(),
        };
        Self { points }
    }
}

impl AmbientCurve {
    pub fn is_complete(&self) -> bool {
        self.points.len() >= MIN_CURVE_POINTS
    }

    fn validate(&self) -> Result<(), String> {
        if self.points.len() > MAX_CURVE_POINTS {
            return Err("too many ambient curve points".into());
        }
        let mut previous: Option<&AmbientPoint> = None;
        for point in &self.points {
            if !point.monitor.is_finite()
                || !point.builtin.is_finite()
                || point.monitor <= 0.0
                || point.monitor > 100.0
                || !(0.0..=100.0).contains(&point.builtin)
            {
                return Err("invalid ambient curve point".into());
            }
            if let Some(prev) = previous {
                if point.monitor <= prev.monitor {
                    return Err("ambient curve points must be strictly ascending".into());
                }
                if point.builtin < prev.builtin {
                    return Err("ambient curve points must not decrease".into());
                }
            }
            previous = Some(point);
        }
        Ok(())
    }
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
fn default_key_step() -> u8 {
    5
}
fn default_presence_min_brightness() -> f64 {
    20.0
}
fn default_inference_device() -> String {
    "auto".into()
}

fn valid_inference_device(value: &str) -> bool {
    if value.len() > 32 {
        return false;
    }
    if matches!(value, "auto" | "cpu" | "ane" | "gpu") {
        return true;
    }
    let Some(rest) = value.strip_prefix("gpu:") else {
        return false;
    };
    let mut parts = rest.split(':');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some(vendor), Some(device), Some(ordinal), None) => {
            is_hex4(vendor) && is_hex4(device) && is_ordinal(ordinal)
        }
        _ => false,
    }
}

fn is_hex4(value: &str) -> bool {
    value.len() == 4
        && value
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

fn is_ordinal(value: &str) -> bool {
    (1..=3).contains(&value.len()) && value.bytes().all(|b| b.is_ascii_digit())
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
            presence_min_brightness_percent: 20.0,
            always_on_camera: false,
            screensaver_protection_enabled: false,
            launch_at_login: false,
            hardware_keys_enabled: false,
            brightness_key_step: 5,
            volume_key_step: 5,
            inference_device: "auto".into(),
            selected_camera_id: None,
            camera_history: Vec::new(),
            screensaver_targets: Default::default(),
            ambient_follow_enabled: false,
            ambient_curves: Default::default(),
            protection_disabled: Default::default(),
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
        if !valid_inference_device(&self.inference_device) {
            return Err(
                "inference_device must be auto, cpu, ane, gpu or gpu:<vendor>:<device>:<ordinal>"
                    .into(),
            );
        }
        if self
            .selected_camera_id
            .as_ref()
            .map(|id| id.is_empty() || id.len() > 512)
            .unwrap_or(false)
        {
            return Err("invalid selected camera ID".into());
        }
        if self.camera_history.len() > MAX_CAMERA_HISTORY {
            return Err("camera history exceeds its limit".into());
        }
        for (index, id) in self.camera_history.iter().enumerate() {
            if id.is_empty() || id.len() > 512 {
                return Err("invalid camera history entry".into());
            }
            if self.camera_history[..index].contains(id) {
                return Err("duplicate camera history entry".into());
            }
        }
        if self.selected_camera_id.as_ref() != self.camera_history.first() {
            return Err("selected camera must be the first camera history entry".into());
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
        if !(1..=15).contains(&self.brightness_key_step) {
            return Err("brightness key step out of range".into());
        }
        if !(1..=15).contains(&self.volume_key_step) {
            return Err("volume key step out of range".into());
        }
        if !self.presence_min_brightness_percent.is_finite()
            || !(10.0..=90.0).contains(&self.presence_min_brightness_percent)
        {
            return Err("presence brightness threshold out of range".into());
        }
        if self
            .screensaver_targets
            .iter()
            .any(|(k, v)| k.is_empty() || !v.is_finite() || !(0.0..=100.0).contains(v))
        {
            return Err("invalid screensaver target".into());
        }
        if self.ambient_curves.len() > MAX_MONITOR_ENTRIES {
            return Err("ambient curves exceed their limit".into());
        }
        for (id, curve) in &self.ambient_curves {
            if id.is_empty() || id.len() > MAX_MONITOR_ID_BYTES {
                return Err("invalid ambient curve monitor ID".into());
            }
            curve.validate()?;
        }
        if self.protection_disabled.len() > MAX_MONITOR_ENTRIES {
            return Err("protection exceptions exceed their limit".into());
        }
        if self
            .protection_disabled
            .iter()
            .any(|id| id.is_empty() || id.len() > MAX_MONITOR_ID_BYTES)
        {
            return Err("invalid protection monitor ID".into());
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
                    // Files written before camera_history existed only have selected_camera_id.
                    // Seed the history from it so the legacy selection is not treated as corrupt.
                    if s.camera_history.is_empty() {
                        s.camera_history = s.selected_camera_id.iter().cloned().collect();
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
