//! Camera session timing, ported from `native/macos/TelaViva/CameraLifecycle.swift`.
//!
//! The lifecycle decides session retention, the acquisition deadline, the warm-up and settle
//! window, the once-per-sample dark settle, the stale-callback check and the assessment token.
//! It never reads a clock: every time-dependent call receives `now` in seconds from the
//! caller's monotonic clock. Whether the camera runs at all is decided by the core's mode and
//! capture mode, which the caller passes in through `reconcile`.

use serde::de::{self, DeserializeSeed, MapAccess, SeqAccess, Visitor};
use serde::Deserializer;
use serde_json::{json, Map, Number, Value};
use std::cell::RefCell;
use std::fmt;

/// Seconds allowed for the first usable frame after acquisition starts.
const ACQUISITION_TIMEOUT: f64 = 5.0;
/// Seconds of exposure settling before a post-settle frame is accepted.
const SETTLE_DURATION: f64 = 5.0;
/// Seconds allowed for the first post-settle frame after settling ends.
const FRAME_TIMEOUT: f64 = 5.0;

/// Largest accepted JSON event, in bytes. This matches the C ABI input cap.
pub const MAX_EVENT_BYTES: usize = 64 * 1024;

/// What the shell does with one camera frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameDecision {
    /// The frame arrived while exposure was still settling; it must not be assessed.
    DiscardWarmup,
    /// Assess the frame for light only (`light_only` mode).
    AssessLight,
    /// Assess the frame for a face (`face` mode).
    AssessPresence,
    /// The post-settle deadline passed before this frame arrived.
    AssessmentExpired,
}

impl FrameDecision {
    /// Returns the snake_case name used as the `value` of a `frame` event.
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::DiscardWarmup => "discard_warmup",
            Self::AssessLight => "assess_light",
            Self::AssessPresence => "assess_presence",
            Self::AssessmentExpired => "assessment_expired",
        }
    }
}

/// Session timing for the camera. Mode and capture mode decide whether the camera runs; this
/// type only owns the timers and bookkeeping for the current session.
#[derive(Debug, Clone, PartialEq)]
pub struct CameraLifecycle {
    generation: u64,
    camera_id: Option<String>,
    mode: String,
    capture_mode: String,
    always_on: bool,
    settling_until: Option<f64>,
    assessment_deadline: Option<f64>,
    acquisition_deadline: Option<f64>,
    assessment_token: u64,
    dark_settle_used: bool,
}

impl Default for CameraLifecycle {
    fn default() -> Self {
        Self::new()
    }
}

impl CameraLifecycle {
    /// Creates an inactive lifecycle: mode `disabled`, capture mode `single_frame`, no timers.
    pub fn new() -> Self {
        Self {
            generation: 0,
            camera_id: None,
            mode: "disabled".to_owned(),
            capture_mode: "single_frame".to_owned(),
            always_on: false,
            settling_until: None,
            assessment_deadline: None,
            acquisition_deadline: None,
            assessment_token: 0,
            dark_settle_used: false,
        }
    }

    /// Generation of the last reconciled snapshot.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// Camera identifier of the last reconciled snapshot, if any.
    pub fn camera_id(&self) -> Option<&str> {
        self.camera_id.as_deref()
    }

    /// Mode of the last reconciled snapshot.
    pub fn mode(&self) -> &str {
        &self.mode
    }

    /// Capture mode of the last reconciled snapshot.
    pub fn capture_mode(&self) -> &str {
        &self.capture_mode
    }

    /// Always-on flag of the last reconciled snapshot.
    pub fn always_on(&self) -> bool {
        self.always_on
    }

    /// End of the exposure settle window, when one is running.
    pub fn settling_until(&self) -> Option<f64> {
        self.settling_until
    }

    /// Deadline for the post-settle frame, when an assessment is pending.
    pub fn assessment_deadline(&self) -> Option<f64> {
        self.assessment_deadline
    }

    /// Deadline for the first usable frame, when acquisition is pending.
    pub fn acquisition_deadline(&self) -> Option<f64> {
        self.acquisition_deadline
    }

    /// Token of the latest assessment. A timer callback is current only if it carries this.
    pub fn assessment_token(&self) -> u64 {
        self.assessment_token
    }

    /// True once the single dark settle of the current sample has been spent.
    pub fn dark_settle_used(&self) -> bool {
        self.dark_settle_used
    }

    /// True when the mode can produce observations (`face` or `light_only`).
    pub fn eligible(&self) -> bool {
        is_eligible_mode(&self.mode)
    }

