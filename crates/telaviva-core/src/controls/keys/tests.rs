//! Ported from native/windows/Tests/key_intent_router.cpp and volume_action_tracker.cpp.
//! Cases whose C++ premise has no Rust counterpart are adapted or trimmed, as noted at each case.

use super::test_support::FakeDisplays;
use super::*;
use crate::controls::model::KeyKind::*;
use crate::controls::model::*;

/// Display state of a volume channel that is supported and awake, reading `percent`.
fn ready(percent: f64) -> DisplayState {
    DisplayState {
        percent,
        supported: true,
        awake: true,
        pending: false,
        failed: false,
    }
}

/// The keys under test, a fake display, the clock and the owner that the shell resolves.
struct Rig {
    keys: Keys,
    displays: FakeDisplays,
    now: f64,
    generation: u64,
    owner: Option<String>,
}

impl Rig {
    fn new() -> Self {
        Self {
            keys: Keys::new(),
            displays: FakeDisplays::default(),
            now: 1.0,
            generation: 1,
            owner: None,
        }
    }

    /// Configures both key steps to `step`.
    fn configure(&mut self, enabled: bool, step: f64) {
        self.keys.configure(enabled, step, step);
    }

    /// Sets the volume owner. The key owner carries the generation of the last observed reading.
    fn set_owner(&mut self, monitor: Option<&str>) {
        self.owner = monitor.map(str::to_owned);
        self.keys.set_volume_owner(monitor);
    }

    /// Sets the volume display of `monitor`. The latest observation replaces any older generation.
    fn set_display(&mut self, monitor: &str, generation: u64, state: DisplayState) {
        self.displays
            .set(monitor, generation, Feature::Volume, state);
    }

    fn observe(&mut self) {
        self.keys.observe(self.now, &self.displays);
    }

    /// Reports a volume reading and reconciles, as the shell does on every observation.
    fn observe_volume(&mut self, monitor: &str, generation: u64, percent: f64) {
        self.generation = generation;
        self.set_display(monitor, generation, ready(percent));
        self.observe();
    }

    fn owner_key(&self) -> Option<KeyOwner> {
        self.owner.as_ref().map(|monitor_id| KeyOwner {
            monitor_id: monitor_id.clone(),
            generation: self.generation,
        })
    }

    fn send(&mut self, event: KeyEvent) -> bool {
        let owner = self.owner_key();
        self.keys
            .handle_key(self.now, &event, owner.as_ref(), &self.displays)
    }

    fn press(&mut self, key: KeyKind, repeat: bool, fine: bool) -> bool {
        self.send(ev(key, KeyPhase::Down, repeat, fine, true))
    }

    fn release(&mut self, key: KeyKind) -> bool {
        self.send(ev(key, KeyPhase::Up, false, false, true))
    }

    fn take(&mut self) -> Vec<KeyIntent> {
        self.keys.take_intents()
    }

    fn finish(&mut self, intent: &KeyIntent, accepted: bool) {
        self.keys
            .intent_finished(self.now, &self.displays, intent, accepted);
    }

    fn advance_ms(&mut self, ms: i64) {
        self.now += ms as f64 / 1000.0;
    }

    fn manual_changed(&mut self, monitor: &str, generation: u64, value: f64) {
        self.keys.manual_control_changed(
            &self.displays,
            monitor,
            generation,
            Feature::Volume,
            value,
        );
    }

    fn manual_accepted(&mut self, monitor: &str, generation: u64, value: f64) {
        self.keys.manual_control_accepted(
            &self.displays,
            monitor,
            generation,
            Feature::Volume,
            value,
        );
    }

    fn accepted(&mut self, monitor: &str, generation: u64, action: u64) {
        self.keys
            .action_accepted(self.now, monitor, generation, Feature::Volume, action);
    }

    fn completed(&mut self, monitor: &str, generation: u64, action: u64) {
        self.keys
            .action_completed(self.now, monitor, generation, Feature::Volume, action);
    }

    fn shutdown(&mut self) {
        self.keys.shutdown();
    }
}

fn ev(key: KeyKind, phase: KeyPhase, repeat: bool, fine: bool, eligible: bool) -> KeyEvent {
    KeyEvent {
        key,
        phase,
        is_repeat: repeat,
        fine,
        modifiers_eligible: eligible,
    }
}

