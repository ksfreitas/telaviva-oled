use crate::settings::{self, AmbientCurve, Settings};
use serde::{
    de::{MapAccess, SeqAccess, Visitor},
    Deserialize, Deserializer,
};
use serde_json::{json, Map, Number, Value};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    path::PathBuf,
    time::{SystemTime, UNIX_EPOCH},
};

const MAX_ACTIONS: usize = 128;
const MAX_MONITORS: usize = 64;
const MAX_RESTORE_FAILURES: u8 = 3;
// First face sample after a resume edge (display wake, battery to AC, protection re-enabled).
const RESUME_FIRST_SAMPLE_MS: f64 = 1000.0;

#[derive(Clone, Debug, Default)]
pub struct CoreOptions {
    pub config_path: Option<PathBuf>,
    pub os_locale: Option<String>,
}

#[derive(Clone, Debug)]
struct Feature {
    supported: bool,
    observed: Option<f64>,
    raw: Option<f64>,
    min: f64,
    max: f64,
    desired: Option<f64>,
    status: String,
    owned: bool,
    ownership_checked_target: Option<f64>,
    reasons: Vec<String>,
}

#[derive(Clone, Debug)]
struct Monitor {
    id: String,
    name: String,
    identity: String,
    path: String,
    generation: u64,
    display_state: String,
    brightness: Feature,
    volume: Feature,
    baseline: Option<f64>,
    // Ambient follow target for the current built-in level. Runtime only.
    follow_raw: Option<f64>,
    presence_dimmed: bool,
    saver_dimmed: bool,
    misses: u8,
    // Consecutive failed wake readbacks or restore writes while ownership is kept.
    restore_failures: u8,
    manual_override: bool,
    wake_ownership_check: bool,
}

#[derive(Clone, Debug)]
struct Action {
    id: u64,
    monitor_id: String,
    generation: u64,
    kind: String,
    value: Option<f64>,
    target: Option<f64>,
}

pub struct Core {
    settings: Settings,
    config_path: Option<PathBuf>,
    // Accepted by tv_create for ABI compatibility; no longer affects snapshot content.
    #[allow(dead_code)]
    os_locale: String,
    monitors: BTreeMap<String, Monitor>,
    connection_generations: BTreeMap<String, u64>,
    actions: VecDeque<Action>,
    revision: u64,
    next_id: u64,
    camera_generation: u64,
    camera_status: String,
    on_battery: Option<bool>,
    saver_state: String,
    low_light: bool,
    usable_light_samples: u8,
    resume_pending: bool,
    last_presence_ms: u64,
    // Result of the latest accepted face-mode sample; cleared on any camera generation change.
    last_presence: Option<&'static str>,
    closed: bool,
    // Built-in display state reported by the shell. Runtime only, never persisted.
    builtin_available: bool,
    builtin_percent: Option<f64>,
    calibrating: bool,
    // Monitors at or below the presence brightness threshold. Runtime only, never persisted.
    below_threshold: BTreeSet<String>,
}

impl Core {
    pub fn new(options: CoreOptions) -> Result<Self, String> {
        let settings = options
            .config_path
            .as_deref()
            .map(settings::load)
            .transpose()?
            .unwrap_or_default();
        Ok(Self {
            settings,
            config_path: options.config_path,
            os_locale: options.os_locale.unwrap_or_else(|| "en-US".into()),
            monitors: BTreeMap::new(),
            connection_generations: BTreeMap::new(),
            actions: VecDeque::new(),
            revision: 1,
            next_id: 1,
            camera_generation: 1,
            camera_status: "unknown".into(),
            on_battery: None,
            saver_state: "unknown".into(),
            low_light: false,
            usable_light_samples: 0,
            resume_pending: false,
            last_presence_ms: 0,
            last_presence: None,
            closed: false,
            builtin_available: false,
            builtin_percent: None,
            calibrating: false,
            below_threshold: BTreeSet::new(),
        })
    }

    pub fn settings(&self) -> Result<Settings, String> {
        Ok(self.settings.clone())
    }

    pub fn submit_json(&mut self, bytes: &[u8]) -> Result<(), String> {
        if bytes.len() > 64 * 1024 {
            return Err("input exceeds 64 KiB".into());
        }
        let value = parse_json_unique(bytes)?;
        let kind = value
            .get("type")
            .and_then(Value::as_str)
            .ok_or("command type is required")?;
        reject_unknown_fields(
            &value,
            match kind {
                "monitor_discovered" => &["type", "monitor"][..],
                "monitor_disconnected" => &["type", "monitor_id"],
                "monitor_observed" => &["type", "monitor_id", "generation", "brightness", "volume"],
                "monitor_display_state" => &["type", "monitor_id", "generation", "state"],
                "presence_observation" => &[
                    "type",
                    "status",
                    "captured_at_ms",
                    "camera_generation",
                    "camera_status",
                ],
                "camera_status" => &["type", "status", "generation"],
                "power" => &["type", "on_battery"],
                "screensaver" => &["type", "state"],
                "set_brightness" | "set_volume" => &[
                    "type",
                    "monitor_id",
                    "value",
                    "revision",
                    "connection_generation",
                ],
                "update_settings" => &["type", "settings", "revision"],
                "hardware_result" => &["type", "action_id", "generation", "ok", "value", "error"],
                "builtin_display" => &["type", "available", "brightness", "calibrating"],
                "shutdown" => &["type"],
                _ => return Err(format!("unknown command type: {kind}")),
            },
        )?;
        if self.closed && kind != "hardware_result" {
            return Err("core is shut down".into());
        }
        match kind {
            "monitor_discovered" => self.discover(&value)?,
            "monitor_disconnected" => self.disconnect(&value)?,
            "monitor_observed" => self.observe_monitor(&value)?,
            "monitor_display_state" => self.display_state(&value)?,
            "presence_observation" => self.presence(&value)?,
            "power" => {
                if !value
                    .get("on_battery")
                    .map(|v| v.is_boolean() || v.is_null())
                    .unwrap_or(false)
                {
                    return Err("on_battery must be boolean or null".into());
                }
                let was_on_battery = self.on_battery == Some(true);
                self.on_battery = value.get("on_battery").and_then(Value::as_bool);
                self.camera_generation += 1;
                self.last_presence = None;
                if self.settings.suspend_presence_on_battery && self.on_battery != Some(false) {
                    for m in self.monitors.values_mut() {
                        m.presence_dimmed = false;
                        m.misses = 0;
                    }
                }
                if was_on_battery
                    && self.on_battery == Some(false)
                    && self.settings.suspend_presence_on_battery
                {
                    self.begin_camera_resume();
                }
                self.reconcile_camera();
                self.reconcile_all()?;
            }
            "camera_status" => {
                if let Some(gen) = value.get("generation").and_then(Value::as_u64) {
                    if gen != self.camera_generation {
                        return Ok(());
                    }
                }
                let status = string(&value, "status")?.to_owned();
                if ![
                    "unknown",
                    "ready",
                    "unavailable",
                    "busy",
                    "permission",
                    "error",
                ]
                .contains(&status.as_str())
                {
                    return Err("invalid camera status".into());
                }
                self.camera_status = status;
                self.reconcile_camera();
                if ["unavailable", "busy", "permission", "error"]
                    .contains(&self.camera_status.as_str())
                {
                    for monitor in self.monitors.values_mut() {
                        monitor.presence_dimmed = false;
                        monitor.misses = 0;
                    }
                    self.reconcile_all()?;
                }
            }
            "screensaver" => {
                let state = value
                    .get("state")
                    .and_then(Value::as_str)
                    .ok_or("screensaver state required")?
                    .to_owned();
                if !["unknown", "inactive", "active", "unavailable"].contains(&state.as_str()) {
                    return Err("invalid screensaver state".into());
                }
                self.saver_state = state;
                self.reconcile_all()?;
            }
            "set_brightness" | "set_volume" => self.manual_set(kind, &value)?,
            "update_settings" => self.update_settings(&value)?,
            "hardware_result" => self.hardware_result(&value)?,
            "builtin_display" => self.builtin_display(&value)?,
            "shutdown" => self.shutdown_policy()?,
            _ => return Err(format!("unknown command type: {kind}")),
        }
        self.sync_brightness_gate();
        self.revision = self.revision.saturating_add(1);
        Ok(())
    }