    /// True while the first usable frame is still awaited.
    pub fn acquisition_pending(&self) -> bool {
        self.acquisition_deadline.is_some()
    }

    /// True when the session should stay open between samples.
    pub fn should_keep_session(&self) -> bool {
        self.eligible() && self.always_on && is_continuous(&self.capture_mode)
    }

    /// True when a callback tagged with `generation` belongs to the current eligible session.
    pub fn accepts_callback(&self, generation: u64) -> bool {
        self.eligible() && generation == self.generation
    }

    /// Applies a new snapshot from the core. Returns true when the existing session must be
    /// stopped before the new state is used; the deadlines are then cancelled.
    pub fn reconcile(
        &mut self,
        mode: &str,
        capture_mode: &str,
        generation: u64,
        camera_id: Option<&str>,
        always_on: bool,
    ) -> bool {
        let next_eligible = is_eligible_mode(mode);
        let same_camera = self.camera_id.as_deref() == camera_id;
        let unchanged = self.mode == mode
            && self.capture_mode == capture_mode
            && self.generation == generation
            && same_camera
            && self.always_on == always_on;
        let retain_always_on = self.always_on
            && always_on
            && next_eligible
            && same_camera
            && is_continuous(capture_mode);
        let retain_return = self.capture_mode == "continuous_return"
            && capture_mode == "continuous_return"
            && next_eligible
            && same_camera;
        let must_restart = !unchanged && !retain_always_on && !retain_return;
        let stop = self.eligible() && (!next_eligible || !same_camera || must_restart);

        self.mode = mode.to_owned();
        self.capture_mode = capture_mode.to_owned();
        self.generation = generation;
        self.camera_id = camera_id.map(str::to_owned);
        self.always_on = always_on;
        if stop {
            self.cancel_deadlines();
        }
        stop
    }

    /// Starts the wait for the first usable frame and resets the dark-settle allowance.
    pub fn begin_acquisition(&mut self, now: f64) {
        self.dark_settle_used = false;
        self.acquisition_deadline = Some(now + ACQUISITION_TIMEOUT);
    }

    /// Clears the acquisition deadline once a frame has arrived.
    pub fn acquired_frame(&mut self) {
        self.acquisition_deadline = None;
    }

    /// Starts a light assessment: five seconds of exposure settling, then a fresh five-second
    /// deadline for the post-settle frame. Each call supersedes earlier tokens.
    pub fn begin_light_assessment(&mut self, now: f64) {
        self.assessment_token = next_token(self.assessment_token);
        self.settling_until = Some(now + SETTLE_DURATION);
        self.assessment_deadline = Some(now + SETTLE_DURATION + FRAME_TIMEOUT);
    }

    /// Starts the one dark settle allowed per sample, in face mode only. Returns true when the
    /// settle started, so the caller keeps the session open. Returns false when the dark result
    /// is final for this sample.
    pub fn begin_dark_settle(&mut self, now: f64) -> bool {
        if self.mode != "face" || self.dark_settle_used {
            return false;
        }
        self.dark_settle_used = true;
        self.assessment_token = next_token(self.assessment_token);
        self.settling_until = Some(now + SETTLE_DURATION);
        self.assessment_deadline = Some(now + SETTLE_DURATION + FRAME_TIMEOUT);
        true
    }

    /// True when `token` is the latest assessment token and its deadline is still armed.
    pub fn is_current_assessment(&self, token: u64) -> bool {
        token == self.assessment_token && self.assessment_deadline.is_some()
    }

    /// Classifies a frame that arrived at `at`. An expired post-settle deadline cancels all
    /// deadlines; a frame inside the settle window is discarded; any other frame clears the
    /// deadlines and is assessed for light or for a face, depending on the mode.
    pub fn frame(&mut self, at: f64) -> FrameDecision {
        if deadline_reached(at, self.assessment_deadline) {
            self.cancel_deadlines();
            return FrameDecision::AssessmentExpired;
        }
        if still_settling(at, self.settling_until) {
            return FrameDecision::DiscardWarmup;
        }
        self.settling_until = None;
        self.assessment_deadline = None;
        self.acquisition_deadline = None;
        if self.mode == "light_only" {
            FrameDecision::AssessLight
        } else {
            FrameDecision::AssessPresence
        }
    }

