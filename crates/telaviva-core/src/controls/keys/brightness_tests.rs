//! Brightness-key cases and the core-applicable cases ported from the macOS key-router tests
//! (native/macos/Tests/HardwareKeyRouterTests.swift). Shell-side cases are not ported here.

use super::*;
use crate::controls::model::{clamped_step, Feature, KeyKind, KeyOwner, KeyPhase};

const MONITOR: &str = "monitor-a";

fn owner_a() -> KeyOwner {
    KeyOwner {
        monitor_id: MONITOR.to_string(),
        generation: 1,
    }
}

fn key_down(key: KeyKind) -> KeyEvent {
    KeyEvent {
        key,
        phase: KeyPhase::Down,
        is_repeat: false,
        fine: false,
        modifiers_eligible: true,
    }
}

fn key_repeat(key: KeyKind) -> KeyEvent {
    KeyEvent {
        is_repeat: true,
        ..key_down(key)
    }
}

fn key_up(key: KeyKind) -> KeyEvent {
    KeyEvent {
        phase: KeyPhase::Up,
        ..key_down(key)
    }
}

#[derive(Clone, Copy)]
struct Readback {
    percent: f64,
    supported: bool,
    awake: bool,
    pending: bool,
    failed: bool,
}

impl Readback {
    fn at(percent: f64) -> Readback {
        Readback {
            percent,
            supported: true,
            awake: true,
            pending: false,
            failed: false,
        }
    }

    fn to_state(self) -> DisplayState {
        DisplayState {
            percent: self.percent,
            supported: self.supported,
            awake: self.awake,
            pending: self.pending,
            failed: self.failed,
        }
    }
}

/// Readbacks of the single test monitor, keyed by connection generation and feature.
#[derive(Default)]
struct FakeDisplays {
    entries: Vec<(u64, Feature, Readback)>,
}

impl FakeDisplays {
    fn set(&mut self, generation: u64, feature: Feature, readback: Readback) {
        self.entries
            .retain(|entry| !(entry.0 == generation && entry.1 == feature));
        self.entries.push((generation, feature, readback));
    }

    fn remove_generation(&mut self, generation: u64) {
        self.entries.retain(|entry| entry.0 != generation);
    }
}

impl Displays for FakeDisplays {
    fn display(&self, monitor_id: &str, generation: u64, feature: Feature) -> Option<DisplayState> {
        if monitor_id != MONITOR {
            return None;
        }
        self.entries
            .iter()
            .find(|entry| entry.0 == generation && entry.1 == feature)
            .map(|entry| entry.2.to_state())
    }
}

struct Rig {
    keys: Keys,
    displays: FakeDisplays,
    /// Seconds, as the core clock reports them.
    now: f64,
}

impl Rig {
    fn new() -> Rig {
        Rig::with_step(KEY_DEFAULT_STEP)
    }

    /// Enabled routing with the monitor as the sound output owner and one step for both keys.
    fn with_step(step: f64) -> Rig {
        Rig::with_steps(step, step)
    }

    /// Enabled routing with the monitor as the sound output owner and separate key steps.
    fn with_steps(brightness_step: f64, volume_step: f64) -> Rig {
        let mut keys = Keys::new();
        keys.configure(true, brightness_step, volume_step);
        keys.set_volume_owner(Some(MONITOR));
        Rig {
            keys,
            displays: FakeDisplays::default(),
            now: 0.0,
        }
    }

    /// Sets the clock in milliseconds, the unit of the Swift tests and of the constants.
    fn at(&mut self, ms: u64) {
        self.now = ms as f64 / 1000.0;
    }

    fn set(&mut self, feature: Feature, readback: Readback) {
        self.displays.set(1, feature, readback);
    }

    fn send(&mut self, event: KeyEvent, owner: Option<&KeyOwner>) -> bool {
        self.keys
            .handle_key(self.now, &event, owner, &self.displays)
    }

    /// Key events routed to the monitor under test.
    fn press(&mut self, key: KeyKind) -> bool {
        let owner = owner_a();
        self.send(key_down(key), Some(&owner))
    }

    fn hold(&mut self, key: KeyKind) -> bool {
        let owner = owner_a();
        self.send(key_repeat(key), Some(&owner))
    }

    fn release(&mut self, key: KeyKind) -> bool {
        let owner = owner_a();
        self.send(key_up(key), Some(&owner))
    }

    fn observe(&mut self) {
        self.keys.observe(self.now, &self.displays);
    }

    fn take(&mut self) -> Vec<KeyIntent> {
        self.keys.take_intents()
    }

    /// Takes the ready intents; exactly one is expected.
    fn single(&mut self) -> KeyIntent {
        let mut taken = self.take();
        assert_eq!(taken.len(), 1, "expected exactly one intent");
        taken.remove(0)
    }

    fn finish(&mut self, intent: &KeyIntent, accepted: bool) {
        self.keys
            .intent_finished(self.now, &self.displays, intent, accepted);
    }

    fn manual(&mut self, feature: Feature, value: f64) {
        self.keys
            .manual_control_changed(&self.displays, MONITOR, 1, feature, value);
    }

    fn manual_accepted(&mut self, feature: Feature, value: f64) {
        self.keys
            .manual_control_accepted(&self.displays, MONITOR, 1, feature, value);
    }

    fn action_accepted(&mut self, feature: Feature, action_generation: u64) {
        self.keys
            .action_accepted(self.now, MONITOR, 1, feature, action_generation);
    }

