//! Control event and effect types, and their JSON form.
//!
//! `strict_json` checks size, syntax, duplicate keys and trailing data. Nested values need more
//! accessors than `strict_json::Fields` offers, so the members are read here through `Members`,
//! which rejects any member left unread.

use super::keys::{KEY_DEFAULT_STEP, KEY_MAX_STEP, KEY_MIN_STEP};
use super::StepError;
use crate::strict_json::parse_object;
use serde_json::{json, Map, Value};

/// Largest accepted JSON event, in bytes. This matches the C ABI input cap.
pub const MAX_EVENT_BYTES: usize = 64 * 1024;
/// Monitors accepted in one `observe` snapshot.
pub(crate) const MAX_MONITORS: usize = 64;
/// Pending actions accepted in one `observe` snapshot.
pub(crate) const MAX_PENDING_ACTIONS: usize = 128;
/// Key step in whole points until the first `configure` event.
pub(crate) const DEFAULT_STEP: f64 = KEY_DEFAULT_STEP;

/// Feature that a slider or a key targets.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Feature {
    Brightness,
    Volume,
}

impl Feature {
    /// Returns the snake_case name used in JSON.
    pub(crate) fn as_str(&self) -> &'static str {
        match self {
            Self::Brightness => "brightness",
            Self::Volume => "volume",
        }
    }

    pub(crate) fn parse(text: &str) -> Option<Self> {
        match text {
            "brightness" => Some(Self::Brightness),
            "volume" => Some(Self::Volume),
            _ => None,
        }
    }
}

/// Hardware key decoded by the shell.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum KeyKind {
    VolumeUp,
    VolumeDown,
    Mute,
    BrightnessUp,
    BrightnessDown,
}

impl KeyKind {
    pub(crate) fn parse(text: &str) -> Option<Self> {
        match text {
            "volume_up" => Some(Self::VolumeUp),
            "volume_down" => Some(Self::VolumeDown),
            "mute" => Some(Self::Mute),
            "brightness_up" => Some(Self::BrightnessUp),
            "brightness_down" => Some(Self::BrightnessDown),
            _ => None,
        }
    }
}

/// Whether a key event is a press or a release.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum KeyPhase {
    Down,
    Up,
}

impl KeyPhase {
    pub(crate) fn parse(text: &str) -> Option<Self> {
        match text {
            "down" => Some(Self::Down),
            "up" => Some(Self::Up),
            _ => None,
        }
    }
}

/// Target that the shell resolved for a key event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct KeyOwner {
    pub(crate) monitor_id: String,
    pub(crate) generation: u64,
}

/// Why one DDC primitive failed, as reported by the shell.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum AttemptError {
    None,
    Unsupported,
    Io,
    Disconnected,
}

impl AttemptError {
    pub(crate) fn parse(text: &str) -> Option<Self> {
        match text {
            "none" => Some(Self::None),
            "unsupported" => Some(Self::Unsupported),
            "io" => Some(Self::Io),
            "disconnected" => Some(Self::Disconnected),
            _ => None,
        }
    }
}

/// Kind of DDC primitive that the core asks the shell to run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum AttemptOp {
    Read,
    Write,
}

impl AttemptOp {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Read => "read",
            Self::Write => "write",
        }
    }
}

/// One channel (brightness or volume) as the shell observed it.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct ChannelObs {
    pub(crate) supported: bool,
    pub(crate) observed: Option<f64>,
    pub(crate) min: f64,
    pub(crate) max: f64,
}

/// One monitor in an `observe` snapshot.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct MonitorObs {
    pub(crate) id: String,
    pub(crate) generation: u64,
    pub(crate) display_state: String,
    pub(crate) identity_status: String,
    pub(crate) brightness: ChannelObs,
    pub(crate) volume: ChannelObs,
}

/// One DDC action that the shell listed in an `observe` snapshot.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct PendingActionObs {
    pub(crate) id: u64,
    pub(crate) generation: u64,
    pub(crate) monitor_id: String,
    pub(crate) kind: String,
    pub(crate) value: Option<f64>,
}

/// One `observe` snapshot.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Observation {
    pub(crate) revision: u64,
    pub(crate) monitors: Vec<MonitorObs>,
    pub(crate) pending_actions: Vec<PendingActionObs>,
}