    fn discover(&mut self, v: &Value) -> Result<(), String> {
        let m = v.get("monitor").ok_or("monitor is required")?;
        reject_unknown_fields(
            m,
            &[
                "id",
                "name",
                "identity_status",
                "connection_path",
                "brightness",
                "volume",
            ],
        )?;
        let id = string(m, "id")?;
        let name = string(m, "name")?;
        let identity = string(m, "identity_status")?;
        let path = string(m, "connection_path")?;
        if !["stable", "ambiguous"].contains(&identity) {
            return Err("invalid identity_status".into());
        }
        if !self.monitors.contains_key(id) && self.monitors.len() >= MAX_MONITORS {
            return Err("monitor limit reached".into());
        }
        let brightness = feature(m.get("brightness"), identity)?;
        let volume = feature(m.get("volume"), identity)?;
        let baseline = brightness.raw;
        let generation = self
            .connection_generations
            .get(id)
            .copied()
            .unwrap_or(0)
            .saturating_add(1);
        self.connection_generations
            .insert(id.to_owned(), generation);
        self.actions.retain(|a| a.monitor_id != id);
        self.camera_generation = self.camera_generation.saturating_add(1);
        self.last_presence = None;
        self.monitors.insert(
            id.into(),
            Monitor {
                id: id.into(),
                name: name.into(),
                identity: identity.into(),
                path: path.into(),
                generation,
                display_state: "unknown".into(),
                brightness,
                volume,
                baseline,
                follow_raw: None,
                presence_dimmed: false,
                saver_dimmed: false,
                misses: 0,
                restore_failures: 0,
                manual_override: false,
                wake_ownership_check: false,
            },
        );
        // Follow runs after discovery sets the baseline, so the response carries the write.
        self.recompute_follow(Some(id));
        if self
            .monitors
            .get(id)
            .is_some_and(|m| m.follow_raw.is_some())
        {
            self.reconcile_all()?;
        }
        Ok(())
    }

    fn disconnect(&mut self, v: &Value) -> Result<(), String> {
        let id = string(v, "monitor_id")?.to_owned();
        let generation = self.connection_generations.entry(id.clone()).or_default();
        *generation = generation.saturating_add(1);
        self.monitors.remove(&id);
        self.actions.retain(|a| a.monitor_id != id);
        self.reconcile_camera();
        Ok(())
    }

    fn observe_monitor(&mut self, v: &Value) -> Result<(), String> {
        let id = string(v, "monitor_id")?.to_owned();
        let generation = number_u64(v, "generation")?;
        let monitor = self.monitors.get_mut(&id).ok_or("unknown monitor")?;
        if generation != monitor.generation {
            return Ok(());
        }
        for key in ["brightness", "volume"] {
            if let Some(obj) = v.get(key) {
                if let Some(val) = obj
                    .get("raw")
                    .and_then(Value::as_f64)
                    .or_else(|| obj.get("value").and_then(Value::as_f64))
                {
                    let feature = if key == "brightness" {
                        &mut monitor.brightness
                    } else {
                        &mut monitor.volume
                    };
                    if key == "brightness"
                        && feature.owned
                        && feature
                            .raw
                            .map(|last| (last - val).abs() > 0.0001)
                            .unwrap_or(false)
                    {
                        monitor.baseline = Some(val);
                        if monitor.presence_dimmed || monitor.saver_dimmed {
                            monitor.manual_override = true;
                        }
                        feature.owned = false;
                    }
                    feature.observed = Some(val);
                    feature.raw = Some(val);
                    feature.status = "ready".into();
                }
            }
        }
        Ok(())
    }

    fn display_state(&mut self, v: &Value) -> Result<(), String> {
        let id = string(v, "monitor_id")?.to_owned();
        let generation = number_u64(v, "generation")?;
        let state = string(v, "state")?;
        if !["unknown", "awake", "asleep"].contains(&state) {
            return Err("invalid display state".into());
        }
        let protected = self.is_protected(&id);
        let monitor = self.monitors.get_mut(&id).ok_or("unknown monitor")?;
        if generation != monitor.generation {
            return Ok(());
        }
        if monitor.display_state == state {
            return Ok(());
        }
        let was_asleep = monitor.display_state == "asleep";
        monitor.display_state = state.into();
        monitor.misses = 0;
        monitor.restore_failures = 0;
        monitor.presence_dimmed = false;
        monitor.brightness.ownership_checked_target = None;
        monitor.wake_ownership_check = if state == "asleep" {
            false
        } else {
            was_asleep || monitor.wake_ownership_check
        };
        let resumed_display =
            was_asleep && state == "awake" && Self::monitor_is_camera_eligible(monitor, protected);
        self.actions.retain(|action| action.monitor_id != id);
        self.camera_generation = self.camera_generation.saturating_add(1);
        self.last_presence = None;
        if resumed_display {
            self.begin_camera_resume();
        }
        self.reconcile_camera();
        self.recompute_follow(Some(id.as_str()));
        self.reconcile_all()
    }