    fn action_completed(&mut self, feature: Feature, action_generation: u64) {
        self.keys
            .action_completed(self.now, MONITOR, 1, feature, action_generation);
    }
}

/// One press on a fresh rig at the given step and readbacks. Returns the first written target,
/// or None when the press wrote nothing. The press itself must be consumed.
fn one_press(kind: KeyKind, step: f64, fine: bool, brightness: f64, volume: f64) -> Option<f64> {
    one_press_with_steps(kind, step, step, fine, brightness, volume)
}

/// Like `one_press`, with a separate brightness step and volume step.
fn one_press_with_steps(
    kind: KeyKind,
    brightness_step: f64,
    volume_step: f64,
    fine: bool,
    brightness: f64,
    volume: f64,
) -> Option<f64> {
    let mut rig = Rig::with_steps(brightness_step, volume_step);
    rig.set(Feature::Brightness, Readback::at(brightness));
    rig.set(Feature::Volume, Readback::at(volume));
    let owner = owner_a();
    let event = KeyEvent {
        fine,
        ..key_down(kind)
    };
    assert!(rig.send(event, Some(&owner)), "single press is consumed");
    let taken = rig.take();
    assert!(taken.len() <= 1, "one press writes at most one intent");
    taken.first().map(|intent| intent.target)
}

#[test]
fn brightness_keyboard_brightness_events() {
    // Keyboard brightness keys arrive as plain brightness up/down; the shell decodes them.
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "keyboard press is consumed"
    );
    let first = rig.single();
    assert_eq!(first.feature, Feature::Brightness);
    assert_eq!(first.target, 50.0 + KEY_DEFAULT_STEP);

    // Held keys coalesce their repeats while the first write is in flight.
    assert!(
        rig.hold(KeyKind::BrightnessUp),
        "keyboard autorepeat is consumed"
    );
    assert!(
        rig.release(KeyKind::BrightnessUp),
        "keyboard release is consumed"
    );
    assert!(rig.take().is_empty(), "no second command while in flight");
    rig.finish(&first, true);
    let second = rig.single();
    assert_eq!(second.target, 50.0 + 2.0 * KEY_DEFAULT_STEP);
    rig.finish(&second, true);

    // Outside every display the owner is None and the whole sequence passes through.
    let mut outside = Rig::new();
    outside.set(Feature::Brightness, Readback::at(50.0));
    assert!(!outside.send(key_down(KeyKind::BrightnessUp), None));
    assert!(!outside.send(key_repeat(KeyKind::BrightnessUp), None));
    assert!(!outside.send(key_up(KeyKind::BrightnessUp), None));
    assert!(
        outside.take().is_empty(),
        "pass-through keys submit nothing"
    );
}

#[test]
fn brightness_target_and_step_policy() {
    // Target selection by pointer (overlap, gaps, built-in, sleeping) is shell-side.
    assert_eq!(stepped(96.875, true, false, KEY_DEFAULT_STEP), 100.0);
    assert_eq!(stepped(0.0, false, true, KEY_DEFAULT_STEP), 0.0);
    assert_eq!(stepped(50.0, true, false, KEY_DEFAULT_STEP), 55.0);
    assert_eq!(stepped(50.0, false, true, KEY_DEFAULT_STEP), 49.0);
    assert_eq!(stepped(50.0, false, false, KEY_DEFAULT_STEP), 45.0);
}

#[test]
fn brightness_sequence_routing() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    let owner = owner_a();
    // The first press decides; its repeat follows that latched decision and steps the pending
    // target. The C++ router cancels a repeat that finds no owner, so the owner is kept here.
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    assert!(
        rig.send(key_repeat(KeyKind::BrightnessUp), Some(&owner)),
        "repeat follows the latched decision"
    );
    // A key-up whose modifiers changed is still consumed and adds no delta.
    let released = KeyEvent {
        modifiers_eligible: false,
        ..key_up(KeyKind::BrightnessUp)
    };
    assert!(
        rig.send(released, Some(&owner)),
        "handled key-up stays consumed"
    );
    assert!(rig.take().is_empty(), "key-up applies no delta");
    rig.finish(&first, true);
    let second = rig.single();
    assert_eq!(second.target, 60.0, "repeat increments the original intent");
    rig.finish(&second, true);

    // Disallowed modifiers pass the press, its repeat and its release through.
    let ineligible = KeyEvent {
        modifiers_eligible: false,
        ..key_down(KeyKind::BrightnessUp)
    };
    let owner = owner_a();
    assert!(
        !rig.send(ineligible, Some(&owner)),
        "ineligible modifiers pass through"
    );
    assert!(!rig.send(key_repeat(KeyKind::BrightnessUp), Some(&owner)));
    assert!(!rig.send(key_up(KeyKind::BrightnessUp), Some(&owner)));

    // An unowned press passes through; a later owned press starts a new latch.
    assert!(
        !rig.send(key_down(KeyKind::BrightnessUp), None),
        "no owner passes through"
    );
    assert!(
        !rig.send(key_up(KeyKind::BrightnessUp), None),
        "unhandled key-up passes"
    );
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "fresh press starts a new latch"
    );
    assert!(rig.release(KeyKind::BrightnessUp));
    // The press hands off a write even though the key-up adds no delta.
    let third = rig.single();
    assert_eq!(
        third.target, 65.0,
        "the new press steps from the previous intent target"
    );
}