/// One validated shell event. Parsing finishes before the machine is touched.
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum Event {
    Configure {
        brightness_step: f64,
        volume_step: f64,
        keys_enabled: bool,
        /// Absent: unchanged. `null`: no owner. A string: that monitor owns volume keys.
        volume_owner: Option<Option<String>>,
    },
    Observe(Observation),
    MonitorProfile {
        monitor_id: String,
        manufacturer: String,
    },
    MonitorRemoved {
        monitor_id: String,
    },
    DisplayPower {
        on: bool,
    },
    Slider {
        monitor_id: String,
        feature: Feature,
        value: f64,
    },
    Key {
        key: KeyKind,
        phase: KeyPhase,
        repeat: bool,
        fine: bool,
        owner: Option<KeyOwner>,
    },
    Submitted {
        token: u64,
        accepted: bool,
    },
    AttemptDone {
        token: u64,
        ok: bool,
        error: Option<AttemptError>,
        value: Option<f64>,
    },
    Tick,
    QueryDraft {
        monitor_id: String,
        feature: Feature,
    },
    Shutdown,
}

/// One instruction for the shell. The shell runs effects in order, outside its own locks.
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum Effect {
    SubmitCommand {
        token: u64,
        command_json: String,
    },
    RunAttempt {
        token: u64,
        monitor_id: String,
        op: AttemptOp,
        code: u8,
        value: Option<f64>,
    },
    SubmitResult {
        token: u64,
        result_json: String,
    },
    DraftChanged {
        monitor_id: String,
        feature: Feature,
    },
}

impl Effect {
    /// Builds the JSON object that the shell reads. `value` appears only on writes.
    pub(crate) fn render(&self) -> Value {
        match self {
            Self::SubmitCommand {
                token,
                command_json,
            } => json!({
                "type": "submit_command",
                "token": token,
                "command_json": command_json,
            }),
            Self::RunAttempt {
                token,
                monitor_id,
                op,
                code,
                value,
            } => {
                let mut effect = json!({
                    "type": "run_attempt",
                    "token": token,
                    "monitor_id": monitor_id,
                    "op": op.as_str(),
                    "code": code,
                });
                if matches!(op, AttemptOp::Write) {
                    effect["value"] = json!(value);
                }
                effect
            }
            Self::SubmitResult { token, result_json } => json!({
                "type": "submit_result",
                "token": token,
                "result_json": result_json,
            }),
            Self::DraftChanged {
                monitor_id,
                feature,
            } => json!({
                "type": "draft_changed",
                "monitor_id": monitor_id,
                "feature": feature.as_str(),
            }),
        }
    }
}

/// The result of one step: the event's value, the effects in order, and the next deadline.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct StepOutput {
    pub(crate) value: Value,
    pub(crate) effects: Vec<Effect>,
    pub(crate) next_deadline: Option<f64>,
}

impl StepOutput {
    /// Renders `{"value":..,"effects":[..],"next_deadline":..}`.
    pub(crate) fn render(&self) -> String {
        let effects: Vec<Value> = self.effects.iter().map(Effect::render).collect();
        json!({
            "value": self.value,
            "effects": effects,
            "next_deadline": self.next_deadline,
        })
        .to_string()
    }
}

/// Clamps a key step to whole points from 1 to 15. A non-finite step becomes the default.
pub(crate) fn clamped_step(value: f64) -> f64 {
    if value.is_finite() {
        value.round().clamp(KEY_MIN_STEP, KEY_MAX_STEP)
    } else {
        KEY_DEFAULT_STEP
    }
}