    fn manual_set(&mut self, kind: &str, v: &Value) -> Result<(), String> {
        let id = string(v, "monitor_id")?.to_owned();
        let value = number(v, "value")?;
        if !(0.0..=100.0).contains(&value) {
            return Err("control value must be in 0..=100".into());
        }
        reject_unknown_fields(
            v,
            &[
                "type",
                "monitor_id",
                "value",
                "revision",
                "connection_generation",
            ],
        )?;
        let expected_generation = if v.get("connection_generation").is_some() {
            Some(number_u64(v, "connection_generation")?)
        } else {
            None
        };
        let action_generation = self.revision.saturating_add(1);
        let m = self.monitors.get_mut(&id).ok_or("unknown monitor")?;
        if expected_generation.is_some_and(|generation| generation != m.generation) {
            return Err("stale connection_generation".into());
        }
        if m.identity != "stable" {
            return Ok(());
        }
        let feature = if kind == "set_brightness" {
            &mut m.brightness
        } else {
            &mut m.volume
        };
        if !feature.supported {
            feature.status = "unsupported".into();
            return Ok(());
        }
        if m.display_state == "asleep" {
            feature.status = "suspended_display_sleep".into();
            return Ok(());
        }
        let raw = map_normalized(value, feature.min, feature.max);
        // Reserve the action before changing desired values or brightness ownership. A full
        // bounded queue must leave keyboard intent and protection state untouched.
        enqueue(
            &mut self.actions,
            &mut self.next_id,
            &id,
            action_generation,
            kind,
            Some(raw),
            None,
        )?;
        feature.desired = Some(raw);
        feature.status = "pending".into();
        if kind == "set_brightness" {
            m.baseline = Some(raw);
            m.manual_override = true;
            m.restore_failures = 0;
        }
        Ok(())
    }

    fn update_settings(&mut self, v: &Value) -> Result<(), String> {
        let mut next = self.settings.clone();
        let obj = v
            .get("settings")
            .and_then(Value::as_object)
            .ok_or("settings object required")?;
        for (key, val) in obj {
            match key.as_str() {
                "schema_version" => {
                    next.schema_version =
                        val.as_u64().ok_or("schema_version must be integer")? as u32
                }
                "locale" => next.locale = val.as_str().ok_or("locale must be string")?.to_owned(),
                "presence_sampling_interval_s" => {
                    next.presence_sampling_interval_s =
                        val.as_f64().ok_or("interval must be number")?
                }
                "return_sampling_interval_s" => {
                    next.return_sampling_interval_s =
                        val.as_f64().ok_or("interval must be number")?
                }
                "suspend_presence_on_battery" => {
                    next.suspend_presence_on_battery =
                        val.as_bool().ok_or("setting must be boolean")?
                }
                "presence_protection_enabled" => {
                    next.presence_protection_enabled =
                        val.as_bool().ok_or("setting must be boolean")?
                }
                "presence_min_brightness_percent" => {
                    next.presence_min_brightness_percent =
                        val.as_f64().ok_or("threshold must be number")?
                }
                "always_on_camera" => {
                    next.always_on_camera = val.as_bool().ok_or("setting must be boolean")?
                }
                "screensaver_protection_enabled" => {
                    next.screensaver_protection_enabled =
                        val.as_bool().ok_or("setting must be boolean")?
                }
                "launch_at_login" => {
                    next.launch_at_login = val.as_bool().ok_or("setting must be boolean")?
                }
                "hardware_keys_enabled" => {
                    next.hardware_keys_enabled = val.as_bool().ok_or("setting must be boolean")?
                }
                "brightness_key_step" => {
                    let n = val.as_u64().ok_or("key step must be integer")?;
                    next.brightness_key_step =
                        u8::try_from(n).map_err(|_| "key step out of range")?;
                }
                "volume_key_step" => {
                    let n = val.as_u64().ok_or("key step must be integer")?;
                    next.volume_key_step = u8::try_from(n).map_err(|_| "key step out of range")?;
                }
                "inference_device" => {
                    next.inference_device = val
                        .as_str()
                        .ok_or("inference_device must be string")?
                        .to_owned()
                }
                "selected_camera_id" => {
                    if val.is_null() {
                        next.selected_camera_id = None;
                        next.camera_history.clear();
                    } else {
                        let id = val
                            .as_str()
                            .ok_or("selected_camera_id must be string or null")?;
                        // Most recent first, deduplicated and bounded. The history is derived
                        // from selections only, so clients cannot set it directly.
                        next.camera_history.retain(|known| known.as_str() != id);
                        next.camera_history.insert(0, id.to_owned());
                        next.camera_history.truncate(settings::MAX_CAMERA_HISTORY);
                        next.selected_camera_id = Some(id.to_owned());
                    }
                }
                "camera_history" => return Err("camera_history is read-only".into()),
                "screensaver_targets" => {
                    next.screensaver_targets =
                        serde_json::from_value(val.clone()).map_err(|e| e.to_string())?
                }
                "ambient_follow_enabled" => {
                    next.ambient_follow_enabled = val.as_bool().ok_or("setting must be boolean")?
                }
                "ambient_curves" => {
                    next.ambient_curves =
                        serde_json::from_value(val.clone()).map_err(|e| e.to_string())?
                }
                "protection_disabled" => {
                    next.protection_disabled =
                        serde_json::from_value(val.clone()).map_err(|e| e.to_string())?
                }
                _ => return Err(format!("unknown setting: {key}")),
            }
        }
        next.validate()?;
        if let Some(path) = &self.config_path {
            settings::store(path, &next)?;
        }
        let camera_changed = next.selected_camera_id != self.settings.selected_camera_id;
        let presence_reenabled =
            !self.settings.presence_protection_enabled && next.presence_protection_enabled;
        // A save that changes nothing (for example reselecting the current front) keeps the
        // camera generation, so in-flight samples and the camera session are not restarted.
        let settings_changed = next != self.settings;
        // Per-monitor protection: a monitor that becomes unprotected drops its dims (reconcile_all
        // restores it), and any change of its protection restarts the presence miss count.
        for m in self.monitors.values_mut() {
            let was_protected = !self.settings.protection_disabled.contains(&m.id);
            let now_protected = !next.protection_disabled.contains(&m.id);
            if was_protected != now_protected {
                m.misses = 0;
            }
            if was_protected && !now_protected {
                m.presence_dimmed = false;
                m.saver_dimmed = false;
            }
        }
        self.settings = next;
        if settings_changed {
            self.camera_generation += 1;
            self.last_presence = None;
        }
        if camera_changed {
            self.camera_status = "unknown".into();
            self.last_presence_ms = 0;
            self.last_presence = None;
            self.low_light = false;
            self.usable_light_samples = 0;
        }
        if presence_reenabled {
            self.begin_camera_resume();
        }
        self.reconcile_camera();
        self.recompute_follow(None);
        self.reconcile_all()
    }