#[test]
fn brightness_intent_coalescing_and_rebase() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    assert_eq!(first.target, 55.0);
    assert_eq!(first.generation, 1);

    // Repeats while the first command is in flight accumulate into one pending target.
    assert!(rig.hold(KeyKind::BrightnessUp));
    assert!(rig.hold(KeyKind::BrightnessUp));
    assert!(
        rig.take().is_empty(),
        "repeats wait for the in-flight write"
    );
    rig.finish(&first, true);
    let second = rig.single();
    assert_eq!(
        second.target, 65.0,
        "in-flight repeats coalesce to the latest target"
    );
    rig.finish(&second, true);

    // A manual slider value rebases the next press.
    rig.manual(Feature::Brightness, 20.0);
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "manual rebase is used by the next key"
    );
    let third = rig.single();
    assert_eq!(third.target, 25.0);
    rig.finish(&third, false);
}

#[test]
fn brightness_rejected_and_stale_work() {
    // A rejected write was already consumed; its release is consumed and nothing is re-sent.
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let submitted = rig.single();
    rig.finish(&submitted, false);
    assert!(
        rig.release(KeyKind::BrightnessUp),
        "paired release stays consumed"
    );
    assert!(rig.take().is_empty());

    // A held key whose display generation was replaced stays consumed and cancels future writes.
    let mut stale = Rig::new();
    stale.set(Feature::Brightness, Readback::at(50.0));
    assert!(stale.press(KeyKind::BrightnessUp));
    let old = stale.single();
    stale.displays.remove_generation(1);
    stale
        .displays
        .set(2, Feature::Brightness, Readback::at(50.0));
    assert!(
        stale.hold(KeyKind::BrightnessUp),
        "stale repeat remains consumed"
    );
    assert!(
        stale.release(KeyKind::BrightnessUp),
        "stale hold release remains consumed"
    );
    stale.finish(&old, true);
    assert!(
        stale.take().is_empty(),
        "stale generation cancels future writes"
    );
}

#[test]
fn brightness_later_key_supersedes_older_manual_acceptance() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    rig.manual(Feature::Brightness, 20.0);
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "key after slider reserves a newer target"
    );
    let first = rig.single();
    assert_eq!(
        first.target, 25.0,
        "key target starts from the latest slider value"
    );

    // An acceptance for a value the key already superseded (20, not the target 25) is ignored.
    rig.manual_accepted(Feature::Brightness, 20.0);
    assert!(rig.hold(KeyKind::BrightnessUp), "held key remains routed");
    rig.finish(&first, true);
    let second = rig.single();
    assert_eq!(
        second.target, 30.0,
        "older slider acceptance cannot rebase the newer target"
    );
    rig.finish(&second, false);
}

#[test]
fn keys_mac_observed_changes_and_orphan_repeats() {
    // A repeat without its initial down passes through and reserves nothing.
    let mut orphan = Rig::new();
    orphan.set(Feature::Volume, Readback::at(50.0));
    assert!(
        !orphan.hold(KeyKind::VolumeUp),
        "orphan repeat passes through"
    );
    assert!(orphan.take().is_empty(), "orphan repeat reserves no work");

    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::VolumeUp));
    let first = rig.single();
    assert_eq!(first.target, 55.0);
    rig.finish(&first, true);
    // The release adds no delta; the readback moved to 56 and is observed.
    rig.set(Feature::Volume, Readback::at(56.0));
    assert!(rig.release(KeyKind::VolumeUp));
    // An external adjustment to 30 is reconciled before the next press.
    rig.set(Feature::Volume, Readback::at(30.0));
    assert!(rig.press(KeyKind::VolumeUp));
    let updated = rig.single();
    assert_eq!(
        updated.target, 35.0,
        "external observed change replaces idle intent"
    );
    rig.finish(&updated, true);
}

#[test]
fn keys_mac_rebasing_does_not_grow_in_flight_work() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::VolumeUp));
    let first = rig.single();
    for _ in 0..100 {
        rig.manual(Feature::Volume, 40.0);
        assert!(rig.hold(KeyKind::VolumeUp));
    }
    assert!(
        rig.take().is_empty(),
        "manual rebases cannot create unbounded outstanding submissions"
    );
    rig.finish(&first, false);
    let latest = rig.single();
    assert_eq!(
        latest.target, 45.0,
        "one coalesced intent follows a superseded epoch"
    );
    rig.finish(&latest, true);
}

#[test]
fn brightness_pure_stepping_policy() {
    assert_eq!(stepped(50.0, true, false, 5.0), 55.0);
    assert_eq!(stepped(50.0, false, false, 5.0), 45.0);
    assert_eq!(stepped(50.0, true, false, 15.0), 65.0);
    assert_eq!(stepped(50.0, false, false, 1.0), 49.0);
    assert_eq!(stepped(50.0, true, true, 15.0), 51.0);
    assert_eq!(stepped(50.0, false, true, 15.0), 49.0);
    assert_eq!(stepped(50.0, true, true, 1.0), 51.0);
    assert_eq!(stepped(95.0, true, false, 15.0), 100.0);
    assert_eq!(stepped(5.0, false, false, 15.0), 0.0);
    assert_eq!(stepped(100.0, true, true, 15.0), 100.0);
    assert_eq!(KEY_DEFAULT_STEP, 5.0);
    assert_eq!(KEY_MIN_STEP, 1.0);
    assert_eq!(KEY_MAX_STEP, 15.0);
    assert_eq!(KEY_FINE_STEP, 1.0);
    assert_eq!(KEY_READBACK_TOLERANCE, 0.51);
    assert_eq!(KEY_REBASE_SETTLE_MS, 1000);
    assert_eq!(KEY_OUTSTANDING_EXPIRY_MS, 10_000);
}