/// Parses and validates one event. Size, syntax, duplicate keys and trailing data are checked
/// first; then every member is read, and any member left over is an unknown field.
pub(crate) fn parse_event(input: &[u8]) -> Result<(f64, Event), StepError> {
    parse_object(input, MAX_EVENT_BYTES)?;
    let root = serde_json::from_slice::<Value>(input)
        .map_err(|error| StepError::MalformedJson(error.to_string()))?;
    let Value::Object(object) = root else {
        return Err(StepError::NotAnObject);
    };
    let mut members = Members { object };
    let kind = members.string("type")?;
    // `now` is judged after the type, so an unknown type is reported as such.
    let now = members.number("now");
    let event = match kind.as_str() {
        "configure" => Event::Configure {
            brightness_step: members.number("brightness_step")?,
            volume_step: members.number("volume_step")?,
            keys_enabled: members.boolean("keys_enabled")?,
            volume_owner: parse_volume_owner(members.remove_optional("volume_owner"))?,
        },
        "observe" => Event::Observe(parse_observation(&mut members)?),
        "monitor_profile" => Event::MonitorProfile {
            monitor_id: members.string("monitor_id")?,
            manufacturer: members.string("manufacturer")?,
        },
        "monitor_removed" => Event::MonitorRemoved {
            monitor_id: members.string("monitor_id")?,
        },
        "display_power" => Event::DisplayPower {
            on: members.boolean("on")?,
        },
        "slider" => Event::Slider {
            monitor_id: members.string("monitor_id")?,
            feature: named(
                &mut members,
                "feature",
                "brightness or volume",
                Feature::parse,
            )?,
            value: members.number("value")?,
        },
        "key" => Event::Key {
            key: named(
                &mut members,
                "key",
                "a volume or brightness key",
                KeyKind::parse,
            )?,
            phase: named(&mut members, "phase", "down or up", KeyPhase::parse)?,
            repeat: members.boolean("repeat")?,
            fine: members.boolean("fine")?,
            owner: parse_owner(members.take("owner")?)?,
        },
        "submitted" => Event::Submitted {
            token: members.unsigned("token")?,
            accepted: members.boolean("accepted")?,
        },
        "attempt_done" => {
            let token = members.unsigned("token")?;
            let ok = members.boolean("ok")?;
            // `error` is required when the attempt failed; a null is accepted only on success.
            let error = match members.remove_optional("error") {
                None if ok => None,
                None => return Err(StepError::MissingField("error")),
                Some(Value::Null) if ok => None,
                Some(value) => Some(parse_attempt_error(value)?),
            };
            Event::AttemptDone {
                token,
                ok,
                error,
                value: members.optional_number("value")?,
            }
        }
        "tick" => Event::Tick,
        "query_draft" => Event::QueryDraft {
            monitor_id: members.string("monitor_id")?,
            feature: named(
                &mut members,
                "feature",
                "brightness or volume",
                Feature::parse,
            )?,
        },
        "shutdown" => Event::Shutdown,
        other => return Err(StepError::UnknownEventType(other.to_owned())),
    };
    let now = now?;
    members.finish()?;
    Ok((now, event))
}

fn parse_observation(members: &mut Members) -> Result<Observation, StepError> {
    let revision = members.unsigned("revision")?;
    let monitors = members
        .array("monitors", MAX_MONITORS)?
        .into_iter()
        .map(parse_monitor)
        .collect::<Result<Vec<_>, _>>()?;
    let pending_actions = members
        .array("pending_actions", MAX_PENDING_ACTIONS)?
        .into_iter()
        .map(parse_pending_action)
        .collect::<Result<Vec<_>, _>>()?;
    Ok(Observation {
        revision,
        monitors,
        pending_actions,
    })
}

fn parse_monitor(value: Value) -> Result<MonitorObs, StepError> {
    let mut monitor = Members::of(value, "monitors")?;
    let id = monitor.string("id")?;
    let generation = monitor.unsigned("generation")?;
    let display_state = monitor.string("display_state")?;
    let identity_status = monitor.string("identity_status")?;
    let brightness = parse_channel(monitor.take("brightness")?, "brightness")?;
    let volume = parse_channel(monitor.take("volume")?, "volume")?;
    monitor.finish()?;
    Ok(MonitorObs {
        id,
        generation,
        display_state,
        identity_status,
        brightness,
        volume,
    })
}

fn parse_channel(value: Value, field: &'static str) -> Result<ChannelObs, StepError> {
    let mut channel = Members::of(value, field)?;
    let supported = channel.boolean("supported")?;
    let observed = channel.optional_number("observed")?;
    let min = channel.number("min")?;
    let max = channel.number("max")?;
    channel.finish()?;
    Ok(ChannelObs {
        supported,
        observed,
        min,
        max,
    })
}

fn parse_pending_action(value: Value) -> Result<PendingActionObs, StepError> {
    let mut action = Members::of(value, "pending_actions")?;
    let id = action.unsigned("id")?;
    let generation = action.unsigned("generation")?;
    let monitor_id = action.string("monitor_id")?;
    let kind = action.string("kind")?;
    let target = action.optional_number("value")?;
    action.finish()?;
    Ok(PendingActionObs {
        id,
        generation,
        monitor_id,
        kind,
        value: target,
    })
}

