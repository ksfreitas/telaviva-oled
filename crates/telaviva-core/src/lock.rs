//! Lock-on-absence stage. Pure state: the core feeds it facts, it never performs OS calls.

// Facts the core observed for one evaluation.
#[derive(Debug, Clone, Copy)]
pub struct LockFacts {
    pub enabled: bool,
    pub dimmed: bool,
    pub session_active: bool,
    pub camera_face: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LockStage {
    Idle,
    Armed { since_ms: u64 },
    Requested { token: u64 },
    Locked { token: u64, seen_inactive: bool },
    Failed { token: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LockState {
    stage: LockStage,
    next_token: u64,
}

impl Default for LockState {
    fn default() -> Self {
        Self::new()
    }
}

impl LockState {
    pub fn new() -> Self {
        Self {
            stage: LockStage::Idle,
            next_token: 1,
        }
    }

    pub fn stage(&self) -> LockStage {
        self.stage
    }

    pub fn name(&self) -> &'static str {
        match self.stage {
            LockStage::Idle => "idle",
            LockStage::Armed { .. } => "armed",
            LockStage::Requested { .. } => "requested",
            LockStage::Locked { .. } => "locked",
            LockStage::Failed { .. } => "failed",
        }
    }

    pub fn requested(&self) -> bool {
        matches!(self.stage, LockStage::Requested { .. })
    }

    pub fn token(&self) -> Option<u64> {
        match self.stage {
            LockStage::Requested { token }
            | LockStage::Locked { token, .. }
            | LockStage::Failed { token } => Some(token),
            LockStage::Idle | LockStage::Armed { .. } => None,
        }
    }

    // True while the session is locked by this stage and is not active, so the camera is held.
    pub fn holds_camera(&self, session_active: bool) -> bool {
        matches!(self.stage, LockStage::Locked { .. }) && !session_active
    }

    // Recomputes the stage from the facts. Returns true when the stage value changed.
    pub fn sync(&mut self, facts: LockFacts) -> bool {
        let before = self.stage;
        let c = condition(facts);
        self.stage = match before {
            LockStage::Idle => LockStage::Idle,
            LockStage::Armed { .. } | LockStage::Requested { .. } | LockStage::Failed { .. } => {
                if c {
                    before
                } else {
                    LockStage::Idle
                }
            }
            LockStage::Locked {
                token,
                seen_inactive,
            } => {
                // Locked ignores camera_face and the session state, except to note that the
                // session went inactive and then active again.
                let seen_inactive = seen_inactive || !facts.session_active;
                if !facts.enabled || !facts.dimmed || (seen_inactive && facts.session_active) {
                    LockStage::Idle
                } else {
                    LockStage::Locked {
                        token,
                        seen_inactive,
                    }
                }
            }
        };
        self.stage != before
    }

    // Call after an absent sample was applied. Returns true when the stage value changed.
    pub fn observe_absent(&mut self, captured_at_ms: u64, delay_ms: u64, facts: LockFacts) -> bool {
        let before = self.stage;
        self.sync(facts);
        let stage = self.stage;
        let c = condition(facts);
        self.stage = match stage {
            LockStage::Idle if c => LockStage::Armed {
                since_ms: captured_at_ms,
            },
            LockStage::Armed { since_ms }
                if c && delay_elapsed(since_ms, captured_at_ms, delay_ms) =>
            {
                let token = self.next_token;
                self.next_token += 1;
                LockStage::Requested { token }
            }
            other => other,
        };
        self.stage != before
    }

    // Applies a lock result. Only the request with the current token is accepted.
    pub fn result(&mut self, token: u64, ok: bool) -> bool {
        let stage = self.stage;
        match stage {
            LockStage::Requested { token: current } if current == token => {
                self.stage = if ok {
                    LockStage::Locked {
                        token,
                        seen_inactive: false,
                    }
                } else {
                    LockStage::Failed { token }
                };
                true
            }
            _ => false,
        }
    }
}

// The lock condition C: the lock is allowed only while the presence dim is on and the camera
// sees a face in an active session.
fn condition(facts: LockFacts) -> bool {
    facts.enabled && facts.dimmed && facts.session_active && facts.camera_face
}

// True once the confirming sample is at least delay_ms after the arming sample.
fn delay_elapsed(since_ms: u64, captured_at_ms: u64, delay_ms: u64) -> bool {
    captured_at_ms > since_ms && captured_at_ms.saturating_sub(since_ms) >= delay_ms
}

#[cfg(test)]
mod tests {
    use super::*;

    fn all_on() -> LockFacts {
        LockFacts {
            enabled: true,
            dimmed: true,
            session_active: true,
            camera_face: true,
        }
    }

    // Each entry turns exactly one part of the condition false.
    fn each_break() -> [LockFacts; 4] {
        let mut out = [all_on(); 4];
        out[0].enabled = false;
        out[1].dimmed = false;
        out[2].session_active = false;
        out[3].camera_face = false;
        out
    }

    // Armed at 1000 ms, with a 5000 ms delay.
    fn armed(facts: LockFacts) -> LockState {
        let mut s = LockState::new();
        assert!(s.observe_absent(1000, 5000, facts));
        s
    }

    // Requested with token 1 at 6000 ms.
    fn requested(facts: LockFacts) -> LockState {
        let mut s = armed(facts);
        assert!(s.observe_absent(6000, 5000, facts));
        s
    }

    fn locked(facts: LockFacts) -> LockState {
        let mut s = requested(facts);
        assert!(s.result(1, true));
        s
    }

    fn failed(facts: LockFacts) -> LockState {
        let mut s = requested(facts);
        assert!(s.result(1, false));
        s
    }

    #[test]
    fn names_follow_stage() {
        let on = all_on();
        assert_eq!(LockState::default().stage(), LockStage::Idle);
        assert_eq!(LockState::new().name(), "idle");
        assert_eq!(armed(on).name(), "armed");
        assert_eq!(requested(on).name(), "requested");
        assert_eq!(locked(on).name(), "locked");
        assert_eq!(failed(on).name(), "failed");
    }

    #[test]
    fn arms_only_when_condition_holds() {
        for facts in each_break() {
            let mut s = LockState::new();
            assert!(!s.observe_absent(1000, 5000, facts));
            assert_eq!(s.stage(), LockStage::Idle);
        }
        assert_eq!(armed(all_on()).stage(), LockStage::Armed { since_ms: 1000 });
    }

    #[test]
    fn no_request_before_delay() {
        let on = all_on();
        let mut s = armed(on);
        assert!(!s.observe_absent(1000, 5000, on));
        assert!(!s.observe_absent(5999, 5000, on));
        assert_eq!(s.stage(), LockStage::Armed { since_ms: 1000 });
        assert!(!s.requested());
        assert_eq!(s.token(), None);
    }

    #[test]
    fn requests_exactly_at_delay_with_increasing_tokens() {
        let on = all_on();
        let mut s = requested(on);
        assert_eq!(s.stage(), LockStage::Requested { token: 1 });
        assert!(s.requested());
        assert_eq!(s.token(), Some(1));

        // A sample without a face ends the episode; the next episode takes the next token.
        let mut no_face = all_on();
        no_face.camera_face = false;
        assert!(s.observe_absent(7000, 5000, no_face));
        assert_eq!(s.stage(), LockStage::Idle);
        assert!(s.observe_absent(8000, 5000, on));
        assert!(!s.observe_absent(12999, 5000, on));
        assert!(s.observe_absent(13000, 5000, on));
        assert_eq!(s.stage(), LockStage::Requested { token: 2 });
    }

    #[test]
    fn arm_on_dim_then_request_after_delay() {
        let on = all_on();
        let mut s = LockState::new();
        // The absent sample that produced the dim arms the stage.
        assert!(s.observe_absent(2000, 3000, on));
        assert_eq!(s.name(), "armed");
        assert!(!s.observe_absent(4999, 3000, on));
        assert_eq!(s.name(), "armed");
        assert!(s.observe_absent(5000, 3000, on));
        assert_eq!(s.name(), "requested");
        assert_eq!(s.token(), Some(1));
    }

    #[test]
    fn armed_requested_and_failed_return_to_idle_when_condition_breaks() {
        for off in each_break() {
            let mut s = armed(all_on());
            assert!(s.sync(off));
            assert_eq!(s.stage(), LockStage::Idle);

            let mut s = requested(all_on());
            assert!(s.sync(off));
            assert_eq!(s.stage(), LockStage::Idle);

            let mut s = failed(all_on());
            assert!(s.sync(off));
            assert_eq!(s.stage(), LockStage::Idle);
        }
    }

    #[test]
    fn locked_stays_while_session_inactive_or_face_missing() {
        let on = all_on();
        let mut inactive = all_on();
        inactive.session_active = false;
        let mut no_face = all_on();
        no_face.camera_face = false;

        let mut s = locked(on);
        assert!(!s.sync(on));
        assert_eq!(s.name(), "locked");

        s.sync(inactive);
        assert_eq!(s.name(), "locked");
        assert_eq!(s.token(), Some(1));

        // A missing face does not end a locked episode, even with the session inactive.
        let mut inactive_no_face = inactive;
        inactive_no_face.camera_face = false;
        s.sync(inactive_no_face);
        assert_eq!(s.name(), "locked");

        let mut s = locked(on);
        assert!(!s.sync(no_face));
        assert_eq!(s.name(), "locked");
    }

    #[test]
    fn locked_leaves_when_session_active_again_after_inactive() {
        let on = all_on();
        let mut inactive = all_on();
        inactive.session_active = false;
        let mut s = locked(on);
        s.sync(inactive);
        assert!(s.sync(on));
        assert_eq!(s.stage(), LockStage::Idle);
    }

    #[test]
    fn locked_leaves_when_dimmed_or_disabled() {
        let on = all_on();
        let mut undimmed = all_on();
        undimmed.dimmed = false;
        let mut s = locked(on);
        assert!(s.sync(undimmed));
        assert_eq!(s.stage(), LockStage::Idle);

        let mut disabled = all_on();
        disabled.enabled = false;
        let mut s = locked(on);
        assert!(s.sync(disabled));
        assert_eq!(s.stage(), LockStage::Idle);
    }

    #[test]
    fn result_moves_requested_to_locked_or_failed() {
        let on = all_on();
        let mut s = requested(on);
        assert!(s.result(1, true));
        assert_eq!(s.name(), "locked");
        assert_eq!(s.token(), Some(1));

        let mut s = requested(on);
        assert!(s.result(1, false));
        assert_eq!(s.stage(), LockStage::Failed { token: 1 });
    }

    #[test]
    fn result_ignores_wrong_token_or_stage() {
        let on = all_on();
        let mut s = requested(on);
        assert!(!s.result(2, true));
        assert!(!s.result(2, false));
        assert_eq!(s.stage(), LockStage::Requested { token: 1 });

        let mut s = LockState::new();
        assert!(!s.result(1, true));
        assert_eq!(s.name(), "idle");

        let mut s = armed(on);
        assert!(!s.result(1, false));
        assert_eq!(s.stage(), LockStage::Armed { since_ms: 1000 });

        let mut s = locked(on);
        assert!(!s.result(1, false));
        assert_eq!(s.name(), "locked");

        let mut s = failed(on);
        assert!(!s.result(1, true));
        assert_eq!(s.stage(), LockStage::Failed { token: 1 });
    }

    #[test]
    fn holds_camera_only_when_locked_and_session_inactive() {
        let on = all_on();
        assert!(locked(on).holds_camera(false));
        assert!(!locked(on).holds_camera(true));
        assert!(!LockState::new().holds_camera(false));
        assert!(!armed(on).holds_camera(false));
        assert!(!requested(on).holds_camera(false));
        assert!(!failed(on).holds_camera(false));
    }

    #[test]
    fn failed_does_not_request_again_until_idle() {
        let on = all_on();
        let mut no_face = all_on();
        no_face.camera_face = false;

        let mut s = failed(on);
        assert!(!s.observe_absent(10_000, 5000, on));
        assert!(!s.observe_absent(20_000, 5000, on));
        assert_eq!(s.stage(), LockStage::Failed { token: 1 });

        assert!(s.observe_absent(30_000, 5000, no_face));
        assert_eq!(s.name(), "idle");
        assert!(s.observe_absent(31_000, 5000, on));
        assert!(s.observe_absent(36_000, 5000, on));
        assert_eq!(s.stage(), LockStage::Requested { token: 2 });
    }
}