#[test]
fn brightness_set_steps_clamps_and_falls_back() {
    assert_eq!(clamped_step(f64::NAN), 5.0);
    assert_eq!(clamped_step(f64::INFINITY), 5.0);
    assert_eq!(clamped_step(0.0), 1.0);
    assert_eq!(clamped_step(99.0), 15.0);
    assert_eq!(clamped_step(7.4), 7.0);
    assert_eq!(clamped_step(7.6), 8.0);

    // Each configured step is clamped the same way; brightness and volume read their own step.
    assert_eq!(
        one_press_with_steps(KeyKind::BrightnessUp, 0.0, 99.0, false, 50.0, 50.0),
        Some(51.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::BrightnessUp, 99.0, 0.0, false, 50.0, 50.0),
        Some(65.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::BrightnessUp, f64::NAN, 7.4, false, 50.0, 50.0),
        Some(55.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::BrightnessUp, 7.4, f64::INFINITY, false, 50.0, 50.0),
        Some(57.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::VolumeDown, 99.0, 0.0, false, 50.0, 50.0),
        Some(49.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::VolumeDown, 0.0, 99.0, false, 50.0, 50.0),
        Some(35.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::VolumeDown, 7.4, f64::NAN, false, 50.0, 50.0),
        Some(45.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::VolumeDown, f64::NAN, 7.4, false, 50.0, 50.0),
        Some(43.0)
    );
}

#[test]
fn brightness_and_volume_use_independent_steps() {
    assert_eq!(
        one_press_with_steps(KeyKind::BrightnessUp, 3.0, 8.0, false, 50.0, 50.0),
        Some(53.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::VolumeUp, 3.0, 8.0, false, 50.0, 50.0),
        Some(58.0)
    );
    // The fine step is one point for both features, whatever the steps are.
    assert_eq!(
        one_press_with_steps(KeyKind::BrightnessDown, 3.0, 8.0, true, 50.0, 50.0),
        Some(49.0)
    );
    assert_eq!(
        one_press_with_steps(KeyKind::VolumeDown, 3.0, 8.0, true, 50.0, 50.0),
        Some(49.0)
    );
}

#[test]
fn brightness_step_change_applies_to_next_press() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    assert_eq!(first.target, 55.0, "first press uses the default step");
    rig.finish(&first, true);
    assert!(rig.release(KeyKind::BrightnessUp));

    rig.keys.configure(true, 12.0, 12.0);
    assert!(rig.press(KeyKind::BrightnessUp), "next press is handled");
    let second = rig.single();
    assert_eq!(second.target, 67.0, "next press uses the new 12-point step");

    // The key is still held while the step changes; each repeat uses the step current at it.
    rig.keys.configure(true, 3.0, 3.0);
    assert!(
        rig.hold(KeyKind::BrightnessUp),
        "held repeat is consumed after the change"
    );
    assert!(
        rig.release(KeyKind::BrightnessUp),
        "held release is consumed"
    );
    rig.finish(&second, true);
    let third = rig.single();
    assert_eq!(
        third.target, 70.0,
        "held repeat uses the step current at the repeat"
    );
    rig.finish(&third, true);
}

#[test]
fn brightness_boundary_clamps_with_large_step() {
    assert_eq!(
        one_press(KeyKind::BrightnessUp, 15.0, false, 90.0, 50.0),
        Some(100.0)
    );
    assert_eq!(
        one_press(KeyKind::BrightnessDown, 15.0, false, 10.0, 50.0),
        Some(0.0)
    );
    assert_eq!(
        one_press(KeyKind::BrightnessUp, 15.0, false, 100.0, 50.0),
        None
    );
    assert_eq!(
        one_press(KeyKind::VolumeUp, 15.0, false, 50.0, 95.0),
        Some(100.0)
    );
    assert_eq!(
        one_press(KeyKind::VolumeDown, 15.0, false, 50.0, 5.0),
        Some(0.0)
    );
    assert_eq!(one_press(KeyKind::VolumeDown, 15.0, false, 50.0, 0.0), None);
}

#[test]
fn brightness_rebase_waits_for_own_hardware_actions() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    assert_eq!(first.target, 55.0);
    rig.action_accepted(Feature::Brightness, 7);
    rig.finish(&first, true);

    // While our accepted write is outstanding, a readback of the old level must not rebase 55.
    rig.set(Feature::Brightness, Readback::at(20.0));
    rig.observe();
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let second = rig.single();
    assert_eq!(
        second.target, 60.0,
        "next press starts from 55, not from readback 20"
    );
    rig.finish(&second, true);
}