    /// Reports whether a deadline has passed at `at`. A passed deadline cancels all deadlines.
    pub fn timed_out(&mut self, at: f64) -> bool {
        if deadline_reached(at, self.acquisition_deadline) {
            self.cancel_deadlines();
            return true;
        }
        if deadline_reached(at, self.assessment_deadline) {
            self.cancel_deadlines();
            return true;
        }
        false
    }

    /// Cancels every pending deadline and supersedes the current assessment token. This also
    /// restores the dark-settle allowance for the next sample.
    pub fn cancel_deadlines(&mut self) {
        self.assessment_token = next_token(self.assessment_token);
        self.settling_until = None;
        self.assessment_deadline = None;
        self.acquisition_deadline = None;
        self.dark_settle_used = false;
    }

    /// Parses one JSON event, applies it and returns `{"value": ..., "state": {...}}`.
    ///
    /// The event is validated completely before any field changes, so a rejected event leaves
    /// the lifecycle exactly as it was.
    pub fn step(&mut self, event_json: &[u8]) -> Result<String, StepError> {
        let event = Event::parse(event_json)?;
        let value = self.apply(event);
        Ok(json!({ "value": value, "state": self.state_value() }).to_string())
    }

    /// Applies a validated event and returns its `value`.
    fn apply(&mut self, event: Event) -> Value {
        match event {
            Event::Reconcile {
                mode,
                capture_mode,
                generation,
                camera_id,
                always_on,
            } => {
                let stop = self.reconcile(
                    &mode,
                    &capture_mode,
                    generation,
                    camera_id.as_deref(),
                    always_on,
                );
                Value::Bool(stop)
            }
            Event::BeginAcquisition { now } => {
                self.begin_acquisition(now);
                Value::Null
            }
            Event::AcquiredFrame => {
                self.acquired_frame();
                Value::Null
            }
            Event::BeginLightAssessment { now } => {
                self.begin_light_assessment(now);
                Value::Null
            }
            Event::BeginDarkSettle { now } => Value::Bool(self.begin_dark_settle(now)),
            Event::IsCurrentAssessment { token } => Value::Bool(self.is_current_assessment(token)),
            Event::Frame { now } => Value::from(self.frame(now).as_str()),
            Event::TimedOut { now } => Value::Bool(self.timed_out(now)),
            Event::CancelDeadlines => {
                self.cancel_deadlines();
                Value::Null
            }
            Event::AcceptsCallback { generation } => Value::Bool(self.accepts_callback(generation)),
        }
    }

    /// Builds the flat `state` object that the shells read.
    fn state_value(&self) -> Value {
        json!({
            "generation": self.generation,
            "camera_id": self.camera_id,
            "mode": self.mode,
            "capture_mode": self.capture_mode,
            "always_on": self.always_on,
            "eligible": self.eligible(),
            "acquisition_pending": self.acquisition_pending(),
            "should_keep_session": self.should_keep_session(),
            "settling_until": self.settling_until,
            "assessment_deadline": self.assessment_deadline,
            "acquisition_deadline": self.acquisition_deadline,
            "assessment_token": self.assessment_token,
        })
    }
}

/// Why a JSON event was rejected. A rejected event never changes the lifecycle.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StepError {
    /// The event is longer than `MAX_EVENT_BYTES`.
    InputTooLarge { length: usize, limit: usize },
    /// The bytes are not one well-formed JSON value (includes invalid UTF-8 and trailing data).
    MalformedJson(String),
    /// An object repeats a key, at the top level or nested inside a value.
    DuplicateKey(String),
    /// The event is valid JSON but not an object.
    NotAnObject,
    /// A required field is absent.
    MissingField(&'static str),
    /// A field has the wrong JSON type or a value outside its range.
    InvalidField {
        field: &'static str,
        expected: &'static str,
    },
    /// The `type` names no lifecycle event.
    UnknownEventType(String),
    /// The event carries a field that its type does not define.
    UnknownField(String),
}

impl fmt::Display for StepError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InputTooLarge { length, limit } => {
                write!(formatter, "event has {length} bytes; the limit is {limit}")
            }
            Self::MalformedJson(detail) => write!(formatter, "malformed JSON: {detail}"),
            Self::DuplicateKey(key) => write!(formatter, "duplicate object key: {key}"),
            Self::NotAnObject => formatter.write_str("event must be a JSON object"),
            Self::MissingField(field) => write!(formatter, "missing field: {field}"),
            Self::InvalidField { field, expected } => {
                write!(formatter, "field {field} must be {expected}")
            }
            Self::UnknownEventType(kind) => write!(formatter, "unknown event type: {kind}"),
            Self::UnknownField(field) => write!(formatter, "unknown field: {field}"),
        }
    }
}

