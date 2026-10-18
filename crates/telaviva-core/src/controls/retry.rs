//! DDC attempt budget, backoff and timing for one monitor. The shell runs each primitive and
//! reports its result; this module decides what runs next and when.

use super::model::AttemptError;

/// Attempts per DDC primitive (one write or one read), counting the first try.
pub(crate) const ATTEMPTS_PER_PRIMITIVE: u32 = 3;
/// Delay before the next attempt, indexed by the attempts already made (after 1, after 2).
pub(crate) const BACKOFF_SECS: [f64; 2] = [0.2, 1.0];
/// Wait after the last write before the readback.
pub(crate) const SETTLE_SECS: f64 = 0.06;
/// Gap between the two writes of a duplicate-set profile (the LG UltraFine drops the first Set).
pub(crate) const DUPLICATE_GAP_SECS: f64 = 0.01;
/// Time after wake during which DDC is held, because some monitors answer slowly after wake.
pub(crate) const WAKE_GRACE_SECS: f64 = 2.0;
/// Times a rejected `hardware_result` is resubmitted before it is dropped.
pub(crate) const RESULT_RESUBMITS: u32 = 3;

/// Per-monitor timing data, keyed by the EDID manufacturer code.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub(crate) struct Profile {
    /// Repeat every write once. Only `GSM` (LG UltraFine) is known to drop the first Set.
    pub(crate) duplicate_set: bool,
}

impl Profile {
    /// Compares the code trimmed and uppercased, as the Windows `DdcTimingProfile` does.
    pub(crate) fn for_manufacturer(manufacturer: &str) -> Profile {
        Profile {
            duplicate_set: manufacturer.trim().to_uppercase() == "GSM",
        }
    }
}

/// One DDC primitive that the shell runs on the wire.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum Primitive {
    Write { code: u8, value: i64 },
    Read { code: u8 },
}

/// The operation the shell asked for.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum OpKind {
    Read { code: u8 },
    SetVerify { code: u8, target: i64 },
}

/// What the driver does next. `Wait` carries an absolute time, so a late timer only delays work.
#[derive(Debug, Clone, PartialEq)]
pub(crate) enum Step {
    Run(Primitive),
    Wait(f64),
    Done(Outcome),
}

/// Final result of one operation. `error` is one of "io", "unsupported" or "disconnected".
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Outcome {
    pub(crate) ok: bool,
    pub(crate) error: Option<&'static str>,
    pub(crate) value: Option<f64>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum Phase {
    Idle,
    /// A primitive was handed to the shell; `attempts` counts it, so it is at least 1.
    Attempting {
        primitive: Primitive,
        attempts: u32,
    },
    /// A timer is armed; `on_timer` runs `next` as attempt `attempts + 1`.
    Waiting {
        next: Primitive,
        attempts: u32,
    },
    Finished,
}

/// One DDC operation: a read, or a write verified by a readback. Ports
/// `DdcTransaction::readVcp` (through `readWithRetry`) and `DdcTransaction::setAndVerify` from
/// `native/windows/Platform/DdcTransaction.cpp`; the macOS `DDCTransaction.swift` has the same
/// budget. One deliberate change: a transient write failure is retried, where the C++ gives up.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Operation {
    kind: OpKind,
    profile: Profile,
    phase: Phase,
    /// Writes that succeeded so far; the duplicate write follows only the first one.
    writes_done: u32,
}

impl Operation {
    pub(crate) fn new(kind: OpKind, profile: Profile) -> Self {
        Self {
            kind,
            profile,
            phase: Phase::Idle,
            writes_done: 0,
        }
    }

    pub(crate) fn start(&mut self, _now: f64) -> Step {
        if self.phase != Phase::Idle {
            return self.misuse();
        }
        let first = match self.kind {
            OpKind::Read { code } => Primitive::Read { code },
            OpKind::SetVerify { code, target } => Primitive::Write {
                code,
                value: target,
            },
        };
        self.run(first, 1)
    }