#[test]
fn brightness_rebase_resumes_after_settle_window() {
    let mut rig = Rig::new();
    rig.at(1_000);
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    rig.action_accepted(Feature::Brightness, 7);
    rig.finish(&first, true);

    rig.at(1_100);
    rig.action_completed(Feature::Brightness, 7);
    rig.at(1_500);
    rig.set(Feature::Brightness, Readback::at(20.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let second = rig.single();
    assert_eq!(
        second.target, 60.0,
        "inside the settle window the readback does not rebase"
    );
    rig.finish(&second, true);

    rig.at(2_200);
    rig.observe();
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let third = rig.single();
    assert_eq!(
        third.target, 25.0,
        "after the settle window an external readback rebases"
    );
    rig.finish(&third, true);
}

#[test]
fn brightness_outstanding_action_expires() {
    let expiry = KEY_OUTSTANDING_EXPIRY_MS as u64;
    let mut rig = Rig::new();
    rig.at(1_000);
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    rig.action_accepted(Feature::Brightness, 7);
    rig.finish(&first, true);

    rig.at(1_000 + expiry - 1);
    rig.set(Feature::Brightness, Readback::at(20.0));
    rig.observe();
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let second = rig.single();
    assert_eq!(
        second.target, 60.0,
        "an unfinished write still blocks the rebase"
    );
    rig.finish(&second, true);

    rig.at(1_000 + expiry + 1);
    rig.observe();
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let third = rig.single();
    assert_eq!(
        third.target, 25.0,
        "an expired acceptance no longer blocks the rebase"
    );
    rig.finish(&third, true);
}

#[test]
fn brightness_early_completion_leaves_nothing_outstanding() {
    let mut rig = Rig::new();
    rig.at(1_000);
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let first = rig.single();
    // The completion arrives before its acceptance and cancels it.
    rig.action_completed(Feature::Brightness, 9);
    rig.action_accepted(Feature::Brightness, 9);
    rig.finish(&first, true);
    assert!(rig.release(KeyKind::BrightnessUp));

    rig.at(2_100);
    rig.set(Feature::Brightness, Readback::at(20.0));
    rig.observe();
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let second = rig.single();
    assert_eq!(
        second.target, 25.0,
        "a completed early acceptance leaves nothing outstanding"
    );
    rig.finish(&second, true);
}

#[test]
fn keys_mac_fine_step_ignores_configured_step() {
    assert_eq!(
        one_press(KeyKind::BrightnessUp, 15.0, false, 50.0, 50.0),
        Some(65.0)
    );
    assert_eq!(
        one_press(KeyKind::BrightnessUp, 15.0, true, 50.0, 50.0),
        Some(51.0)
    );
    assert_eq!(
        one_press(KeyKind::BrightnessDown, 1.0, true, 50.0, 50.0),
        Some(49.0)
    );
    assert_eq!(
        one_press(KeyKind::VolumeUp, 15.0, true, 50.0, 50.0),
        Some(51.0)
    );
    assert_eq!(
        one_press(KeyKind::VolumeDown, 15.0, true, 50.0, 50.0),
        Some(49.0)
    );
}

#[test]
fn brightness_fine_step_is_one_point() {
    assert_eq!(
        one_press(KeyKind::BrightnessUp, 15.0, true, 50.0, 50.0),
        Some(51.0)
    );
    assert_eq!(
        one_press(KeyKind::BrightnessDown, 15.0, true, 50.0, 50.0),
        Some(49.0)
    );
    assert_eq!(
        one_press(KeyKind::BrightnessDown, 1.0, true, 50.0, 50.0),
        Some(49.0)
    );
}

#[test]
fn brightness_outstanding_write_does_not_block_volume_rebase() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::VolumeUp));
    assert!(rig.release(KeyKind::VolumeUp));
    let volume = rig.single();
    rig.finish(&volume, true);

    // An accepted brightness write stays outstanding; volume is not settling and rebases at once.
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let brightness = rig.single();
    rig.action_accepted(Feature::Brightness, 7);
    rig.finish(&brightness, true);

    rig.set(Feature::Volume, Readback::at(20.0));
    rig.observe();
    assert!(rig.press(KeyKind::VolumeUp));
    assert!(rig.release(KeyKind::VolumeUp));
    let rebased = rig.single();
    assert_eq!(rebased.feature, Feature::Volume);
    assert_eq!(
        rebased.target, 25.0,
        "volume rebases despite the brightness outstanding write"
    );
    rig.finish(&rebased, true);
}

#[test]
fn brightness_at_boundaries_is_consumed_without_write() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(100.0));
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "brightness up at 100 is consumed"
    );
    assert!(rig.release(KeyKind::BrightnessUp));
    rig.set(Feature::Brightness, Readback::at(0.0));
    assert!(
        rig.press(KeyKind::BrightnessDown),
        "brightness down at 0 is consumed"
    );
    assert!(rig.release(KeyKind::BrightnessDown));
    assert!(rig.take().is_empty(), "no write at either boundary");
}

#[test]
fn brightness_owner_none_passes_the_whole_sequence() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(
        !rig.send(key_down(KeyKind::BrightnessUp), None),
        "no owner passes the press"
    );
    assert!(
        !rig.send(key_repeat(KeyKind::BrightnessUp), Some(&owner_a())),
        "the repeat of a passed-through press stays passed through"
    );
    assert!(
        !rig.send(key_up(KeyKind::BrightnessUp), Some(&owner_a())),
        "the release of a passed-through press stays passed through"
    );
    assert!(rig.take().is_empty(), "nothing is written");
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "a fresh owned press is handled"
    );
    let first = rig.single();
    assert_eq!(first.target, 55.0);
    rig.finish(&first, true);
}

#[test]
fn brightness_disable_clears_held_key_state() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    rig.keys
        .configure(false, KEY_DEFAULT_STEP, KEY_DEFAULT_STEP);
    rig.keys.configure(true, KEY_DEFAULT_STEP, KEY_DEFAULT_STEP);
    assert!(
        !rig.release(KeyKind::BrightnessUp),
        "disabling cleared the held latch"
    );
}