/// A rig with the default step, "m1" as the owner, and "m1" reading `percent`.
fn owned_rig(percent: f64) -> Rig {
    let mut rig = Rig::new();
    rig.configure(true, KEY_DEFAULT_STEP);
    rig.set_owner(Some("m1"));
    rig.observe_volume("m1", 1, percent);
    rig
}

/// A rig that owns "m1" at 50 and has muted it, with the mute write finished.
fn muted_rig() -> Rig {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(Mute, false, false));
    let muted = take_one(&mut rig);
    assert!(rig.release(Mute));
    rig.finish(&muted, true);
    rig
}

/// Takes the one intent handed out, failing if there is not exactly one.
fn take_one(rig: &mut Rig) -> KeyIntent {
    let mut intents = rig.take();
    assert_eq!(intents.len(), 1);
    intents.remove(0)
}

/// Down, repeat and release of one key; each reports whether it was consumed.
fn hold(rig: &mut Rig, key: KeyKind) -> [bool; 3] {
    [
        rig.press(key, false, false),
        rig.press(key, true, false),
        rig.release(key),
    ]
}

fn any(consumed: [bool; 3]) -> bool {
    consumed.contains(&true)
}

/// One press on a fresh rig that owns "m1" at `percent`. Returns the target handed out, if any.
fn press_once(key: KeyKind, percent: f64, step: f64, fine: bool) -> Option<f64> {
    let mut rig = Rig::new();
    rig.configure(true, step);
    rig.set_owner(Some("m1"));
    rig.observe_volume("m1", 1, percent);
    assert!(rig.press(key, false, fine));
    let intents = rig.take();
    match intents.as_slice() {
        [intent] => Some(intent.target),
        _ => None,
    }
}

/// Holds `key` on a fresh rig with `owner`, where `display` is reported for `monitor`.
/// Asserts that a write is handed out only when `expect_write` is set.
fn hold_fresh(
    owner: Option<&str>,
    monitor: &str,
    display: DisplayState,
    key: KeyKind,
    expect_write: bool,
) -> [bool; 3] {
    let mut rig = Rig::new();
    rig.configure(true, KEY_DEFAULT_STEP);
    rig.set_owner(owner);
    rig.set_display(monitor, 1, display);
    rig.observe();
    let consumed = hold(&mut rig, key);
    assert_eq!(!rig.take().is_empty(), expect_write);
    consumed
}

#[test]
fn keys_key_step_policy() {
    assert_eq!(stepped(50.0, true, false, 5.0), 55.0);
    assert_eq!(stepped(50.0, false, false, 5.0), 45.0);
    assert_eq!(stepped(50.0, true, false, 15.0), 65.0);
    assert_eq!(stepped(50.0, false, false, 1.0), 49.0);
    assert_eq!(stepped(50.0, true, true, 15.0), 51.0);
    assert_eq!(stepped(50.0, false, true, 15.0), 49.0);
    assert_eq!(stepped(50.0, false, true, 1.0), 49.0);
    assert_eq!(stepped(95.0, true, false, 15.0), 100.0);
    assert_eq!(stepped(5.0, false, false, 15.0), 0.0);
    assert_eq!(stepped(100.0, true, true, 15.0), 100.0);
    assert_eq!(KEY_DEFAULT_STEP, 5.0);
    assert_eq!(KEY_MIN_STEP, 1.0);
    assert_eq!(KEY_MAX_STEP, 15.0);
    assert_eq!(KEY_FINE_STEP, 1.0);
    assert_eq!(clamped_step(0.0), 1.0);
    assert_eq!(clamped_step(99.0), 15.0);
    assert_eq!(clamped_step(f64::NAN), 5.0);
    assert_eq!(clamped_step(f64::INFINITY), 5.0);
    assert_eq!(clamped_step(7.4), 7.0);
    assert_eq!(clamped_step(7.6), 8.0);
}

