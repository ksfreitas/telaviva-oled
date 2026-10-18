//! Pins the preserved C++ quirks of the key policy. A failure here means a behavior change that
//! must be made on purpose.

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

/// Keys under test with a fake panel and a clock. Every key uses connection generation 1.
struct Rig {
    keys: Keys,
    panel: FakeDisplays,
    now: f64,
    owner: Option<String>,
}

impl Rig {
    fn new() -> Self {
        Self {
            keys: Keys::new(),
            panel: FakeDisplays::default(),
            now: 1.0,
            owner: None,
        }
    }

    /// Enables keys with both steps set to `step`.
    fn configure(&mut self, step: f64) {
        self.keys.configure(true, step, step);
    }

    /// Reports a volume state of `monitor` and reconciles, as the shell does on observation.
    fn report(&mut self, monitor: &str, state: DisplayState) {
        self.panel.set(monitor, 1, Feature::Volume, state);
        self.keys.observe(self.now, &self.panel);
    }

    fn set_owner(&mut self, monitor: Option<&str>) {
        self.owner = monitor.map(str::to_owned);
        self.keys.set_volume_owner(monitor);
    }

    fn send(&mut self, key: KeyKind, phase: KeyPhase, repeat: bool) -> bool {
        let owner = self.owner.as_ref().map(|monitor_id| KeyOwner {
            monitor_id: monitor_id.clone(),
            generation: 1,
        });
        let event = KeyEvent {
            key,
            phase,
            is_repeat: repeat,
            fine: false,
            modifiers_eligible: true,
        };
        self.keys
            .handle_key(self.now, &event, owner.as_ref(), &self.panel)
    }

    fn press(&mut self, key: KeyKind) -> bool {
        self.send(key, KeyPhase::Down, false)
    }

    fn repeat(&mut self, key: KeyKind) -> bool {
        self.send(key, KeyPhase::Down, true)
    }

    fn release(&mut self, key: KeyKind) -> bool {
        self.send(key, KeyPhase::Up, false)
    }

    fn take(&mut self) -> Vec<KeyIntent> {
        self.keys.take_intents()
    }

    /// Takes the one intent handed out, failing if there is not exactly one.
    fn take_one(&mut self) -> KeyIntent {
        let mut intents = self.take();
        assert_eq!(intents.len(), 1);
        intents.remove(0)
    }

    fn finish(&mut self, intent: &KeyIntent, accepted: bool) {
        self.keys
            .intent_finished(self.now, &self.panel, intent, accepted);
    }

    fn accepted(&mut self, monitor: &str, action: u64) {
        self.keys
            .action_accepted(self.now, monitor, 1, Feature::Volume, action);
    }

    fn completed(&mut self, monitor: &str, action: u64) {
        self.keys
            .action_completed(self.now, monitor, 1, Feature::Volume, action);
    }
}

/// An unknown (NaN) readback erases a pending intent, so the next press steps from the readback.
/// Preserved from the Windows C++ original, not a design goal.
#[test]
fn known_quirk_reconcile_erases_an_intent_on_an_unknown_readback() {
    let mut rig = Rig::new();
    rig.configure(5.0);
    rig.set_owner(Some("m1"));
    rig.report("m1", ready(50.0));
    assert!(rig.press(VolumeUp));
    let first = rig.take_one();
    assert_eq!(first.target, 55.0);
    assert!(rig.release(VolumeUp));
    rig.finish(&first, true);

    rig.report(
        "m1",
        DisplayState {
            percent: f64::NAN,
            ..ready(50.0)
        },
    );
    rig.report("m1", ready(50.0));
    assert!(rig.press(VolumeUp));
    assert_eq!(rig.take_one().target, 55.0);
}

/// An intent is erased when its display turns unsupported, even while its write is in flight.
/// Preserved from the Windows C++ original, not a design goal.
#[test]
fn known_quirk_flush_erases_an_intent_while_a_write_is_in_flight() {
    let mut rig = Rig::new();
    rig.configure(5.0);
    rig.set_owner(Some("m1"));
    rig.report("m1", ready(50.0));
    assert!(rig.press(VolumeUp));
    let first = rig.take_one();

    rig.report(
        "m1",
        DisplayState {
            supported: false,
            ..ready(50.0)
        },
    );
    assert!(rig.repeat(VolumeUp));
    assert!(rig.take().is_empty());
    rig.finish(&first, true);
    assert!(rig.take().is_empty());
}

/// A held mute writes 0 once; its repeats and its key-up are consumed without another write.
/// Preserved from the Windows C++ original, not a design goal.
#[test]
fn known_quirk_mute_repeats_are_consumed_without_a_write() {
    let mut rig = Rig::new();
    rig.configure(5.0);
    rig.set_owner(Some("m1"));
    rig.report("m1", ready(50.0));
    assert!(rig.press(Mute));
    let muted = rig.take_one();
    assert_eq!(muted.target, 0.0);
    assert!(rig.repeat(Mute));
    assert!(rig.take().is_empty());
    assert!(rig.release(Mute));
    rig.finish(&muted, true);
    assert!(rig.take().is_empty());
}

/// Overflowing the early-completion bound clears every early completion, so a later acceptance
/// of the first action becomes an outstanding write that blocks the rebase.
/// Preserved from the Windows C++ original, not a design goal.
#[test]
fn known_quirk_early_completion_overflow_clears_every_early_completion() {
    let mut rig = Rig::new();
    rig.configure(5.0);
    let ids: Vec<String> = (0..=KEY_MAX_EARLY_COMPLETED)
        .map(|index| format!("k{index:02}"))
        .collect();
    for id in &ids {
        rig.report(id, ready(50.0));
    }

    for id in &ids[..KEY_MAX_EARLY_COMPLETED] {
        rig.completed(id, 1);
    }
    // The 65th completion overflows the bound and clears the first key's early completion.
    rig.completed(&ids[KEY_MAX_EARLY_COMPLETED], 1);
    rig.accepted(&ids[0], 1);

    // A manual intent on the first key, then a readback that moved past the tolerance.
    rig.keys
        .manual_control_changed(&rig.panel, &ids[0], 1, Feature::Volume, 50.0);
    rig.now += 2.0;
    rig.report(&ids[0], ready(20.0));

    // Not rebased to 20 while the acceptance is outstanding, so the press steps from 50.
    rig.set_owner(Some(ids[0].as_str()));
    assert!(rig.press(VolumeUp));
    assert_eq!(rig.take_one().target, 55.0);
}

/// A 129th key with a write in flight is not consumed and creates no intent, since the bound is 128.
/// Preserved from the Windows C++ original, not a design goal.
#[test]
fn known_quirk_the_129th_in_flight_key_passes_through() {
    let mut rig = Rig::new();
    rig.configure(5.0);
    let ids: Vec<String> = (0..=KEY_MAX_IN_FLIGHT)
        .map(|index| format!("m{index:03}"))
        .collect();
    for id in &ids[..KEY_MAX_IN_FLIGHT] {
        rig.report(id, ready(50.0));
        rig.set_owner(Some(id.as_str()));
        assert!(rig.press(VolumeUp));
    }
    assert_eq!(rig.take().len(), KEY_MAX_IN_FLIGHT);

    let probe = &ids[KEY_MAX_IN_FLIGHT];
    rig.report(probe, ready(50.0));
    rig.set_owner(Some(probe.as_str()));
    assert!(!rig.press(VolumeUp));
    assert!(rig.take().is_empty());
}