    fn presence(&mut self, v: &Value) -> Result<(), String> {
        let status = string(v, "status")?;
        let ts = number_u64(v, "captured_at_ms")?;
        let gen = number_u64(v, "camera_generation")?;
        if ![
            "present",
            "absent",
            "insufficient_light",
            "unavailable",
            "usable_light",
        ]
        .contains(&status)
        {
            return Err("invalid presence status".into());
        }
        if status == "unavailable" {
            let camera_status = v
                .get("camera_status")
                .and_then(Value::as_str)
                .unwrap_or("unavailable");
            if !["unavailable", "busy", "permission", "error"].contains(&camera_status) {
                return Err("invalid unavailable camera status".into());
            }
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_millis() as u64;
        if gen != self.camera_generation
            || matches!(self.camera_mode(), "disabled" | "suspended")
            || ts <= self.last_presence_ms
            || ts > now
            || now.saturating_sub(ts) > 60_000
        {
            return Ok(());
        }
        self.last_presence_ms = ts;
        self.resume_pending = false;
        if status != "unavailable" {
            self.camera_status = "ready".into();
        }
        match status {
            "present" => {
                self.last_presence = Some("present");
                self.low_light = false;
                self.usable_light_samples = 0;
                for m in self.monitors.values_mut() {
                    m.misses = 0;
                    m.presence_dimmed = false;
                    if !m.saver_dimmed {
                        m.manual_override = false;
                    }
                }
                self.reconcile_all()?;
            }
            "absent" => {
                if self.camera_mode() != "face" {
                    return Ok(());
                }
                self.last_presence = Some("absent");
                let protection_disabled = &self.settings.protection_disabled;
                for m in self.monitors.values_mut() {
                    if m.display_state == "asleep" {
                        continue;
                    }
                    m.misses = m.misses.saturating_add(1);
                    if m.misses >= 2 && !protection_disabled.contains(&m.id) {
                        m.presence_dimmed = true;
                    }
                }
                self.reconcile_all()?;
            }
            "insufficient_light" => {
                self.last_presence = None;
                self.low_light = true;
                self.usable_light_samples = 0;
                for m in self.monitors.values_mut() {
                    m.misses = 0;
                    m.presence_dimmed = false;
                }
                self.reconcile_camera();
                self.reconcile_all()?;
            }
            "unavailable" => {
                self.last_presence = None;
                self.camera_status = v
                    .get("camera_status")
                    .and_then(Value::as_str)
                    .unwrap_or("unavailable")
                    .into();
                for m in self.monitors.values_mut() {
                    m.misses = 0;
                    m.presence_dimmed = false;
                }
                self.reconcile_camera();
                self.reconcile_all()?;
            }
            "usable_light" => {
                self.usable_light_samples = self.usable_light_samples.saturating_add(1);
                if self.usable_light_samples >= 2 {
                    self.low_light = false;
                    self.usable_light_samples = 0;
                }
                self.reconcile_camera();
            }
            _ => return Err("invalid presence status".into()),
        }
        Ok(())
    }

    fn reconcile_camera(&mut self) {
        if !self.settings.presence_protection_enabled
            || (self.settings.suspend_presence_on_battery && self.on_battery != Some(false))
        {
            self.camera_status = "unknown".into();
            self.last_presence = None;
            for monitor in self.monitors.values_mut() {
                monitor.presence_dimmed = false;
                monitor.misses = 0;
            }
        } else if self.all_eligible_displays_asleep() {
            if !["unavailable", "busy", "permission", "error"]
                .contains(&self.camera_status.as_str())
            {
                self.camera_status = "unknown".into();
            }
            self.last_presence = None;
            for monitor in self.monitors.values_mut() {
                monitor.presence_dimmed = false;
                monitor.misses = 0;
            }
        }
        // A busy or permission status suspends the camera without a generation change.
        if matches!(self.camera_mode(), "disabled" | "suspended") {
            self.last_presence = None;
        }
    }
    // Clears state that a suspended period leaves behind so the first sample after a resume
    // starts clean. Permission and error need user action and are kept.
    fn begin_camera_resume(&mut self) {
        if !self.settings.presence_protection_enabled {
            return;
        }
        self.low_light = false;
        if ["busy", "unavailable"].contains(&self.camera_status.as_str()) {
            self.camera_status = "unknown".into();
            self.last_presence = None;
        }
        self.resume_pending = true;
    }

    fn camera_mode(&self) -> &'static str {
        if self.closed || !self.settings.presence_protection_enabled {
            "disabled"
        } else if (self.settings.suspend_presence_on_battery && self.on_battery != Some(false))
            || !self
                .monitors
                .values()
                .any(|m| self.monitor_is_camera_active(m))
            || (self.camera_status != "unknown"
                && ["unavailable", "busy", "permission", "error"]
                    .contains(&self.camera_status.as_str()))
        {
            "suspended"
        } else if self.low_light {
            "light_only"
        } else {
            "face"
        }
    }

    // Absence from protection_disabled means protection is on for that monitor.
    fn is_protected(&self, id: &str) -> bool {
        !self.settings.protection_disabled.contains(id)
    }

    fn monitor_is_camera_eligible(m: &Monitor, protected: bool) -> bool {
        protected && m.identity == "stable" && m.brightness.supported && m.baseline.is_some()
    }

    // Brightness used for the threshold. The baseline is the user's intended level while the app
    // owns the value or has protection reasons, so its own dims do not stop the camera.
    fn monitor_level_percent(m: &Monitor) -> Option<f64> {
        let raw = if m.brightness.owned || !m.brightness.reasons.is_empty() {
            m.baseline
        } else {
            m.brightness.observed
        }?;
        Some(normalized(raw, m.brightness.min, m.brightness.max))
    }

    // A dimmed monitor counts as above: it was dimmed only because it was above, and its return
    // check must keep running. An unknown level is not below, so protection stays on.
    fn monitor_below_threshold(&self, m: &Monitor) -> bool {
        Self::monitor_is_camera_eligible(m, self.is_protected(&m.id))
            && !m.presence_dimmed
            && Self::monitor_level_percent(m)
                .is_some_and(|level| level <= self.settings.presence_min_brightness_percent)
    }