#[test]
fn keys_configured_step_bounds_apply_to_presses() {
    assert_eq!(press_once(VolumeDown, 50.0, 0.0, false), Some(49.0));
    assert_eq!(press_once(VolumeDown, 50.0, 99.0, false), Some(35.0));
    assert_eq!(press_once(VolumeDown, 50.0, f64::NAN, false), Some(45.0));
    assert_eq!(press_once(VolumeDown, 50.0, 7.4, false), Some(43.0));
    assert_eq!(press_once(VolumeUp, 50.0, 15.0, false), Some(65.0));
    // Shift+Option moves exactly one point whatever the configured step is.
    assert_eq!(press_once(VolumeUp, 50.0, 15.0, true), Some(51.0));
    assert_eq!(press_once(VolumeDown, 50.0, 15.0, true), Some(49.0));
}

#[test]
fn keys_volume_boundaries_consume_without_write() {
    assert_eq!(press_once(VolumeUp, 100.0, 15.0, false), None);
    assert_eq!(press_once(VolumeDown, 0.0, 15.0, false), None);
    assert_eq!(press_once(VolumeUp, 95.0, 15.0, false), Some(100.0));
    assert_eq!(press_once(VolumeDown, 5.0, 15.0, false), Some(0.0));

    let mut rig = owned_rig(100.0);
    assert_eq!(hold(&mut rig, VolumeUp), [true; 3]);
    assert!(rig.take().is_empty());
}

#[test]
fn keys_owner_selects_only_an_eligible_observation() {
    let unsupported = DisplayState {
        supported: false,
        ..ready(50.0)
    };
    let asleep = DisplayState {
        awake: false,
        ..ready(50.0)
    };
    assert!(!hold_fresh(None, "m1", ready(50.0), VolumeUp, false)[0]);
    assert!(!hold_fresh(Some("m1"), "m2", ready(50.0), VolumeUp, false)[0]);
    assert!(!hold_fresh(Some("m1"), "m1", unsupported, VolumeUp, false)[0]);
    assert!(!hold_fresh(Some("m1"), "m1", asleep, VolumeUp, false)[0]);
    assert!(!hold_fresh(Some("m1"), "m1", ready(f64::NAN), VolumeUp, false)[0]);
    assert!(!hold_fresh(Some("m1"), "m1", ready(101.0), VolumeUp, false)[0]);
    assert!(!hold_fresh(Some("m1"), "m1", ready(-1.0), VolumeUp, false)[0]);
    assert!(hold_fresh(Some("m1"), "m1", ready(50.0), VolumeUp, true)[0]);
}

#[test]
fn keys_no_owner_passes_whole_sequence_through() {
    for key in [VolumeUp, VolumeDown, Mute] {
        assert!(!any(hold_fresh(None, "m1", ready(50.0), key, false)));
    }
}

#[test]
fn keys_held_key_coalesces_repeats_into_one_follow_up_write() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    assert_eq!(first.monitor_id, "m1");
    assert_eq!(first.target, 55.0);
    assert_eq!(first.generation, 1);
    assert!(rig.press(VolumeUp, true, false));
    assert!(rig.press(VolumeUp, true, false));
    // The release has ineligible modifiers, but the latched press stays consumed.
    assert!(rig.send(ev(VolumeUp, KeyPhase::Up, false, false, false)));
    assert!(rig.take().is_empty());
    rig.finish(&first, true);
    let second = take_one(&mut rig);
    assert_eq!(second.target, 65.0);
    rig.finish(&second, true);
    assert!(rig.take().is_empty());
}

#[test]
fn keys_latch_decides_whole_sequence_per_key() {
    let mut rig = owned_rig(50.0);
    // An ineligible press passes through, and so do its repeat and release.
    assert!(!rig.send(ev(VolumeDown, KeyPhase::Down, false, false, false)));
    assert!(!rig.send(ev(VolumeDown, KeyPhase::Down, true, false, true)));
    assert!(!rig.release(VolumeDown));
    assert!(rig.take().is_empty());
    // A handled press stays consumed even when its release has ineligible modifiers.
    assert!(rig.press(VolumeUp, false, false));
    assert!(rig.send(ev(VolumeUp, KeyPhase::Up, false, false, false)));
    assert_eq!(take_one(&mut rig).target, 55.0);
    // A release without a press passes through.
    assert!(!rig.release(VolumeUp));
}

#[test]
fn keys_orphan_repeat_passes_through() {
    let mut rig = owned_rig(50.0);
    assert!(!rig.press(VolumeUp, true, false));
    assert!(rig.take().is_empty());
}