impl std::error::Error for StepError {}

/// One validated lifecycle event. Parsing finishes before the lifecycle is touched.
enum Event {
    Reconcile {
        mode: String,
        capture_mode: String,
        generation: u64,
        camera_id: Option<String>,
        always_on: bool,
    },
    BeginAcquisition {
        now: f64,
    },
    AcquiredFrame,
    BeginLightAssessment {
        now: f64,
    },
    BeginDarkSettle {
        now: f64,
    },
    IsCurrentAssessment {
        token: u64,
    },
    Frame {
        now: f64,
    },
    TimedOut {
        now: f64,
    },
    CancelDeadlines,
    AcceptsCallback {
        generation: u64,
    },
}

impl Event {
    /// Parses and validates one event object. Unknown types and unknown fields are errors.
    fn parse(input: &[u8]) -> Result<Self, StepError> {
        if input.len() > MAX_EVENT_BYTES {
            return Err(StepError::InputTooLarge {
                length: input.len(),
                limit: MAX_EVENT_BYTES,
            });
        }
        let Value::Object(object) = parse_unique(input)? else {
            return Err(StepError::NotAnObject);
        };
        let mut fields = Fields { object };
        let kind = match fields.take("type")? {
            Value::String(kind) => kind,
            _ => return Err(invalid("type", "a string")),
        };
        let event = match kind.as_str() {
            "reconcile" => Self::Reconcile {
                mode: fields.string("mode")?,
                capture_mode: fields.string("capture_mode")?,
                generation: fields.unsigned("generation")?,
                camera_id: fields.optional_string("camera_id")?,
                always_on: fields.boolean("always_on")?,
            },
            "begin_acquisition" => Self::BeginAcquisition {
                now: fields.seconds("now")?,
            },
            "acquired_frame" => Self::AcquiredFrame,
            "begin_light_assessment" => Self::BeginLightAssessment {
                now: fields.seconds("now")?,
            },
            "begin_dark_settle" => Self::BeginDarkSettle {
                now: fields.seconds("now")?,
            },
            "is_current_assessment" => Self::IsCurrentAssessment {
                token: fields.unsigned("token")?,
            },
            "frame" => Self::Frame {
                now: fields.seconds("now")?,
            },
            "timed_out" => Self::TimedOut {
                now: fields.seconds("now")?,
            },
            "cancel_deadlines" => Self::CancelDeadlines,
            "accepts_callback" => Self::AcceptsCallback {
                generation: fields.unsigned("generation")?,
            },
            other => return Err(StepError::UnknownEventType(other.to_owned())),
        };
        fields.finish()?;
        Ok(event)
    }
}

/// Typed access to one event object. Each accessor removes the member it reads, so anything
/// left over when `finish` runs is an unknown field.
struct Fields {
    object: Map<String, Value>,
}

impl Fields {
    fn take(&mut self, name: &'static str) -> Result<Value, StepError> {
        match self.object.remove(name) {
            Some(value) => Ok(value),
            None => Err(StepError::MissingField(name)),
        }
    }

    fn string(&mut self, name: &'static str) -> Result<String, StepError> {
        match self.take(name)? {
            Value::String(text) => Ok(text),
            _ => Err(invalid(name, "a string")),
        }
    }

    fn optional_string(&mut self, name: &'static str) -> Result<Option<String>, StepError> {
        match self.take(name)? {
            Value::Null => Ok(None),
            Value::String(text) => Ok(Some(text)),
            _ => Err(invalid(name, "a string or null")),
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

    fn seconds(&mut self, name: &'static str) -> Result<f64, StepError> {
        match self.take(name)?.as_f64() {
            Some(seconds) if seconds.is_finite() => Ok(seconds),
            _ => Err(invalid(name, "a finite number of seconds")),
        }
    }

    fn finish(self) -> Result<(), StepError> {
        match self.object.into_iter().next() {
            Some((field, _)) => Err(StepError::UnknownField(field)),
            None => Ok(()),
        }
    }
}

fn invalid(field: &'static str, expected: &'static str) -> StepError {
    StepError::InvalidField { field, expected }
}

/// Parses one JSON value. A repeated object key at any depth is reported as `DuplicateKey`.
fn parse_unique(input: &[u8]) -> Result<Value, StepError> {
    let duplicate = RefCell::new(None);
    let mut deserializer = serde_json::Deserializer::from_slice(input);
    let value = match UniqueSeed(&duplicate).deserialize(&mut deserializer) {
        Ok(value) => value,
        Err(error) => {
            let repeated = duplicate.borrow().clone();
            return Err(match repeated {
                Some(key) => StepError::DuplicateKey(key),
                None => StepError::MalformedJson(error.to_string()),
            });
        }
    };
    deserializer
        .end()
        .map_err(|error| StepError::MalformedJson(error.to_string()))?;
    Ok(value)
}

/// Reads one JSON value. When an object repeats a key, the key is recorded in the slot and
/// the parse fails, so the caller can tell a duplicate apart from malformed input.
struct UniqueSeed<'a>(&'a RefCell<Option<String>>);

impl<'de> DeserializeSeed<'de> for UniqueSeed<'_> {
    type Value = Value;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<Value, D::Error> {
        deserializer.deserialize_any(UniqueVisitor(self.0))
    }
}