    fn monitor_is_camera_active(&self, m: &Monitor) -> bool {
        Self::monitor_is_camera_eligible(m, self.is_protected(&m.id))
            && m.display_state != "asleep"
            && !self.monitor_below_threshold(m)
    }

    // Runs after each accepted command. Entering the below-threshold set resets that monitor's
    // miss count; a still-connected monitor leaving it starts a clean camera resume.
    fn sync_brightness_gate(&mut self) {
        let below: BTreeSet<String> = self
            .monitors
            .values()
            .filter(|m| self.monitor_below_threshold(m))
            .map(|m| m.id.clone())
            .collect();
        let entered: Vec<String> = below.difference(&self.below_threshold).cloned().collect();
        let left: Vec<String> = self
            .below_threshold
            .difference(&below)
            .filter(|id| self.monitors.contains_key(id.as_str()))
            .cloned()
            .collect();
        self.below_threshold = below;
        if entered.is_empty() && left.is_empty() {
            return;
        }
        self.camera_generation = self.camera_generation.saturating_add(1);
        self.last_presence = None;
        for id in &entered {
            if let Some(monitor) = self.monitors.get_mut(id) {
                monitor.misses = 0;
            }
        }
        if !left.is_empty() {
            self.begin_camera_resume();
        }
        self.reconcile_camera();
    }

    fn all_eligible_displays_asleep(&self) -> bool {
        let mut eligible = self
            .monitors
            .values()
            .filter(|monitor| {
                Self::monitor_is_camera_eligible(monitor, self.is_protected(&monitor.id))
            })
            .peekable();
        eligible.peek().is_some() && eligible.all(|monitor| monitor.display_state == "asleep")
    }