#[test]
fn keys_held_hold_is_cancelled_when_owner_leaves_mid_hold() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    assert_eq!(first.target, 55.0);
    // The sound output moved to another device while the key is held.
    rig.set_owner(None);
    assert!(rig.press(VolumeUp, true, false));
    assert!(rig.release(VolumeUp));
    rig.finish(&first, true);
    assert!(rig.take().is_empty());
    assert!(!rig.press(VolumeUp, false, false));
    assert!(!rig.release(VolumeUp));

    // A press that passed through stays passed through even if the output returns mid-hold.
    let mut late = owned_rig(50.0);
    late.set_owner(None);
    assert!(!late.press(VolumeUp, false, false));
    late.set_owner(Some("m1"));
    assert!(!late.press(VolumeUp, true, false));
    assert!(!late.release(VolumeUp));
    assert!(late.press(VolumeUp, false, false));
    assert_eq!(late.take().len(), 1);
}

#[test]
fn keys_stale_generation_cancels_hold() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    // The monitor reconnected with a new generation.
    rig.observe_volume("m1", 2, 50.0);
    assert!(rig.press(VolumeUp, true, false));
    assert!(rig.release(VolumeUp));
    rig.finish(&first, true);
    assert!(rig.take().is_empty());
}

#[test]
fn keys_rejected_submission_leaves_release_consumed() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.finish(&first, false);
    // The C++ isIntentCurrent check has no Rust counterpart, so it is dropped.
    assert!(rig.release(VolumeUp));
    assert!(rig.take().is_empty());
    // The next press starts again from the readback.
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 55.0);
}

#[test]
fn keys_manual_change_invalidates_handed_off_intent() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let old = take_one(&mut rig);
    // The C++ isIntentCurrent checks have no Rust counterpart, so they are dropped.
    rig.manual_changed("m1", 1, 20.0);
    rig.finish(&old, false);
    assert!(rig.take().is_empty());
    // The held key continues from the slider value.
    assert!(rig.press(VolumeUp, true, false));
    assert_eq!(take_one(&mut rig).target, 25.0);

    rig.shutdown();
    rig.configure(true, KEY_DEFAULT_STEP);
    rig.observe_volume("m1", 1, 50.0);
    assert!(!rig.press(VolumeDown, false, false));
}

#[test]
fn keys_manual_rebase_is_used_by_next_key() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    assert_eq!(first.target, 55.0);
    rig.finish(&first, true);
    assert!(rig.release(VolumeUp));

    rig.manual_changed("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, false, false));
    let next = take_one(&mut rig);
    assert_eq!(next.target, 25.0);
    rig.finish(&next, false);
}

#[test]
fn keys_late_slider_acceptance_cannot_rebase_newer_key_target() {
    // Adapted: without revisions, an acceptance whose value differs from the intent target is
    // the case that has no effect, as the key has already moved the target to 25.
    let mut rig = owned_rig(50.0);
    rig.manual_changed("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    assert_eq!(first.target, 25.0);
    rig.manual_accepted("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, true, false));
    rig.finish(&first, true);
    let second = take_one(&mut rig);
    assert_eq!(second.target, 30.0);
    rig.finish(&second, false);
}

#[test]
fn keys_rebasing_is_bounded_and_coalesces_superseded_epochs() {
    // Adapted: the C++ per-revision loop is a plain repeat, since revisions are gone.
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    for _ in 0..100 {
        rig.manual_changed("m1", 1, 40.0);
        assert!(rig.press(VolumeUp, true, false));
    }
    // One write stays in flight; nothing more is handed out while it runs.
    assert!(rig.take().is_empty());
    rig.finish(&first, false);
    let latest = take_one(&mut rig);
    assert_eq!(latest.target, 45.0);
    rig.finish(&latest, true);
    assert!(rig.take().is_empty());
}

#[test]
fn keys_external_readback_rebases_idle_intent() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.finish(&first, true);
    assert!(rig.release(VolumeUp));
    // The level changed elsewhere while idle, beyond the readback tolerance.
    rig.observe_volume("m1", 1, 56.0);
    rig.observe_volume("m1", 1, 30.0);
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 35.0);
}

#[test]
fn keys_readback_within_tolerance_does_not_rebase() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.finish(&first, true);
    rig.observe_volume("m1", 1, 55.4);
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 60.0);
}