struct UniqueVisitor<'a>(&'a RefCell<Option<String>>);

impl<'de> Visitor<'de> for UniqueVisitor<'_> {
    type Value = Value;

    fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("a JSON value without repeated object keys")
    }

    fn visit_bool<E: de::Error>(self, value: bool) -> Result<Value, E> {
        Ok(Value::Bool(value))
    }

    fn visit_i64<E: de::Error>(self, value: i64) -> Result<Value, E> {
        Ok(Value::from(value))
    }

    fn visit_u64<E: de::Error>(self, value: u64) -> Result<Value, E> {
        Ok(Value::from(value))
    }

    fn visit_f64<E: de::Error>(self, value: f64) -> Result<Value, E> {
        Number::from_f64(value)
            .map(Value::Number)
            .ok_or_else(|| E::custom("non-finite number"))
    }

    fn visit_str<E: de::Error>(self, value: &str) -> Result<Value, E> {
        Ok(Value::String(value.to_owned()))
    }

    fn visit_unit<E: de::Error>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }

    fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Value, A::Error> {
        let mut values = Vec::new();
        while let Some(value) = seq.next_element_seed(UniqueSeed(self.0))? {
            values.push(value);
        }
        Ok(Value::Array(values))
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Value, A::Error> {
        let mut object = Map::new();
        while let Some(key) = map.next_key::<String>()? {
            if object.contains_key(&key) {
                *self.0.borrow_mut() = Some(key);
                return Err(de::Error::custom("duplicate object key"));
            }
            let value = map.next_value_seed(UniqueSeed(self.0))?;
            object.insert(key, value);
        }
        Ok(Value::Object(object))
    }
}

/// Advances the assessment token. Swift uses `&+=` here; wrapping keeps the token from
/// trapping after `u64::MAX` updates.
fn next_token(token: u64) -> u64 {
    token.wrapping_add(1)
}

/// True when the deadline is set and `now` has reached it (`now >= deadline`).
fn deadline_reached(now: f64, deadline: Option<f64>) -> bool {
    matches!(deadline, Some(deadline) if now >= deadline)
}

/// True when the settle window is set and `now` is still before its end (`now < until`).
fn still_settling(now: f64, until: Option<f64>) -> bool {
    matches!(until, Some(until) if now < until)
}

fn is_eligible_mode(mode: &str) -> bool {
    mode == "face" || mode == "light_only"
}

fn is_continuous(capture_mode: &str) -> bool {
    capture_mode == "continuous_monitoring" || capture_mode == "continuous_return"
}

#[cfg(test)]
mod tests {
    use super::*;

    // The token field is private, so the u64::MAX wrap is checked here. Integration tests can
    // only reach the token through public calls, which never reach u64::MAX.
    #[test]
    fn assessment_token_wraps_from_u64_max_without_panicking() {
        assert_eq!(next_token(u64::MAX), 0);

        let mut lifecycle = CameraLifecycle::new();
        lifecycle.assessment_token = u64::MAX;
        lifecycle.begin_light_assessment(0.0);
        assert_eq!(lifecycle.assessment_token, 0);
        assert!(lifecycle.is_current_assessment(0));

        lifecycle.assessment_token = u64::MAX;
        lifecycle.cancel_deadlines();
        assert_eq!(lifecycle.assessment_token, 0);

        lifecycle.mode = "face".to_owned();
        lifecycle.assessment_token = u64::MAX;
        assert!(lifecycle.begin_dark_settle(0.0));
        assert_eq!(lifecycle.assessment_token, 0);
    }
}