    fn camera_display_status(&self, mode: &str) -> &'static str {
        match mode {
            "disabled" => "disabled",
            "suspended" => match self.camera_status.as_str() {
                "unavailable" => "unavailable",
                "busy" => "busy",
                "permission" => "permission",
                "error" => "error",
                _ if self.settings.suspend_presence_on_battery && self.on_battery == Some(true) => {
                    "suspended_battery"
                }
                _ if self.settings.suspend_presence_on_battery && self.on_battery.is_none() => {
                    "suspended_power_unknown"
                }
                _ if self.all_eligible_displays_asleep() => "suspended_display_sleep",
                _ if self
                    .monitors
                    .values()
                    .any(|m| Self::monitor_is_camera_eligible(m, self.is_protected(&m.id))) =>
                {
                    "suspended_low_brightness"
                }
                _ => "suspended_no_monitor",
            },
            "light_only" => "insufficient_light",
            "face" if self.camera_status == "unknown" => "checking",
            "face" => "ready",
            _ => "unknown",
        }
    }

    fn reconcile_all(&mut self) -> Result<(), String> {
        let saver_active =
            self.settings.screensaver_protection_enabled && self.saver_state == "active";
        let targets = self.settings.screensaver_targets.clone();
        let protection_disabled = &self.settings.protection_disabled;
        let action_generation = self.revision.saturating_add(1);
        for m in self.monitors.values_mut() {
            if m.identity != "stable" || !m.brightness.supported {
                continue;
            }
            // Unprotected monitors get no reasons, so the no-reason branch below restores them.
            let protected = !protection_disabled.contains(&m.id);
            let presence_dimmed = m.presence_dimmed && protected;
            m.saver_dimmed = saver_active && protected;
            let mut reasons = Vec::new();
            if presence_dimmed {
                reasons.push("presence".to_owned());
            }
            if m.saver_dimmed {
                reasons.push("screensaver".to_owned());
            }
            m.brightness.reasons = reasons;
            if m.display_state == "asleep" {
                continue;
            }
            if presence_dimmed && m.baseline.is_none() {
                m.brightness.status = "read_required".into();
                continue;
            }
            if m.brightness.reasons.is_empty() {
                m.manual_override = false;
            }
            if m.manual_override {
                continue;
            }
            // Follow applies only when no protection reason holds. A full queue defers it to the
            // next reconcile instead of failing the command.
            let following = m.follow_raw.filter(|_| m.brightness.reasons.is_empty());
            if following.is_some() && self.actions.len() >= MAX_ACTIONS {
                continue;
            }
            let target = if !m.brightness.reasons.is_empty() {
                let base = m.baseline.unwrap_or(m.brightness.observed.unwrap_or(0.0));
                let mut desired = base;
                if presence_dimmed {
                    desired = desired.min(m.brightness.min);
                }
                if m.saver_dimmed {
                    desired = desired.min(map_normalized(
                        *targets.get(&m.id).unwrap_or(&0.0),
                        m.brightness.min,
                        m.brightness.max,
                    ));
                }
                Some(desired)
            } else if following.is_some() {
                following
            } else if m.brightness.owned {
                m.baseline
            } else {
                None
            };
            if let Some(target) = target {
                if m.wake_ownership_check
                    && m.brightness
                        .observed
                        .map(|observed| (observed - target).abs() > 0.0001)
                        .unwrap_or(true)
                {
                    enqueue_brightness_read(
                        &mut self.actions,
                        &mut self.next_id,
                        &m.id,
                        action_generation,
                        target,
                        &mut m.brightness.status,
                    )?;
                    continue;
                }
                // Follow stops once the last desired value matches, so a monitor that reports a
                // slightly different level is not rewritten in a loop.
                if following.is_some()
                    && m.brightness
                        .desired
                        .is_some_and(|desired| (desired - target).abs() < 0.0001)
                {
                    continue;
                }
                if m.brightness.owned
                    && m.brightness
                        .raw
                        .map(|raw| (raw - target).abs() > 0.0001)
                        .unwrap_or(true)
                {
                    if m.brightness.ownership_checked_target == Some(target) {
                        m.brightness.ownership_checked_target = None;
                        m.brightness.desired = Some(target);
                        m.brightness.status = "pending".into();
                        enqueue(
                            &mut self.actions,
                            &mut self.next_id,
                            &m.id,
                            action_generation,
                            "set_brightness",
                            Some(target),
                            None,
                        )?;
                    } else {
                        enqueue_brightness_read(
                            &mut self.actions,
                            &mut self.next_id,
                            &m.id,
                            action_generation,
                            target,
                            &mut m.brightness.status,
                        )?;
                    }
                    continue;
                }
                let target_queued = self.actions.iter().any(|a| {
                    a.monitor_id == m.id
                        && a.kind == "set_brightness"
                        && a.value
                            .map(|v| (v - target).abs() < 0.0001)
                            .unwrap_or(false)
                });
                if m.brightness
                    .observed
                    .map(|o| (o - target).abs() > 0.0001)
                    .unwrap_or(true)
                    && m.identity == "stable"
                    && !target_queued
                {
                    m.brightness.desired = Some(target);
                    m.brightness.status = "pending".into();
                    enqueue(
                        &mut self.actions,
                        &mut self.next_id,
                        &m.id,
                        action_generation,
                        "set_brightness",
                        Some(target),
                        None,
                    )?;
                }
            }
        }
        Ok(())
    }

    fn hardware_result(&mut self, v: &Value) -> Result<(), String> {
        let id = number_u64(v, "action_id")?;
        let generation = number_u64(v, "generation")?;
        let ok = v
            .get("ok")
            .and_then(Value::as_bool)
            .ok_or("ok must be a boolean")?;
        let pos = self
            .actions
            .iter()
            .position(|a| a.id == id && a.generation == generation);
        let Some(pos) = pos else { return Ok(()) };
        let action = self
            .actions
            .get(pos)
            .cloned()
            .ok_or("hardware action disappeared")?;
        if action.kind == "read_brightness" && ok && !v.get("value").is_some_and(Value::is_number) {
            return Err("successful brightness readback requires value".into());
        }
        if let Some(raw) = v.get("value") {
            let raw = raw
                .as_f64()
                .filter(|n| n.is_finite())
                .ok_or("hardware result value must be finite")?;
            if let Some(m) = self.monitors.get(&action.monitor_id) {
                let f = if action.kind == "set_volume" {
                    &m.volume
                } else {
                    &m.brightness
                };
                if raw < f.min || raw > f.max {
                    return Err("hardware result is outside feature range".into());
                }
            }
        }
        let action = self.actions.remove(pos).unwrap();
        let closed = self.closed;
        let Some(m) = self.monitors.get_mut(&action.monitor_id) else {
            return Ok(());
        };
        if action.kind == "read_brightness" {
            let feature = &mut m.brightness;
            m.wake_ownership_check = false;
            if ok {
                let value = v
                    .get("value")
                    .and_then(Value::as_f64)
                    .ok_or("brightness readback requires a raw value")?;
                feature.observed = Some(value);
                feature.status = "ready".into();
                if feature
                    .raw
                    .map(|last| (last - value).abs() <= 0.0001)
                    .unwrap_or(false)
                {
                    feature.ownership_checked_target = action.target;
                } else {
                    feature.raw = Some(value);
                    feature.owned = false;
                    feature.ownership_checked_target = None;
                    // Follow is authoritative: a differing readback is overwritten, not adopted.
                    if m.follow_raw.is_some() {
                        feature.desired = None;
                    } else {
                        m.baseline = Some(value);
                        m.manual_override = !feature.reasons.is_empty();
                    }
                }
            } else {
                feature.status = v
                    .get("error")
                    .and_then(Value::as_str)
                    .unwrap_or("read_error")
                    .into();
                // No retries once shutdown has started; quitting must not wait on them.
                if feature.owned
                    && feature.reasons.is_empty()
                    && !closed
                    && m.restore_failures < MAX_RESTORE_FAILURES
                {
                    // Keep ownership: reconcile_all re-reads and the baseline stays reachable.
                    m.restore_failures += 1;
                    feature.ownership_checked_target = None;
                } else {
                    feature.owned = false;
                    feature.ownership_checked_target = None;
                    m.manual_override = !feature.reasons.is_empty();
                }
            }
            self.reconcile_all()?;
            return Ok(());
        }
        let feature = if action.kind == "set_volume" {
            &mut m.volume
        } else {
            &mut m.brightness
        };
        if ok {
            if action.kind == "set_brightness" {
                m.restore_failures = 0;
            }
            let value = v.get("value").and_then(Value::as_f64).or(action.value);
            feature.observed = value;
            feature.raw = value;
            feature.status = "ready".into();
            feature.owned = action.kind == "set_brightness";
            if feature.owned
                && m.manual_override
                && feature
                    .desired
                    .zip(value)
                    .map(|(wanted, actual)| (wanted - actual).abs() < 0.0001)
                    .unwrap_or(false)
            {
                if let Some(value) = value {
                    m.baseline = Some(value);
                }
            }
        } else {
            feature.status = v
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("error")
                .into();
            // Only a failed restore write keeps ownership; reconcile_all re-reads first.
            let restore_write = action.kind == "set_brightness"
                && feature.owned
                && feature.reasons.is_empty()
                && !m.manual_override
                && action
                    .value
                    .zip(m.baseline)
                    .is_some_and(|(wanted, base)| (wanted - base).abs() < 0.0001);
            if restore_write && !closed && m.restore_failures < MAX_RESTORE_FAILURES {
                m.restore_failures += 1;
            } else {
                feature.owned = false;
            }
        }
        self.reconcile_all()?;
        Ok(())
    }

    fn shutdown_policy(&mut self) -> Result<(), String> {
        let action_generation = self.revision.saturating_add(1);
        for m in self.monitors.values_mut() {
            m.presence_dimmed = false;
            m.saver_dimmed = false;
            if m.brightness.owned && m.display_state != "asleep" {
                if let Some(v) = m.baseline {
                    enqueue(
                        &mut self.actions,
                        &mut self.next_id,
                        &m.id,
                        action_generation,
                        "read_brightness",
                        None,
                        Some(v),
                    )?;
                }
            }
        }
        self.closed = true;
        Ok(())
    }

    // Shell report of the built-in display. Every field is validated before any state changes.
    fn builtin_display(&mut self, v: &Value) -> Result<(), String> {
        let available = v
            .get("available")
            .and_then(Value::as_bool)
            .ok_or("available must be boolean")?;
        let calibrating = v
            .get("calibrating")
            .and_then(Value::as_bool)
            .ok_or("calibrating must be boolean")?;
        let percent = match v.get("brightness") {
            None => return Err("brightness is required".into()),
            Some(Value::Null) => None,
            Some(level) => match level.as_f64() {
                Some(n) if n.is_finite() && (0.0..=100.0).contains(&n) => Some(n),
                _ => return Err("brightness must be a number in 0..=100 or null".into()),
            },
        };
        self.builtin_available = available;
        self.builtin_percent = percent;
        self.calibrating = calibrating;
        self.recompute_follow(None);
        self.reconcile_all()
    }

    // Recomputes follow targets for one monitor or all of them. A target also becomes the
    // baseline so protection restores the mapped value. manual_override and restore_failures
    // stay untouched because follow is not a manual change.
    fn recompute_follow(&mut self, monitor_id: Option<&str>) {
        let targets: Vec<(String, Option<f64>)> = self
            .monitors
            .values()
            .filter(|m| monitor_id.is_none() || monitor_id == Some(m.id.as_str()))
            .map(|m| (m.id.clone(), self.follow_target(m)))
            .collect();
        for (id, raw) in targets {
            if let Some(m) = self.monitors.get_mut(&id) {
                m.follow_raw = raw;
                if let Some(raw) = raw {
                    m.baseline = Some(raw);
                }
            }
        }
    }

    // Mapped raw target for one monitor, or None while follow is not applying to it.
    fn follow_target(&self, m: &Monitor) -> Option<f64> {
        if !self.settings.ambient_follow_enabled
            || !self.builtin_available
            || self.calibrating
            || self.follow_eligibility(m) != "eligible"
        {
            return None;
        }
        let curve = self.settings.ambient_curves.get(&m.id)?;
        let percent = curve_percent(curve, self.builtin_percent?)?;
        let raw = map_normalized(percent, m.brightness.min, m.brightness.max).round();
        Some(raw.clamp(m.brightness.min, m.brightness.max))
    }

    // Ambient follow eligibility, regardless of whether follow is switched on. Later follow
    // targets match on "eligible".
    fn follow_eligibility(&self, m: &Monitor) -> &'static str {
        if m.identity != "stable" {
            "unstable"
        } else if !m.brightness.supported {
            "unsupported"
        } else if self
            .settings
            .ambient_curves
            .get(&m.id)
            .is_some_and(AmbientCurve::is_complete)
        {
            "eligible"
        } else {
            "uncalibrated"
        }
    }

    pub fn snapshot_value(&self) -> Result<Value, String> {
        let monitors:Vec<Value>=self.monitors.values().map(|m|json!({"id":m.id,"name":m.name,"identity_status":m.identity,"connection_path":m.path,"generation":m.generation,"display_state":m.display_state,
            "presence_dimmed":m.presence_dimmed,"brightness":feature_json(&m.brightness),"volume":feature_json(&m.volume),
            "follow":self.follow_eligibility(m),"protected":!self.settings.protection_disabled.contains(&m.id)})).collect();
        let mut scheduled_monitors = BTreeSet::new();
        let actions: Vec<Value> = self.actions.iter()
            .filter(|a| scheduled_monitors.insert(a.monitor_id.as_str()))
            .take(8)
            .map(|a| json!({"id":a.id,"monitor_id":a.monitor_id,"generation":a.generation,"kind":a.kind,"value":a.value,"target":a.target}))
            .collect();
        let camera_mode = self.camera_mode();
        let cadence = if camera_mode == "face" && self.resume_pending {
            RESUME_FIRST_SAMPLE_MS
        } else if camera_mode == "face" && self.monitors.values().any(|m| m.presence_dimmed) {
            self.settings.return_sampling_interval_s * 1000.0
        } else if camera_mode == "face" {
            self.settings.presence_sampling_interval_s * 1000.0
        } else if camera_mode == "light_only" {
            10000.0
        } else if self.monitors.values().any(|m| m.presence_dimmed) {
            self.settings.return_sampling_interval_s * 1000.0
        } else {
            self.settings.presence_sampling_interval_s * 1000.0
        };
        let capture_mode = if camera_mode == "disabled" || camera_mode == "suspended" {
            "stopped"
        } else if camera_mode == "face" && self.monitors.values().any(|m| m.presence_dimmed) {
            "continuous_return"
        } else if self.settings.always_on_camera {
            "continuous_monitoring"
        } else {
            "single_frame"
        };
        let display_status = self.camera_display_status(camera_mode);
        let presence = match camera_mode {
            "light_only" => json!("insufficient_light"),
            "face" => json!(self.last_presence),
            _ => Value::Null,
        };
        let value = json!({"revision":self.revision,"settings":self.settings,"monitors":monitors,"pending_actions":actions,
            "ambient":{"enabled":self.settings.ambient_follow_enabled,"available":self.builtin_available,"active":self.settings.ambient_follow_enabled&&self.builtin_available,"calibrating":self.calibrating,"builtin_percent":self.builtin_percent},
            "camera":{"mode":camera_mode,"capture_mode":capture_mode,"status":self.camera_status,"display_status":display_status,"next_sample_in_ms":if camera_mode=="disabled"||camera_mode=="suspended"{Value::Null}else{json!(cadence)},"generation":self.camera_generation,"presence":presence},
            "power":{"on_battery":self.on_battery},"screensaver":{"state":self.saver_state},"status":"ready"});
        let bytes = serde_json::to_vec(&value).map_err(|e| e.to_string())?;
        if bytes.len() > 8 * 1024 * 1024 {
            return Err("snapshot exceeds 8 MiB".into());
        }
        Ok(value)
    }
}