fn parse_owner(value: Value) -> Result<Option<KeyOwner>, StepError> {
    if value.is_null() {
        return Ok(None);
    }
    let mut owner = Members::of(value, "owner")?;
    let monitor_id = owner.string("monitor_id")?;
    let generation = owner.unsigned("generation")?;
    owner.finish()?;
    Ok(Some(KeyOwner {
        monitor_id,
        generation,
    }))
}

/// Reads the optional `volume_owner` member: absent is `None`, `null` clears the owner and a
/// string names it. Any other type is an invalid field.
fn parse_volume_owner(value: Option<Value>) -> Result<Option<Option<String>>, StepError> {
    match value {
        None => Ok(None),
        Some(Value::Null) => Ok(Some(None)),
        Some(Value::String(id)) => Ok(Some(Some(id))),
        Some(_) => Err(invalid("volume_owner", "a string or null")),
    }
}

fn parse_attempt_error(value: Value) -> Result<AttemptError, StepError> {
    match value {
        Value::String(text) => AttemptError::parse(&text)
            .ok_or_else(|| invalid("error", "none, unsupported, io or disconnected")),
        _ => Err(invalid("error", "a failure name")),
    }
}

/// Reads a string member and maps it through `parse`; an unknown name is an invalid field.
fn named<T>(
    members: &mut Members,
    name: &'static str,
    expected: &'static str,
    parse: fn(&str) -> Option<T>,
) -> Result<T, StepError> {
    let text = members.string(name)?;
    parse(&text).ok_or_else(|| invalid(name, expected))
}

fn invalid(field: &'static str, expected: &'static str) -> StepError {
    StepError::InvalidField { field, expected }
}

/// Typed access to one JSON object. Each accessor removes the member it reads, so a member left
/// over at `finish` is an unknown field.
struct Members {
    object: Map<String, Value>,
}

impl Members {
    fn of(value: Value, field: &'static str) -> Result<Self, StepError> {
        match value {
            Value::Object(object) => Ok(Self { object }),
            _ => Err(invalid(field, "an object")),
        }
    }

    fn take(&mut self, name: &'static str) -> Result<Value, StepError> {
        match self.object.remove(name) {
            Some(value) => Ok(value),
            None => Err(StepError::MissingField(name)),
        }
    }

    fn remove_optional(&mut self, name: &'static str) -> Option<Value> {
        self.object.remove(name)
    }

    fn string(&mut self, name: &'static str) -> Result<String, StepError> {
        match self.take(name)? {
            Value::String(text) => Ok(text),
            _ => Err(invalid(name, "a string")),
        }
    }

    fn boolean(&mut self, name: &'static str) -> Result<bool, StepError> {
        match self.take(name)? {
            Value::Bool(flag) => Ok(flag),
            _ => Err(invalid(name, "true or false")),
        }
    }

    fn unsigned(&mut self, name: &'static str) -> Result<u64, StepError> {
        match self.take(name)?.as_u64() {
            Some(number) => Ok(number),
            None => Err(invalid(name, "a non-negative integer")),
        }
    }

    fn number(&mut self, name: &'static str) -> Result<f64, StepError> {
        match self.take(name)?.as_f64() {
            Some(number) if number.is_finite() => Ok(number),
            _ => Err(invalid(name, "a finite number")),
        }
    }

    fn optional_number(&mut self, name: &'static str) -> Result<Option<f64>, StepError> {
        match self.take(name)? {
            Value::Null => Ok(None),
            value => match value.as_f64() {
                Some(number) if number.is_finite() => Ok(Some(number)),
                _ => Err(invalid(name, "a finite number or null")),
            },
        }
    }

    fn array(&mut self, name: &'static str, limit: usize) -> Result<Vec<Value>, StepError> {
        match self.take(name)? {
            Value::Array(items) if items.len() <= limit => Ok(items),
            Value::Array(_) => Err(invalid(name, "an array within its size limit")),
            _ => Err(invalid(name, "an array")),
        }
    }

    fn finish(self) -> Result<(), StepError> {
        match self.object.into_iter().next() {
            Some((field, _)) => Err(StepError::UnknownField(field)),
            None => Ok(()),
        }
    }
}