#[test]
fn brightness_and_volume_intents_are_independent() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    rig.set(Feature::Volume, Readback::at(50.0));

    // A volume press is not held back by an in-flight brightness write, and vice versa.
    assert!(rig.press(KeyKind::BrightnessUp));
    let brightness = rig.single();
    assert_eq!(brightness.feature, Feature::Brightness);
    assert_eq!(brightness.target, 55.0);
    assert!(rig.press(KeyKind::VolumeUp));
    let volume = rig.single();
    assert_eq!(volume.feature, Feature::Volume);
    assert_eq!(volume.target, 55.0);

    // A brightness repeat accumulates only in the brightness intent.
    assert!(rig.hold(KeyKind::BrightnessUp));
    assert!(rig.take().is_empty(), "both features are in flight");
    rig.finish(&volume, true);
    assert!(
        rig.take().is_empty(),
        "a finished volume write leaves brightness alone"
    );
    rig.finish(&brightness, true);
    let coalesced = rig.single();
    assert_eq!(coalesced.feature, Feature::Brightness);
    assert_eq!(coalesced.target, 60.0);
    assert!(rig.release(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::VolumeUp));

    // Rebase is per feature: an external volume change rebases volume while brightness is in flight.
    rig.set(Feature::Volume, Readback::at(20.0));
    rig.observe();
    assert!(rig.press(KeyKind::VolumeUp));
    let rebased = rig.single();
    assert_eq!(rebased.feature, Feature::Volume);
    assert_eq!(rebased.target, 25.0);
    rig.finish(&rebased, true);
    rig.finish(&coalesced, true);
    assert!(rig.take().is_empty());
}

#[test]
fn brightness_never_touches_mute_memory() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert_eq!(muted.target, 0.0);
    assert!(rig.release(KeyKind::Mute));
    rig.finish(&muted, true);

    // The monitor now reads 0. Brightness keys in between must not forget the remembered 50.
    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    assert!(rig.release(KeyKind::BrightnessUp));
    let up = rig.single();
    assert_eq!(up.feature, Feature::Brightness);
    assert_eq!(up.target, 55.0);
    rig.finish(&up, true);
    assert!(rig.press(KeyKind::BrightnessDown));
    assert!(rig.release(KeyKind::BrightnessDown));
    let down = rig.single();
    assert_eq!(down.target, 50.0);
    rig.finish(&down, true);

    // Volume-up restores the remembered 50 and adds one step.
    assert!(rig.press(KeyKind::VolumeUp));
    assert!(rig.release(KeyKind::VolumeUp));
    let restored = rig.single();
    assert_eq!(restored.feature, Feature::Volume);
    assert_eq!(
        restored.target, 55.0,
        "brightness keys leave the mute memory intact"
    );
    rig.finish(&restored, true);
}

#[test]
fn brightness_key_with_owner_none_is_not_consumed_for_whole_sequence() {
    brightness_owner_none_passes_the_whole_sequence();
}

#[test]
fn keys_mac_handoff_stale_completion_keeps_newer_manual_epoch() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    assert!(rig.press(KeyKind::BrightnessUp));
    let old = rig.single();
    rig.manual(Feature::Brightness, 20.0);
    rig.finish(&old, false);
    assert!(
        rig.hold(KeyKind::BrightnessUp),
        "held key continues from the manual rebase"
    );
    let current = rig.single();
    assert_eq!(
        current.target, 25.0,
        "stale completion cannot erase a newer manual epoch"
    );
    rig.finish(&current, true);

    rig.keys.shutdown();
    assert!(
        !rig.press(KeyKind::BrightnessUp),
        "a stopped router cannot be restarted"
    );
}

#[test]
fn keys_mac_mute_writes_zero_once() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute), "mute press is handled");
    let muted = rig.single();
    assert_eq!(muted.feature, Feature::Volume);
    assert_eq!(muted.target, 0.0);
    assert!(rig.hold(KeyKind::Mute), "held mute repeat is consumed");
    assert!(rig.release(KeyKind::Mute), "mute key-up is consumed");
    rig.finish(&muted, true);
    assert!(rig.take().is_empty(), "held mute submits no second command");
}

#[test]
fn keys_mac_mute_restores_remembered_level() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert!(rig.release(KeyKind::Mute), "first mute release is consumed");
    rig.finish(&muted, true);

    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(rig.press(KeyKind::Mute), "restoring mute press is handled");
    assert!(
        rig.release(KeyKind::Mute),
        "restoring mute release is consumed"
    );
    let restored = rig.single();
    assert_eq!(restored.target, 50.0, "mute restores the remembered level");
    rig.finish(&restored, true);
}

#[test]
fn keys_mac_mute_then_volume_up_steps_from_remembered_level() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert!(rig.release(KeyKind::Mute));
    rig.finish(&muted, true);

    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(
        rig.press(KeyKind::VolumeUp),
        "volume-up while muted is handled"
    );
    assert!(
        rig.release(KeyKind::VolumeUp),
        "volume-up release while muted is consumed"
    );
    let unmuted = rig.single();
    assert_eq!(
        unmuted.target,
        50.0 + KEY_DEFAULT_STEP,
        "steps from the remembered level"
    );
    rig.finish(&unmuted, true);
}

#[test]
fn keys_mac_mute_then_volume_down_writes_nothing() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert!(rig.release(KeyKind::Mute));
    rig.finish(&muted, true);

    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(
        rig.press(KeyKind::VolumeDown),
        "volume-down while muted is consumed"
    );
    assert!(
        rig.release(KeyKind::VolumeDown),
        "volume-down release while muted is consumed"
    );
    assert!(
        rig.take().is_empty(),
        "volume-down while muted writes nothing"
    );
}