    pub(crate) fn on_attempt_done(
        &mut self,
        now: f64,
        ok: bool,
        error: AttemptError,
        value: Option<f64>,
    ) -> Step {
        let phase = self.phase;
        let (primitive, attempts) = match phase {
            Phase::Attempting {
                primitive,
                attempts,
            } => (primitive, attempts),
            _ => return self.misuse(),
        };
        // A write only submits the packet, so its own success carries no value. A read must
        // carry one; a successful read without one counts as a transient failure.
        let outcome = match (primitive, ok, value) {
            (Primitive::Write { .. }, true, _) => Ok(None),
            (Primitive::Write { .. }, false, _) => Err(error),
            (Primitive::Read { .. }, true, Some(reading)) => Ok(Some(reading)),
            (Primitive::Read { .. }, true, None) => Err(AttemptError::Io),
            (Primitive::Read { .. }, false, _) => Err(error),
        };
        match outcome {
            Ok(reading) => self.on_success(now, primitive, reading),
            Err(failed) => self.on_failure(now, primitive, attempts, failed),
        }
    }

    pub(crate) fn on_timer(&mut self, _now: f64) -> Step {
        let phase = self.phase;
        let (next, attempts) = match phase {
            Phase::Waiting { next, attempts } => (next, attempts),
            _ => return self.misuse(),
        };
        self.run(next, attempts + 1)
    }

    fn on_success(&mut self, now: f64, primitive: Primitive, reading: Option<f64>) -> Step {
        match (self.kind, primitive) {
            (OpKind::Read { .. }, Primitive::Read { .. }) => self.finish(Outcome {
                ok: true,
                error: None,
                value: reading,
            }),
            (OpKind::SetVerify { code, target }, Primitive::Write { .. }) => {
                self.writes_done += 1;
                // The settle wait belongs after the last write only, so a duplicate write
                // follows the short gap instead.
                if self.writes_done == 1 && self.profile.duplicate_set {
                    let second = Primitive::Write {
                        code,
                        value: target,
                    };
                    self.wait(second, 0, now + DUPLICATE_GAP_SECS)
                } else {
                    self.wait(Primitive::Read { code }, 0, now + SETTLE_SECS)
                }
            }
            (OpKind::SetVerify { target, .. }, Primitive::Read { .. }) => match reading {
                Some(readback) => {
                    // A readback mismatch is final and never retried.
                    let matches = readback == target as f64;
                    self.finish(Outcome {
                        ok: matches,
                        error: if matches { None } else { Some("io") },
                        value: Some(readback),
                    })
                }
                None => self.misuse(),
            },
            _ => self.misuse(),
        }
    }

    fn on_failure(
        &mut self,
        now: f64,
        primitive: Primitive,
        attempts: u32,
        error: AttemptError,
    ) -> Step {
        // Permanent errors cannot change on retry, so they end the primitive at once.
        let permanent = matches!(
            error,
            AttemptError::Unsupported | AttemptError::Disconnected
        );
        if !permanent && attempts < ATTEMPTS_PER_PRIMITIVE {
            // `attempts` is at least 1 here and below 3, so the index is 0 or 1.
            let backoff = BACKOFF_SECS[(attempts - 1) as usize];
            return self.wait(primitive, attempts, now + backoff);
        }
        self.finish(Outcome {
            ok: false,
            error: Some(error_name(error)),
            value: None,
        })
    }

    fn run(&mut self, primitive: Primitive, attempts: u32) -> Step {
        self.phase = Phase::Attempting {
            primitive,
            attempts,
        };
        Step::Run(primitive)
    }

    fn wait(&mut self, next: Primitive, attempts: u32, until: f64) -> Step {
        self.phase = Phase::Waiting { next, attempts };
        Step::Wait(until)
    }

    fn finish(&mut self, outcome: Outcome) -> Step {
        self.phase = Phase::Finished;
        Step::Done(outcome)
    }

    /// A call in the wrong phase fails the operation instead of panicking.
    fn misuse(&mut self) -> Step {
        self.finish(Outcome {
            ok: false,
            error: Some("io"),
            value: None,
        })
    }
}

/// Maps a failure to the name the contracts use. A failure without a known cause is io.
fn error_name(error: AttemptError) -> &'static str {
    match error {
        AttemptError::Unsupported => "unsupported",
        AttemptError::Disconnected => "disconnected",
        AttemptError::None | AttemptError::Io => "io",
    }
}

#[cfg(test)]
mod tests;