fn reject_unknown_fields(value: &Value, allowed: &[&str]) -> Result<(), String> {
    let object = value.as_object().ok_or("JSON envelope must be an object")?;
    if let Some(key) = object.keys().find(|key| !allowed.contains(&key.as_str())) {
        return Err(format!("unknown field: {key}"));
    }
    Ok(())
}

fn string<'a>(v: &'a Value, k: &str) -> Result<&'a str, String> {
    v.get(k)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("{k} must be a string"))
}
fn number(v: &Value, k: &str) -> Result<f64, String> {
    let n = v
        .get(k)
        .and_then(Value::as_f64)
        .ok_or_else(|| format!("{k} must be a number"))?;
    if !n.is_finite() {
        return Err(format!("{k} must be finite"));
    }
    Ok(n)
}
fn number_u64(v: &Value, k: &str) -> Result<u64, String> {
    v.get(k)
        .and_then(Value::as_u64)
        .ok_or_else(|| format!("{k} must be an unsigned integer"))
}
fn feature(v: Option<&Value>, identity: &str) -> Result<Feature, String> {
    let v = v.ok_or("feature required")?;
    reject_unknown_fields(v, &["supported", "value", "raw", "min", "max"])?;
    let supported = v
        .get("supported")
        .and_then(Value::as_bool)
        .ok_or("supported must be boolean")?;
    let min = v.get("min").and_then(Value::as_f64).unwrap_or(0.0);
    let max = v.get("max").and_then(Value::as_f64).unwrap_or(100.0);
    if !min.is_finite() || !max.is_finite() || max <= min {
        return Err("invalid feature range".into());
    }
    let raw = v.get("raw").and_then(Value::as_f64);
    let value = raw.or_else(|| {
        v.get("value")
            .and_then(Value::as_f64)
            .map(|n| map_normalized(n, min, max))
    });
    if value
        .map(|n| !n.is_finite() || n < min || n > max)
        .unwrap_or(false)
    {
        return Err("feature value is outside the reported range".into());
    }
    Ok(Feature {
        supported,
        observed: value,
        raw,
        min,
        max,
        desired: None,
        status: if identity == "ambiguous" {
            "ambiguous".into()
        } else if supported {
            "ready".into()
        } else {
            "unsupported".into()
        },
        owned: false,
        ownership_checked_target: None,
        reasons: vec![],
    })
}