#[test]
fn keys_mac_mute_at_zero_with_nothing_remembered() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(
        rig.press(KeyKind::Mute),
        "mute at zero with nothing remembered is consumed"
    );
    assert!(
        rig.release(KeyKind::Mute),
        "mute at zero release is consumed"
    );
    assert!(rig.take().is_empty(), "mute at zero submits no write");
}

#[test]
fn keys_mac_mute_external_change_discards_remembered_level() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert!(rig.release(KeyKind::Mute));
    rig.finish(&muted, true);

    rig.set(Feature::Volume, Readback::at(30.0));
    assert!(
        rig.press(KeyKind::Mute),
        "mute after external change is handled"
    );
    assert!(
        rig.release(KeyKind::Mute),
        "mute release after external change is consumed"
    );
    let rewritten = rig.single();
    assert_eq!(
        rewritten.target, 0.0,
        "external change discards the remembered level"
    );
    rig.finish(&rewritten, true);
}

#[test]
fn keys_mac_output_round_trip_forgets_remembered_level() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert!(rig.release(KeyKind::Mute));
    rig.finish(&muted, true);

    rig.keys.set_volume_owner(None);
    rig.keys.set_volume_owner(Some(MONITOR));
    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(
        rig.press(KeyKind::Mute),
        "mute after output round trip is consumed"
    );
    assert!(
        rig.release(KeyKind::Mute),
        "mute release after output round trip is consumed"
    );
    assert!(
        rig.take().is_empty(),
        "output change forgets the remembered level"
    );
}

#[test]
fn keys_mac_mute_passes_through_without_a_supported_monitor() {
    // No owner: the whole mute sequence passes through.
    let mut unowned = Rig::new();
    unowned.set(Feature::Volume, Readback::at(50.0));
    unowned.keys.set_volume_owner(None);
    assert!(!unowned.send(key_down(KeyKind::Mute), None));
    assert!(!unowned.send(key_repeat(KeyKind::Mute), None));
    assert!(!unowned.send(key_up(KeyKind::Mute), None));
    assert!(unowned.take().is_empty());

    // Unsupported volume or a sleeping display: the same.
    let unsupported = Readback {
        supported: false,
        ..Readback::at(50.0)
    };
    let asleep = Readback {
        awake: false,
        ..Readback::at(50.0)
    };
    for readback in [unsupported, asleep] {
        let mut rig = Rig::new();
        rig.set(Feature::Volume, readback);
        assert!(!rig.press(KeyKind::Mute), "mute passes through");
        assert!(!rig.hold(KeyKind::Mute), "held mute passes through");
        assert!(!rig.release(KeyKind::Mute), "mute release passes through");
        assert!(rig.take().is_empty());
    }
}

#[test]
fn keys_mac_output_switch_cancels_held_mute() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute), "held mute starts on the monitor");
    let muted = rig.single();
    rig.keys.set_volume_owner(None);
    assert!(
        rig.send(key_repeat(KeyKind::Mute), None),
        "repeat after output switch is consumed"
    );
    assert!(
        rig.send(key_up(KeyKind::Mute), None),
        "release after output switch is consumed"
    );
    rig.finish(&muted, true);
    assert!(rig.take().is_empty(), "output switch cancels the held mute");
}

#[test]
fn keys_mac_mute_with_disallowed_modifiers_passes_through() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    let down = KeyEvent {
        modifiers_eligible: false,
        ..key_down(KeyKind::Mute)
    };
    assert!(
        !rig.send(down, Some(&owner_a())),
        "mute with disallowed modifiers passes through"
    );
    let up = KeyEvent {
        modifiers_eligible: false,
        ..key_up(KeyKind::Mute)
    };
    assert!(
        !rig.send(up, Some(&owner_a())),
        "mute release with disallowed modifiers passes"
    );
    assert!(rig.take().is_empty(), "ineligible mute submits no write");
}

#[test]
fn keys_mac_volume_hold_across_output_changes() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::VolumeUp), "hold starts on the monitor");
    let first = rig.single();

    rig.keys.set_volume_owner(None);
    assert!(
        rig.send(key_repeat(KeyKind::VolumeUp), None),
        "repeat after the output switched away is still consumed"
    );
    assert!(
        rig.send(key_repeat(KeyKind::VolumeUp), None),
        "later repeats are still consumed"
    );
    assert!(
        rig.send(key_up(KeyKind::VolumeUp), None),
        "key-up is consumed"
    );
    rig.finish(&first, true);
    assert!(
        rig.take().is_empty(),
        "no write once the output switched away"
    );

    // The next press follows the new output and passes through.
    assert!(
        !rig.send(key_down(KeyKind::VolumeUp), None),
        "next press passes through"
    );
    assert!(!rig.send(key_up(KeyKind::VolumeUp), None));

    // A passed-through press stays passed through when the output becomes the monitor mid-hold.
    let mut pass = Rig::new();
    pass.set(Feature::Volume, Readback::at(50.0));
    pass.keys.set_volume_owner(None);
    assert!(
        !pass.send(key_down(KeyKind::VolumeUp), None),
        "hold starts passed through"
    );
    pass.keys.set_volume_owner(Some(MONITOR));
    assert!(
        !pass.send(key_repeat(KeyKind::VolumeUp), Some(&owner_a())),
        "repeat stays pass-through after the output switched to the monitor"
    );
    assert!(
        !pass.send(key_up(KeyKind::VolumeUp), Some(&owner_a())),
        "key-up stays pass-through after the output switched to the monitor"
    );
    assert!(
        pass.press(KeyKind::VolumeUp),
        "next distinct press targets the monitor"
    );
    let next = pass.single();
    assert_eq!(next.target, 55.0);
    pass.finish(&next, true);
}