#[test]
fn keys_step_change_applies_to_next_press_and_held_repeat() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    assert_eq!(first.target, 55.0);
    rig.finish(&first, true);
    assert!(rig.release(VolumeUp));

    rig.configure(true, 12.0);
    assert!(rig.press(VolumeUp, false, false));
    let second = take_one(&mut rig);
    assert_eq!(second.target, 67.0);

    // Still held while the step changes again: the repeat uses the current step.
    rig.configure(true, 3.0);
    assert!(rig.press(VolumeUp, true, false));
    assert!(rig.release(VolumeUp));
    assert!(rig.take().is_empty());
    rig.finish(&second, true);
    let third = take_one(&mut rig);
    assert_eq!(third.target, 70.0);
    rig.finish(&third, true);
}

#[test]
fn keys_own_write_outstanding_blocks_rebase() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.accepted("m1", 1, 7);
    rig.finish(&first, true);
    // The readback still shows the old level while our write is outstanding.
    rig.observe_volume("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, true, false));
    assert_eq!(take_one(&mut rig).target, 60.0);
}

#[test]
fn keys_settle_window_ignores_readback_then_external_change_rebases() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.accepted("m1", 1, 7);
    rig.finish(&first, true);
    rig.advance_ms(100);
    rig.completed("m1", 1, 7); // settle window until 2.1 s
    rig.advance_ms(400);
    rig.observe_volume("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, true, false));
    let second = take_one(&mut rig);
    assert_eq!(second.target, 60.0);
    rig.finish(&second, true);

    rig.advance_ms(700);
    rig.observe_volume("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 25.0);
}

#[test]
fn keys_unfinished_write_expires_after_ten_seconds() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.accepted("m1", 1, 7);
    rig.finish(&first, true);

    rig.advance_ms(9_999); // one millisecond before expiry
    rig.observe_volume("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, true, false));
    let second = take_one(&mut rig);
    assert_eq!(second.target, 60.0);
    rig.finish(&second, true);

    rig.advance_ms(2); // one millisecond after expiry
    rig.observe_volume("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 25.0);
}

#[test]
fn keys_completion_before_acceptance_cancels_it() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(VolumeUp, false, false));
    let first = take_one(&mut rig);
    rig.completed("m1", 1, 9);
    rig.accepted("m1", 1, 9); // cancels the early completion
    rig.finish(&first, true);

    rig.advance_ms(1100);
    rig.observe_volume("m1", 1, 20.0);
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 25.0);
}

#[test]
fn keys_mute_restore_uses_volume_step() {
    let mut rig = owned_rig(50.0);
    // The brightness step must not change the volume restore.
    rig.keys.configure(true, 1.0, 12.0);
    assert!(rig.press(Mute, false, false));
    let muted = take_one(&mut rig);
    assert_eq!(muted.target, 0.0);
    assert!(rig.release(Mute));
    rig.finish(&muted, true);

    rig.observe_volume("m1", 1, 0.0);
    assert!(rig.press(VolumeUp, false, false));
    assert_eq!(take_one(&mut rig).target, 62.0);
}

#[test]
fn keys_mute_writes_zero_once() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(Mute, false, false));
    let muted = take_one(&mut rig);
    assert_eq!(muted.target, 0.0);
    assert!(rig.press(Mute, true, false));
    assert!(rig.release(Mute));
    rig.finish(&muted, true);
    assert!(rig.take().is_empty());
}

#[test]
fn keys_mute_restores_remembered_level() {
    let mut rig = muted_rig();
    rig.observe_volume("m1", 1, 0.0);
    assert!(rig.press(Mute, false, false));
    let restored = take_one(&mut rig);
    assert_eq!(restored.target, 50.0);
    assert!(rig.release(Mute));
    rig.finish(&restored, true);
}

#[test]
fn keys_volume_up_while_muted_steps_from_remembered_level() {
    let mut rig = muted_rig();
    rig.observe_volume("m1", 1, 0.0);
    assert!(rig.press(VolumeUp, false, false));
    assert!(rig.release(VolumeUp));
    let unmuted = take_one(&mut rig);
    assert_eq!(unmuted.target, 55.0);
    rig.finish(&unmuted, true);
}

