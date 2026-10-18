//! Strict parsing for JSON objects that carry typed fields. Inputs are size-capped before parsing,
//! repeated keys and non-finite numbers are rejected at any depth, trailing data is rejected, and
//! members that no accessor reads are reported as unknown.

use serde::de::{self, DeserializeSeed, MapAccess, SeqAccess, Visitor};
use serde::Deserializer;
use serde_json::{Map, Number, Value};
use std::cell::RefCell;
use std::fmt;

/// Why a JSON object was rejected. The messages are the ones shown to shells and tests.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum JsonError {
    /// The input is longer than the caller's limit.
    InputTooLarge { length: usize, limit: usize },
    /// The bytes are not one well-formed JSON value (includes invalid UTF-8 and trailing data).
    Malformed(String),
    /// An object repeats a key, at the top level or nested inside a value.
    DuplicateKey(String),
    /// The value is valid JSON but not an object.
    NotAnObject,
    /// A required field is absent.
    MissingField(&'static str),
    /// A field has the wrong JSON type or a value outside its range.
    InvalidField {
        field: &'static str,
        expected: &'static str,
    },
    /// The object carries a member that no accessor read.
    UnknownField(String),
}

impl fmt::Display for JsonError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InputTooLarge { length, limit } => {
                write!(formatter, "event has {length} bytes; the limit is {limit}")
            }
            Self::Malformed(detail) => write!(formatter, "malformed JSON: {detail}"),
            Self::DuplicateKey(key) => write!(formatter, "duplicate object key: {key}"),
            Self::NotAnObject => formatter.write_str("event must be a JSON object"),
            Self::MissingField(field) => write!(formatter, "missing field: {field}"),
            Self::InvalidField { field, expected } => {
                write!(formatter, "field {field} must be {expected}")
            }
            Self::UnknownField(field) => write!(formatter, "unknown field: {field}"),
        }
    }
}

impl std::error::Error for JsonError {}

/// Why a JSON event was rejected. A rejected event never changes the receiving state machine.
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

impl From<JsonError> for StepError {
    fn from(error: JsonError) -> Self {
        match error {
            JsonError::InputTooLarge { length, limit } => Self::InputTooLarge { length, limit },
            JsonError::Malformed(detail) => Self::MalformedJson(detail),
            JsonError::DuplicateKey(key) => Self::DuplicateKey(key),
            JsonError::NotAnObject => Self::NotAnObject,
            JsonError::MissingField(field) => Self::MissingField(field),
            JsonError::InvalidField { field, expected } => Self::InvalidField { field, expected },
            JsonError::UnknownField(field) => Self::UnknownField(field),
        }
    }
}

/// Parses `input` as one strict JSON object. Inputs longer than `limit` are rejected before any
/// parsing happens.
pub(crate) fn parse_object(input: &[u8], limit: usize) -> Result<Fields, JsonError> {
    if input.len() > limit {
        return Err(JsonError::InputTooLarge {
            length: input.len(),
            limit,
        });
    }
    let Value::Object(object) = parse_unique(input)? else {
        return Err(JsonError::NotAnObject);
    };
    Ok(Fields { object })
}

/// Typed access to one object. Each accessor removes the member it reads, so anything left over
/// when `finish` runs is an unknown field.
pub(crate) struct Fields {
    object: Map<String, Value>,
}

impl Fields {
    fn take(&mut self, name: &'static str) -> Result<Value, JsonError> {
        match self.object.remove(name) {
            Some(value) => Ok(value),
            None => Err(JsonError::MissingField(name)),
        }
    }

    pub(crate) fn string(&mut self, name: &'static str) -> Result<String, JsonError> {
        match self.take(name)? {
            Value::String(text) => Ok(text),
            _ => Err(invalid(name, "a string")),
        }
    }

    pub(crate) fn optional_string(
        &mut self,
        name: &'static str,
    ) -> Result<Option<String>, JsonError> {
        match self.take(name)? {
            Value::Null => Ok(None),
            Value::String(text) => Ok(Some(text)),
            _ => Err(invalid(name, "a string or null")),
        }
    }

    pub(crate) fn boolean(&mut self, name: &'static str) -> Result<bool, JsonError> {
        match self.take(name)? {
            Value::Bool(flag) => Ok(flag),
            _ => Err(invalid(name, "true or false")),
        }
    }

    pub(crate) fn unsigned(&mut self, name: &'static str) -> Result<u64, JsonError> {
        match self.take(name)?.as_u64() {
            Some(number) => Ok(number),
            None => Err(invalid(name, "a non-negative integer")),
        }
    }

    pub(crate) fn seconds(&mut self, name: &'static str) -> Result<f64, JsonError> {
        match self.take(name)?.as_f64() {
            Some(seconds) if seconds.is_finite() => Ok(seconds),
            _ => Err(invalid(name, "a finite number of seconds")),
        }
    }

    pub(crate) fn finish(self) -> Result<(), JsonError> {
        match self.object.into_iter().next() {
            Some((field, _)) => Err(JsonError::UnknownField(field)),
            None => Ok(()),
        }
    }
}

fn invalid(field: &'static str, expected: &'static str) -> JsonError {
    JsonError::InvalidField { field, expected }
}

/// Parses one JSON value. A repeated object key at any depth is reported as `DuplicateKey`.
fn parse_unique(input: &[u8]) -> Result<Value, JsonError> {
    let duplicate = RefCell::new(None);
    let mut deserializer = serde_json::Deserializer::from_slice(input);
    let value = match UniqueSeed(&duplicate).deserialize(&mut deserializer) {
        Ok(value) => value,
        Err(error) => {
            let repeated = duplicate.borrow().clone();
            return Err(match repeated {
                Some(key) => JsonError::DuplicateKey(key),
                None => JsonError::Malformed(error.to_string()),
            });
        }
    };
    deserializer
        .end()
        .map_err(|error| JsonError::Malformed(error.to_string()))?;
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_input_over_the_limit_before_parsing() {
        assert_eq!(
            parse_object(b"{}", 1).map(|_| ()),
            Err(JsonError::InputTooLarge {
                length: 2,
                limit: 1
            })
        );
    }

    #[test]
    fn rejects_duplicate_keys_at_any_depth() {
        assert_eq!(
            parse_object(br#"{"a":1,"a":2}"#, 100).map(|_| ()),
            Err(JsonError::DuplicateKey("a".to_owned()))
        );
        assert_eq!(
            parse_object(br#"{"b":{"a":1,"a":2}}"#, 100).map(|_| ()),
            Err(JsonError::DuplicateKey("a".to_owned()))
        );
    }

    #[test]
    fn rejects_trailing_data() {
        assert!(matches!(
            parse_object(b"{} x", 100).map(|_| ()),
            Err(JsonError::Malformed(_))
        ));
    }

    #[test]
    fn rejects_unread_members_as_unknown_fields() {
        let fields = parse_object(br#"{"a":1}"#, 100).unwrap();
        assert_eq!(
            fields.finish(),
            Err(JsonError::UnknownField("a".to_owned()))
        );
    }
}