#[test]
fn keys_mac_volume_boundaries_modifiers_and_latches() {
    let mut rig = Rig::new();
    // Boundary values are consumed without a write.
    rig.set(Feature::Volume, Readback::at(100.0));
    assert!(
        rig.press(KeyKind::VolumeUp),
        "volume at maximum is consumed"
    );
    assert!(rig.release(KeyKind::VolumeUp));
    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(
        rig.press(KeyKind::VolumeDown),
        "volume at minimum is consumed"
    );
    assert!(rig.release(KeyKind::VolumeDown));
    assert!(rig.take().is_empty(), "boundary presses submit no write");

    // Disallowed modifiers pass through, on press and on release.
    rig.set(Feature::Volume, Readback::at(50.0));
    let ineligible = KeyEvent {
        modifiers_eligible: false,
        ..key_down(KeyKind::VolumeUp)
    };
    assert!(
        !rig.send(ineligible, Some(&owner_a())),
        "disallowed modifiers pass through"
    );
    let ineligible_up = KeyEvent {
        modifiers_eligible: false,
        ..key_up(KeyKind::VolumeUp)
    };
    assert!(
        !rig.send(ineligible_up, Some(&owner_a())),
        "disallowed-modifier release passes"
    );

    // Shift+Option takes the one-point fine step.
    let fine = KeyEvent {
        fine: true,
        ..key_down(KeyKind::VolumeDown)
    };
    assert!(rig.send(fine, Some(&owner_a())), "fine press is handled");
    let step = rig.single();
    assert_eq!(step.target, 49.0, "fine step is one point");
    rig.finish(&step, true);
    assert!(rig.release(KeyKind::VolumeDown));
}

#[test]
fn keys_mac_brightness_and_volume_latches_are_independent() {
    let mut rig = Rig::new();
    rig.set(Feature::Brightness, Readback::at(50.0));
    rig.set(Feature::Volume, Readback::at(50.0));
    rig.keys.set_volume_owner(None);
    assert!(
        rig.press(KeyKind::BrightnessUp),
        "brightness follows the pointer, not the output"
    );
    assert!(
        !rig.send(key_down(KeyKind::VolumeUp), None),
        "volume passes through while the brightness latch is handled"
    );
    rig.keys.set_volume_owner(Some(MONITOR));
    assert!(
        rig.hold(KeyKind::BrightnessUp),
        "brightness latch is unaffected by the output change"
    );
    assert!(
        !rig.send(key_repeat(KeyKind::VolumeUp), None),
        "volume pass-through latch is unaffected by the output change"
    );
    assert!(
        rig.release(KeyKind::BrightnessUp),
        "brightness release is consumed"
    );
    assert!(
        !rig.send(key_up(KeyKind::VolumeUp), None),
        "volume release passes through"
    );
    assert!(
        !rig.send(key_down(KeyKind::BrightnessUp), None),
        "brightness outside every display passes through"
    );
    assert!(!rig.send(key_up(KeyKind::BrightnessUp), None));
}

#[test]
fn keys_mac_volume_stale_generation_passes_through() {
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::VolumeUp));
    let first = rig.single();
    rig.displays.remove_generation(1);
    rig.displays.set(2, Feature::Volume, Readback::at(50.0));
    assert!(
        rig.hold(KeyKind::VolumeUp),
        "stale volume repeat remains consumed"
    );
    assert!(
        rig.release(KeyKind::VolumeUp),
        "stale volume hold release remains consumed"
    );
    rig.finish(&first, true);
    assert!(
        rig.take().is_empty(),
        "stale generation cancels future volume deltas"
    );
}

#[test]
fn keys_mac_disable_and_shutdown_stop_routing() {
    let mut disabled = Rig::new();
    disabled.set(Feature::Volume, Readback::at(50.0));
    disabled
        .keys
        .configure(false, KEY_DEFAULT_STEP, KEY_DEFAULT_STEP);
    assert!(
        !disabled.press(KeyKind::VolumeUp),
        "disabled routing passes volume through"
    );
    assert!(!disabled.release(KeyKind::VolumeUp));

    let mut stopped = Rig::new();
    stopped.set(Feature::Volume, Readback::at(50.0));
    stopped.set(Feature::Brightness, Readback::at(50.0));
    stopped.keys.shutdown();
    assert!(
        !stopped.press(KeyKind::VolumeUp),
        "a stopped router passes volume through"
    );
    assert!(
        !stopped.press(KeyKind::BrightnessUp),
        "a stopped router passes brightness through"
    );
    assert!(stopped.take().is_empty());
}

#[test]
fn keys_mac_mute_memory_is_per_key_not_per_press() {
    // Mute remembers the level only while the monitor is the owner; this covers the restore.
    let mut rig = Rig::new();
    rig.set(Feature::Volume, Readback::at(50.0));
    assert!(rig.press(KeyKind::Mute));
    let muted = rig.single();
    assert!(rig.release(KeyKind::Mute));
    rig.finish(&muted, true);
    rig.set(Feature::Volume, Readback::at(0.0));
    assert!(rig.press(KeyKind::Mute));
    assert!(rig.release(KeyKind::Mute));
    let restored = rig.single();
    assert_eq!(restored.target, 50.0);
    rig.finish(&restored, true);
}