#[test]
fn keys_volume_down_while_muted_writes_nothing() {
    let mut rig = muted_rig();
    rig.observe_volume("m1", 1, 0.0);
    assert!(rig.press(VolumeDown, false, false));
    assert!(rig.release(VolumeDown));
    assert!(rig.take().is_empty());
}

#[test]
fn keys_mute_at_zero_with_nothing_remembered_writes_nothing() {
    let mut rig = owned_rig(0.0);
    assert!(rig.press(Mute, false, false));
    assert!(rig.release(Mute));
    assert!(rig.take().is_empty());
}

#[test]
fn keys_external_change_discards_remembered_mute_level() {
    let mut rig = muted_rig();
    rig.observe_volume("m1", 1, 30.0);
    assert!(rig.press(Mute, false, false));
    let rewritten = take_one(&mut rig);
    assert_eq!(rewritten.target, 0.0);
    assert!(rig.release(Mute));
    rig.finish(&rewritten, true);
}

#[test]
fn keys_output_round_trip_forgets_remembered_mute_level() {
    // Adapted: the C++ empty-owner route is set_owner(None), then the owner returns.
    let mut rig = muted_rig();
    rig.set_owner(None);
    rig.set_owner(Some("m1"));
    rig.observe_volume("m1", 1, 0.0);
    assert!(rig.press(Mute, false, false));
    assert!(rig.release(Mute));
    assert!(rig.take().is_empty());
}

#[test]
fn keys_mute_and_volume_pass_through_without_supported_owner() {
    let unsupported = DisplayState {
        supported: false,
        ..ready(50.0)
    };
    let asleep = DisplayState {
        awake: false,
        ..ready(50.0)
    };
    assert!(!any(hold_fresh(None, "m1", ready(50.0), Mute, false)));
    assert!(!any(hold_fresh(Some("m1"), "m1", unsupported, Mute, false)));
    assert!(!any(hold_fresh(Some("m1"), "m1", asleep, Mute, false)));
    assert!(!any(hold_fresh(
        Some("m1"),
        "m1",
        ready(f64::NAN),
        Mute,
        false
    )));
    assert!(!any(hold_fresh(
        Some("m1"),
        "m1",
        unsupported,
        VolumeUp,
        false
    )));
}

#[test]
fn keys_mute_with_disallowed_modifiers_passes_through() {
    let mut rig = owned_rig(50.0);
    assert!(!rig.send(ev(Mute, KeyPhase::Down, false, false, false)));
    assert!(!rig.send(ev(Mute, KeyPhase::Up, false, false, false)));
    assert!(rig.take().is_empty());
}

#[test]
fn keys_held_mute_cancelled_by_output_switch() {
    let mut rig = owned_rig(50.0);
    assert!(rig.press(Mute, false, false));
    let muted = take_one(&mut rig);
    rig.set_owner(None);
    assert!(rig.press(Mute, true, false));
    assert!(rig.release(Mute));
    rig.finish(&muted, true);
    assert!(rig.take().is_empty());
}

#[test]
fn keys_disable_and_shutdown_clear_owner() {
    let mut rig = owned_rig(50.0);
    rig.configure(false, KEY_DEFAULT_STEP);
    assert!(!rig.press(VolumeUp, false, false));
    assert!(!rig.release(VolumeUp));

    rig.shutdown();
    rig.configure(true, KEY_DEFAULT_STEP);
    rig.observe_volume("m1", 1, 50.0);
    assert!(!rig.press(VolumeUp, false, false));
    assert!(rig.take().is_empty());
}

/// A monitor with brightness 41 and volume 20, both on a 0..100 range.
fn monitor(id: &str, generation: u64) -> MonitorObs {
    let channel = |observed: f64| ChannelObs {
        supported: true,
        observed: Some(observed),
        min: 0.0,
        max: 100.0,
    };
    MonitorObs {
        id: id.to_owned(),
        generation,
        display_state: "awake".to_owned(),
        identity_status: "stable".to_owned(),
        brightness: channel(41.0),
        volume: channel(20.0),
    }
}

fn action(id: u64, generation: u64, monitor: &str, kind: &str) -> PendingActionObs {
    PendingActionObs {
        id,
        generation,
        monitor_id: monitor.to_owned(),
        kind: kind.to_owned(),
        value: Some(20.0),
    }
}