fn parse_json_unique(bytes: &[u8]) -> Result<Value, String> {
    struct Unique(Value);
    impl<'de> Deserialize<'de> for Unique {
        fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
            struct UniqueVisitor;
            impl<'de> Visitor<'de> for UniqueVisitor {
                type Value = Unique;
                fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                    f.write_str("JSON without duplicate object keys")
                }
                fn visit_bool<E: serde::de::Error>(self, v: bool) -> Result<Unique, E> {
                    Ok(Unique(Value::Bool(v)))
                }
                fn visit_i64<E: serde::de::Error>(self, v: i64) -> Result<Unique, E> {
                    Ok(Unique(Value::Number(v.into())))
                }
                fn visit_u64<E: serde::de::Error>(self, v: u64) -> Result<Unique, E> {
                    Ok(Unique(Value::Number(v.into())))
                }
                fn visit_f64<E: serde::de::Error>(self, v: f64) -> Result<Unique, E> {
                    Number::from_f64(v)
                        .map(|n| Unique(Value::Number(n)))
                        .ok_or_else(|| E::custom("non-finite number"))
                }
                fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<Unique, E> {
                    Ok(Unique(Value::String(v.to_owned())))
                }
                fn visit_string<E: serde::de::Error>(self, v: String) -> Result<Unique, E> {
                    Ok(Unique(Value::String(v)))
                }
                fn visit_none<E: serde::de::Error>(self) -> Result<Unique, E> {
                    Ok(Unique(Value::Null))
                }
                fn visit_unit<E: serde::de::Error>(self) -> Result<Unique, E> {
                    Ok(Unique(Value::Null))
                }
                fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Unique, A::Error> {
                    let mut values = Vec::new();
                    while let Some(value) = seq.next_element::<Unique>()? {
                        values.push(value.0);
                    }
                    Ok(Unique(Value::Array(values)))
                }
                fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Unique, A::Error> {
                    let mut values = Map::new();
                    while let Some(key) = map.next_key::<String>()? {
                        if values.contains_key(&key) {
                            return Err(serde::de::Error::custom(format!("duplicate field {key}")));
                        }
                        let value = map.next_value::<Unique>()?;
                        values.insert(key, value.0);
                    }
                    Ok(Unique(Value::Object(values)))
                }
            }
            deserializer.deserialize_any(UniqueVisitor)
        }
    }
    serde_json::from_slice::<Unique>(bytes)
        .map(|v| v.0)
        .map_err(|e| format!("invalid JSON: {e}"))
}

fn enqueue(
    q: &mut VecDeque<Action>,
    next: &mut u64,
    monitor_id: &str,
    generation: u64,
    kind: &str,
    value: Option<f64>,
    target: Option<f64>,
) -> Result<(), String> {
    if let Some(active) = q.iter().position(|a| a.monitor_id == monitor_id) {
        if let Some(queued) = q
            .iter_mut()
            .skip(active + 1)
            .find(|a| a.monitor_id == monitor_id && a.kind == kind)
        {
            let id = *next;
            *next = next.saturating_add(1);
            queued.id = id;
            queued.generation = generation;
            queued.value = value;
            queued.target = target;
            return Ok(());
        }
    }
    if q.len() >= MAX_ACTIONS {
        return Err("hardware action queue is full".into());
    }
    let id = *next;
    *next = next.saturating_add(1);
    q.push_back(Action {
        id,
        monitor_id: monitor_id.into(),
        generation,
        kind: kind.into(),
        value,
        target,
    });
    Ok(())
}
fn enqueue_brightness_read(
    q: &mut VecDeque<Action>,
    next: &mut u64,
    monitor_id: &str,
    generation: u64,
    target: f64,
    status: &mut String,
) -> Result<(), String> {
    if q.iter()
        .any(|action| action.monitor_id == monitor_id && action.kind == "read_brightness")
    {
        return Ok(());
    }
    *status = "verifying_ownership".into();
    enqueue(
        q,
        next,
        monitor_id,
        generation,
        "read_brightness",
        None,
        Some(target),
    )
}
// Monitor percent for a built-in level: piecewise linear through (0, 0) and each point
// (MacBook level, monitor percent) in order. Above the last point it holds that point's monitor
// percent. None while the curve has fewer than MIN_CURVE_POINTS points.
fn curve_percent(curve: &AmbientCurve, level: f64) -> Option<f64> {
    if !curve.is_complete() {
        return None;
    }
    let mut knots = vec![(0.0, 0.0)];
    knots.extend(
        curve
            .points
            .iter()
            .map(|point| (point.builtin, point.monitor)),
    );
    let x = level.clamp(0.0, 100.0);
    let Some(segment) = knots.windows(2).find(|pair| x <= pair[1].0) else {
        return knots.last().map(|knot| knot.1);
    };
    let ((x0, y0), (x1, y1)) = (segment[0], segment[1]);
    if x1 <= x0 {
        return Some(y1);
    }
    Some(y0 + (x - x0) * (y1 - y0) / (x1 - x0))
}
fn map_normalized(v: f64, min: f64, max: f64) -> f64 {
    min + (v / 100.0) * (max - min)
}
// Inverse of map_normalized: a raw device value as a percentage of its range.
fn normalized(raw: f64, min: f64, max: f64) -> f64 {
    if max > min {
        (raw - min) * 100.0 / (max - min)
    } else {
        0.0
    }
}
fn feature_json(f: &Feature) -> Value {
    json!({"supported":f.supported,"normalized":f.observed.map(|v|normalized(v,f.min,f.max)),"raw":f.raw,"min":f.min,"max":f.max,"observed":f.observed,"desired":f.desired,"status":f.status,"owned":f.owned,"reasons":f.reasons})
}