fn observation(
    revision: u64,
    monitors: Vec<MonitorObs>,
    pending_actions: Vec<PendingActionObs>,
) -> Observation {
    Observation {
        revision,
        monitors,
        pending_actions,
    }
}

#[test]
fn tracker_reports_new_action_once_with_both_generations() {
    let mut keys = Keys::new();
    let first = observation(
        1,
        vec![monitor("m1", 3)],
        vec![action(5, 9, "m1", "set_volume")],
    );
    assert_eq!(
        keys.report_accepted_actions(1.0, &first),
        vec![("m1".to_owned(), 3, Feature::Volume, 9)]
    );
    assert!(keys.report_accepted_actions(1.0, &first).is_empty());
    let second = observation(
        2,
        vec![monitor("m1", 3)],
        vec![action(5, 9, "m1", "set_volume")],
    );
    assert!(keys.report_accepted_actions(1.0, &second).is_empty());
}

#[test]
fn tracker_reports_each_monitor() {
    let mut keys = Keys::new();
    let snapshot = observation(
        1,
        vec![monitor("m1", 1), monitor("m2", 2)],
        vec![
            action(5, 4, "m1", "set_volume"),
            action(6, 4, "m2", "set_volume"),
        ],
    );
    assert_eq!(
        keys.report_accepted_actions(1.0, &snapshot),
        vec![
            ("m1".to_owned(), 1, Feature::Volume, 4),
            ("m2".to_owned(), 2, Feature::Volume, 4),
        ]
    );
}

#[test]
fn tracker_ignores_other_kinds_and_missing_monitors() {
    let mut keys = Keys::new();
    // Adapted: set_power stands in for an other kind, since set_brightness is now tracked too.
    let other = observation(
        1,
        vec![monitor("m1", 1)],
        vec![action(5, 1, "m1", "set_power")],
    );
    assert!(keys.report_accepted_actions(1.0, &other).is_empty());

    // A monitor missing from the snapshot is skipped, and its action is reported once it appears.
    let missing = observation(2, vec![], vec![action(6, 1, "m2", "set_volume")]);
    assert!(keys.report_accepted_actions(1.0, &missing).is_empty());
    let later = observation(
        3,
        vec![monitor("m2", 2)],
        vec![action(6, 1, "m2", "set_volume")],
    );
    assert_eq!(
        keys.report_accepted_actions(1.0, &later),
        vec![("m2".to_owned(), 2, Feature::Volume, 1)]
    );
}

#[test]
fn tracker_forgets_ids_that_are_no_longer_pending() {
    let mut keys = Keys::new();
    let pending = observation(
        1,
        vec![monitor("m1", 1)],
        vec![action(5, 1, "m1", "set_volume")],
    );
    assert_eq!(keys.report_accepted_actions(1.0, &pending).len(), 1);
    let cleared = observation(2, vec![monitor("m1", 1)], vec![]);
    assert!(keys.report_accepted_actions(1.0, &cleared).is_empty());
    // Once the core stops listing the id, it no longer occupies the tracker.
    assert_eq!(keys.report_accepted_actions(1.0, &pending).len(), 1);
}

#[test]
fn tracker_is_capped() {
    let mut keys = Keys::new();
    let ids: Vec<String> = (0..300).map(|index| format!("monitor-{index}")).collect();
    let monitors: Vec<MonitorObs> = ids.iter().map(|id| monitor(id, 1)).collect();
    let actions: Vec<PendingActionObs> = ids
        .iter()
        .enumerate()
        .map(|(index, id)| action(index as u64 + 1, 1, id, "set_volume"))
        .collect();
    let crowded = observation(1, monitors, actions);
    assert_eq!(
        keys.report_accepted_actions(1.0, &crowded).len(),
        TRACKER_MAX_REMEMBERED
    );
    // The cap holds while the same ids stay pending, so nothing is reported twice.
    assert!(keys.report_accepted_actions(1.0, &crowded).is_empty());
}

#[test]
fn tracker_also_reports_set_brightness() {
    let mut keys = Keys::new();
    let brightness = observation(
        1,
        vec![monitor("m1", 1)],
        vec![action(5, 1, "m1", "set_brightness")],
    );
    assert_eq!(
        keys.report_accepted_actions(1.0, &brightness),
        vec![("m1".to_owned(), 1, Feature::Brightness, 1)]
    );
}
