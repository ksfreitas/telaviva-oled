use super::*;
use crate::controls::model::*;
use crate::controls::Tokens;
use serde_json::Value;

/// A channel that spans 0 to 100 and reads `observed`.
fn channel(observed: f64) -> ChannelObs {
    ChannelObs {
        supported: true,
        observed: Some(observed),
        min: 0.0,
        max: 100.0,
    }
}

fn mon(id: &str, generation: u64, state: &str, brightness: f64, volume: f64) -> MonitorObs {
    MonitorObs {
        id: id.to_owned(),
        generation,
        display_state: state.to_owned(),
        identity_status: "stable".to_owned(),
        brightness: channel(brightness),
        volume: channel(volume),
    }
}

/// The fixture monitor: "m1", generation 1, awake, brightness 41, volume 20.
fn base() -> MonitorObs {
    mon("m1", 1, "awake", 41.0, 20.0)
}

/// A monitor whose brightness channel has the given range; the rest is as in `base`.
fn mon_range(id: &str, brightness: ChannelObs) -> MonitorObs {
    let mut monitor = mon(id, 1, "awake", 0.0, 20.0);
    monitor.brightness = brightness;
    monitor
}

fn obs(revision: u64, monitors: Vec<MonitorObs>) -> Observation {
    obs_pending(revision, monitors, Vec::new())
}

fn obs_pending(
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

fn action(id: u64, generation: u64, monitor: &str, kind: &str, value: f64) -> PendingActionObs {
    PendingActionObs {
        id,
        generation,
        monitor_id: monitor.to_owned(),
        kind: kind.to_owned(),
        value: Some(value),
    }
}

fn view(has_draft: bool, draft: f64, status: DraftStatus) -> DraftView {
    DraftView {
        has_draft,
        draft,
        status,
    }
}

/// Parses every SubmitCommand in `effects` into (token, command JSON).
fn submits(effects: &[Effect]) -> Vec<(u64, Value)> {
    effects
        .iter()
        .filter_map(|effect| match effect {
            Effect::SubmitCommand {
                token,
                command_json,
            } => Some((
                *token,
                serde_json::from_str::<Value>(command_json).expect("command_json is JSON"),
            )),
            _ => None,
        })
        .collect()
}

/// Whether `effects` report a draft change for this monitor and feature.
fn changed(effects: &[Effect], monitor: &str, feature: Feature) -> bool {
    effects.iter().any(|effect| match effect {
        Effect::DraftChanged {
            monitor_id,
            feature: changed_feature,
        } => monitor_id.as_str() == monitor && *changed_feature == feature,
        _ => false,
    })
}

/// Whether a submitted command is of `kind` and mentions `monitor` (the id field name is not asserted).
fn has_submit(subs: &[(u64, Value)], monitor: &str, kind: &str) -> bool {
    subs.iter().any(|(_, json)| {
        json["type"] == kind && json.to_string().contains(&format!("\"{}\"", monitor))
    })
}

/// The machine under test with its tokens and a clock that the tests move by hand.
struct Rig {
    manual: Manual,
    tokens: Tokens,
    now: f64,
}

impl Rig {
    fn new() -> Self {
        Self {
            manual: Manual::new(),
            tokens: Tokens::new(),
            now: 1.0,
        }
    }

    fn observe(&mut self, snapshot: &Observation) -> Vec<Effect> {
        self.manual.observe(self.now, snapshot)
    }

    fn set(&mut self, monitor: &str, feature: Feature, value: f64) -> Option<DraftView> {
        self.manual.set_target(self.now, monitor, feature, value)
    }

    fn tick(&mut self) -> Vec<Effect> {
        self.manual.tick(self.now, &mut self.tokens)
    }

    /// Ticks one drain delay later, as the shell does when the drain timer fires.
    fn after_drain(&mut self) -> Vec<Effect> {
        self.now += DRAIN_DELAY;
        self.tick()
    }

    fn submitted(&mut self, token: u64, accepted: bool) -> Vec<Effect> {
        self.manual
            .on_submitted(self.now, &mut self.tokens, token, accepted)
    }

    fn complete(
        &mut self,
        monitor: &str,
        feature: Feature,
        generation: u64,
        ok: bool,
        observed: Option<f64>,
    ) -> Vec<Effect> {
        self.manual
            .on_hardware_completed(self.now, monitor, feature, generation, ok, observed)
    }

    fn draft(&self, monitor: &str, feature: Feature) -> DraftView {
        self.manual.draft(monitor, feature)
    }

    fn next_deadline(&self) -> Option<f64> {
        self.manual.next_deadline()
    }

    fn shutdown(&mut self) {
        self.manual.shutdown();
    }

    /// Drains the queued target at its drain time and returns the token of its one command.
    fn submit_one(&mut self) -> u64 {
        let effects = self.after_drain();
        let subs = submits(&effects);
        assert_eq!(subs.len(), 1);
        subs[0].0
    }

    /// Drains the queued target and accepts the command it submits.
    fn submit_accepted(&mut self) -> u64 {
        let token = self.submit_one();
        self.submitted(token, true);
        token
    }
}

/// A rig whose brightness command for `value` on "m1" is accepted and in flight.
fn accepted_brightness(value: f64) -> Rig {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, value);
    rig.submit_accepted();
    rig
}

#[test]
fn manual_idle_control_does_no_work() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(false, 41.0, DraftStatus::Ready)
    );
    assert_eq!(rig.next_deadline(), None);
    assert!(rig.tick().is_empty());
}

#[test]
fn manual_set_target_shows_draft_at_once_and_submits_after_drain() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    let shown = rig.set("m1", Feature::Brightness, 37.0);
    assert_eq!(shown, Some(view(true, 37.0, DraftStatus::Pending)));
    let due = 1.0 + DRAIN_DELAY;
    assert_eq!(rig.next_deadline(), Some(due));

    rig.now = 1.019;
    assert!(rig.tick().is_empty());

    rig.now = due;
    let effects = rig.tick();
    let subs = submits(&effects);
    assert_eq!(effects.len(), 1);
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["type"], "set_brightness");
    assert_eq!(subs[0].1["value"], 37.0);
    assert_eq!(subs[0].1["connection_generation"], 1);
}

#[test]
fn manual_rejected_submit_fails_without_retry() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 62.0);
    let token = rig.submit_one();

    let effects = rig.submitted(token, false);
    assert!(changed(&effects, "m1", Feature::Brightness));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 41.0, DraftStatus::Failed)
    );

    rig.observe(&obs(2, vec![base()]));
    rig.now = 10.0;
    assert!(rig.tick().is_empty());

    rig.set("m1", Feature::Brightness, 62.0);
    assert_eq!(submits(&rig.after_drain()).len(), 1);
}

#[test]
fn manual_rapid_targets_coalesce_behind_in_flight_submission() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 0.0);
    rig.submit_accepted();

    rig.set("m1", Feature::Brightness, 100.0);
    rig.set("m1", Feature::Brightness, 37.0);
    assert!(rig.after_drain().is_empty());

    rig.now = 1.5;
    rig.complete("m1", Feature::Brightness, 1, true, Some(0.0));
    assert_eq!(rig.next_deadline(), Some(1.5));
    let subs = submits(&rig.tick());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 37.0);
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 37.0, DraftStatus::Pending)
    );
}

#[test]
fn manual_queued_target_waits_for_core_pending_action() {
    let mut rig = Rig::new();
    let pending = vec![action(7, 1, "m1", "set_brightness", 41.0)];
    rig.observe(&obs_pending(1, vec![base()], pending));
    rig.set("m1", Feature::Brightness, 50.0);
    assert!(rig.after_drain().is_empty());
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 50.0, DraftStatus::Pending)
    );

    rig.now = 2.0;
    rig.observe(&obs(2, vec![base()]));
    assert_eq!(rig.next_deadline(), Some(2.0 + DRAIN_DELAY));
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 50.0);
}

#[test]
fn manual_accepted_command_stays_in_flight_until_completion() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 20.0);
    rig.submit_accepted();

    // Newer snapshots while the command is young do not release it.
    rig.now = 1.1;
    rig.observe(&obs(2, vec![base()]));
    rig.now = 1.2;
    rig.observe(&obs(3, vec![base()]));
    rig.set("m1", Feature::Brightness, 37.0);
    assert!(rig.after_drain().is_empty());

    rig.now = 1.5;
    rig.complete("m1", Feature::Brightness, 1, true, Some(20.0));
    let subs = submits(&rig.tick());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 37.0);
}

#[test]
fn manual_readback_within_tolerance_clears_draft() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 37.0);
    rig.submit_accepted();

    let effects = rig.observe(&obs(2, vec![mon("m1", 1, "awake", 37.2, 20.0)]));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(false, 37.2, DraftStatus::Ready)
    );
    assert!(changed(&effects, "m1", Feature::Brightness));
}

#[test]
fn manual_completion_without_readback_clears_draft() {
    let mut rig = accepted_brightness(37.0);
    rig.complete("m1", Feature::Brightness, 1, true, Some(30.0));
    let draft = rig.draft("m1", Feature::Brightness);
    assert!(!draft.has_draft);
    assert_eq!(draft.status, DraftStatus::Ready);
}

#[test]
fn manual_stale_completion_cannot_replace_newer_draft() {
    let mut rig = accepted_brightness(20.0);
    rig.set("m1", Feature::Brightness, 37.0);

    // The completion of the 20 command arrives after the newer 37 target.
    rig.complete("m1", Feature::Brightness, 1, true, Some(20.0));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 37.0, DraftStatus::Pending)
    );
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 37.0);
}

#[test]
fn manual_target_equal_to_observed_is_still_submitted() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 41.0);
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 41.0);
}

#[test]
fn manual_duplicate_raw_target_is_not_resubmitted() {
    let mut rig = Rig::new();
    // A 0..10 range maps 51 and 52 percent to the same raw value, 5.
    let narrow = ChannelObs {
        supported: true,
        observed: Some(5.0),
        min: 0.0,
        max: 10.0,
    };
    rig.observe(&obs(1, vec![mon_range("m1", narrow)]));
    rig.set("m1", Feature::Brightness, 51.0);
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 51.0);
    rig.submitted(subs[0].0, true);

    let shown = rig.set("m1", Feature::Brightness, 52.0);
    assert_eq!(shown, Some(view(true, 52.0, DraftStatus::Pending)));
    assert!(rig.after_drain().is_empty());

    rig.complete("m1", Feature::Brightness, 1, true, Some(5.0));
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);
    assert!(rig.after_drain().is_empty());
}

#[test]
fn manual_more_than_128_targets_are_capped() {
    let mut rig = Rig::new();
    let cap = MAX_DRAFTS;
    let ids: Vec<String> = (0..=cap).map(|index| format!("m{:03}", index)).collect();
    let monitors: Vec<MonitorObs> = ids
        .iter()
        .map(|id| mon(id, 1, "awake", 41.0, 20.0))
        .collect();
    rig.observe(&obs(1, monitors));
    for id in &ids[..cap] {
        assert!(rig.set(id, Feature::Brightness, 60.0).is_some());
    }
    assert_eq!(rig.set(&ids[cap], Feature::Brightness, 60.0), None);
    assert_eq!(submits(&rig.after_drain()).len(), cap);
    assert!(!rig.draft(&ids[cap], Feature::Brightness).has_draft);

    // A monitor that already has a draft can still be updated at the cap.
    let updated = rig.set(&ids[0], Feature::Brightness, 70.0);
    assert_eq!(updated.map(|shown| shown.draft), Some(70.0));
}

#[test]
fn manual_set_target_clamps_and_ignores_non_finite_values() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    assert_eq!(
        rig.set("m1", Feature::Brightness, 150.0)
            .map(|shown| shown.draft),
        Some(100.0)
    );
    assert_eq!(rig.set("m1", Feature::Brightness, f64::NAN), None);
    assert_eq!(
        rig.set("m1", Feature::Brightness, -5.0)
            .map(|shown| shown.draft),
        Some(0.0)
    );
    assert_eq!(rig.set("m1", Feature::Brightness, f64::INFINITY), None);
}

#[test]
fn manual_sleep_drops_queued_draft_and_wake_does_not_replay() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 60.0);

    let effects = rig.observe(&obs(2, vec![mon("m1", 1, "asleep", 41.0, 20.0)]));
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);
    assert!(changed(&effects, "m1", Feature::Brightness));
    assert!(rig.after_drain().is_empty());

    rig.observe(&obs(3, vec![base()]));
    assert!(rig.after_drain().is_empty());

    rig.set("m1", Feature::Brightness, 33.0);
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 33.0);
}

#[test]
fn manual_asleep_monitor_ignores_targets() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![mon("m1", 1, "asleep", 41.0, 20.0)]));
    assert_eq!(rig.set("m1", Feature::Brightness, 50.0), None);
    assert_eq!(rig.next_deadline(), None);
    assert!(rig.after_drain().is_empty());
}

#[test]
fn manual_disappearing_monitor_drops_its_drafts() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 25.0);
    rig.submit_accepted();
    rig.set("m1", Feature::Brightness, 60.0);

    rig.observe(&obs(2, Vec::new()));
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);
    assert!(rig.after_drain().is_empty());
}

#[test]
fn manual_generation_rollover_drops_old_generation_draft() {
    let mut rig = accepted_brightness(20.0);
    rig.observe(&obs(3, vec![mon("m1", 2, "awake", 41.0, 20.0)]));
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);

    rig.set("m1", Feature::Brightness, 70.0);
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["connection_generation"], 2);
    assert_eq!(subs[0].1["value"], 70.0);
}

#[test]
fn manual_shutdown_stops_submissions() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 25.0);
    rig.shutdown();
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);
    assert_eq!(rig.next_deadline(), None);
    assert_eq!(rig.set("m1", Feature::Brightness, 30.0), None);
    assert!(rig.after_drain().is_empty());
}

#[test]
fn manual_in_flight_command_is_released_only_after_safety_window() {
    let mut rig = Rig::new();
    rig.now = 0.98;
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 20.0);

    // The drain deadline of the first target, computed exactly as the machine does.
    rig.now = 0.98 + DRAIN_DELAY;
    let subs = submits(&rig.tick());
    assert_eq!(subs.len(), 1);
    rig.submitted(subs[0].0, true);

    rig.set("m1", Feature::Brightness, 37.0);
    rig.now = 2.5;
    rig.observe(&obs(2, vec![base()]));
    assert!(rig.after_drain().is_empty());

    rig.now = 4.5;
    rig.observe(&obs(3, vec![base()]));
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 37.0);
}

#[test]
fn manual_failed_completion_marks_draft_failed_at_observed_value() {
    let mut rig = accepted_brightness(62.0);
    let effects = rig.complete("m1", Feature::Brightness, 1, false, None);
    assert!(changed(&effects, "m1", Feature::Brightness));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 41.0, DraftStatus::Failed)
    );

    // A failed draft never retries on its own.
    assert!(rig.after_drain().is_empty());

    rig.set("m1", Feature::Brightness, 62.0);
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 62.0);
}

#[test]
fn manual_failed_completion_with_queued_target_sends_queued_target() {
    let mut rig = accepted_brightness(20.0);
    rig.set("m1", Feature::Brightness, 30.0);

    rig.now = 1.5;
    rig.complete("m1", Feature::Brightness, 1, false, None);
    // An already armed drain deadline is kept, so it may be earlier than now.
    assert!(rig.next_deadline().is_some_and(|d| d <= 1.5 + 1e-9));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 30.0, DraftStatus::Pending)
    );
    let subs = submits(&rig.tick());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 30.0);
}

#[test]
fn manual_completion_ignored_for_unknown_monitor_or_idle_slot() {
    let mut rig = accepted_brightness(20.0);
    rig.set("m1", Feature::Brightness, 30.0);

    // A completion for a monitor that is not known cannot belong to this slot.
    let effects = rig.complete("monitor-missing", Feature::Brightness, 1, true, Some(20.0));
    assert!(effects.is_empty());
    assert!(rig.after_drain().is_empty());
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 30.0, DraftStatus::Pending)
    );

    rig.complete("m1", Feature::Brightness, 1, true, Some(20.0));
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 30.0);
    rig.submitted(subs[0].0, true);
    rig.complete("m1", Feature::Brightness, 1, true, Some(30.0));
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);

    // Nothing in flight and nothing queued: the completion changes nothing.
    let effects = rig.complete("m1", Feature::Brightness, 1, false, None);
    assert!(effects.is_empty());
    let idle = rig.draft("m1", Feature::Brightness);
    assert!(!idle.has_draft);
    assert_eq!(idle.status, DraftStatus::Ready);
}

// The core numbers action generations by snapshot revision, so a pending action and its
// completion carry a generation that differs from the monitor's connection generation (1).
#[test]
fn manual_completion_and_pending_match_by_monitor_not_action_generation() {
    let mut rig = accepted_brightness(20.0);

    // The core lists the command as action generation 8 on connection generation 1.
    let listed = vec![action(8, 8, "m1", "set_brightness", 20.0)];
    rig.observe(&obs_pending(2, vec![base()], listed));
    rig.set("m1", Feature::Brightness, 37.0);
    assert!(rig.after_drain().is_empty());

    // The completion releases the slot, but the core still lists the action, so the target waits.
    rig.complete("m1", Feature::Brightness, 1, true, Some(20.0));
    assert!(rig.after_drain().is_empty());

    // Once the core stops listing the action, the queued target goes out on connection 1.
    rig.observe(&obs(3, vec![base()]));
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 37.0);
    assert_eq!(subs[0].1["connection_generation"], 1);
}

#[test]
fn manual_monitors_and_features_submit_independently() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base(), mon("m2", 1, "awake", 41.0, 20.0)]));
    rig.set("m1", Feature::Brightness, 70.0);
    rig.set("m1", Feature::Volume, 30.0);
    rig.set("m2", Feature::Brightness, 80.0);

    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 3);
    assert!(has_submit(&subs, "m1", "set_brightness"));
    assert!(has_submit(&subs, "m1", "set_volume"));
    assert!(has_submit(&subs, "m2", "set_brightness"));
}

#[test]
fn manual_late_completion_from_older_connection_generation_is_ignored() {
    let mut rig = accepted_brightness(20.0);

    // Generation 2 replaces the slot, so the generation 1 command is gone.
    rig.observe(&obs(2, vec![mon("m1", 2, "awake", 41.0, 20.0)]));
    rig.set("m1", Feature::Brightness, 70.0);
    let subs = submits(&rig.after_drain());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["connection_generation"], 2);
    rig.submitted(subs[0].0, true);
    rig.set("m1", Feature::Brightness, 80.0);

    // A late generation 1 completion must not release the generation 2 command.
    rig.complete("m1", Feature::Brightness, 1, true, Some(20.0));
    assert!(rig.after_drain().is_empty());

    rig.complete("m1", Feature::Brightness, 2, true, Some(70.0));
    let subs = submits(&rig.tick());
    assert_eq!(subs.len(), 1);
    assert_eq!(subs[0].1["value"], 80.0);
    assert_eq!(subs[0].1["connection_generation"], 2);
}

#[test]
fn manual_observe_with_lower_revision_is_ignored() {
    let mut rig = Rig::new();
    rig.observe(&obs(5, vec![base()]));

    // A stale snapshot with a different brightness does not change the observed value.
    rig.observe(&obs(4, vec![mon("m1", 1, "awake", 30.0, 20.0)]));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(false, 41.0, DraftStatus::Ready)
    );

    rig.set("m1", Feature::Brightness, 60.0);
    // A stale asleep state does not drop the draft.
    rig.observe(&obs(4, vec![mon("m1", 1, "asleep", 41.0, 20.0)]));
    assert!(rig.draft("m1", Feature::Brightness).has_draft);

    // An equal revision is applied.
    rig.observe(&obs(5, vec![mon("m1", 1, "asleep", 41.0, 20.0)]));
    assert!(!rig.draft("m1", Feature::Brightness).has_draft);
}

#[test]
fn manual_draft_changed_reports_completion_failure_and_readback() {
    // Success clears the draft.
    let mut rig = accepted_brightness(37.0);
    let effects = rig.complete("m1", Feature::Brightness, 1, true, Some(37.0));
    assert!(changed(&effects, "m1", Feature::Brightness));

    // Failure marks the draft failed.
    let mut rig = accepted_brightness(62.0);
    let effects = rig.complete("m1", Feature::Brightness, 1, false, None);
    assert!(changed(&effects, "m1", Feature::Brightness));

    // A readback within tolerance clears the draft.
    let mut rig = accepted_brightness(37.0);
    let effects = rig.observe(&obs(2, vec![mon("m1", 1, "awake", 37.0, 20.0)]));
    assert!(changed(&effects, "m1", Feature::Brightness));
}

/// The display state of the fixture monitor's brightness channel at generation 1.
fn brightness_state(manual: &Manual) -> DisplayState {
    manual
        .display_state("m1", 1, Feature::Brightness)
        .expect("m1 is known at generation 1")
}

#[test]
fn manual_take_accepted_reports_the_in_flight_value_once() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 37.0);
    let token = rig.submit_one();
    rig.submitted(token, true);
    assert_eq!(
        rig.manual.take_accepted(),
        vec![("m1".to_owned(), Feature::Brightness, 1, 37.0)]
    );
    assert!(rig.manual.take_accepted().is_empty());

    // A rejected submission logs nothing.
    rig.complete("m1", Feature::Brightness, 1, true, Some(37.0));
    rig.set("m1", Feature::Brightness, 50.0);
    let token = rig.submit_one();
    rig.submitted(token, false);
    assert!(rig.manual.take_accepted().is_empty());
}

#[test]
fn manual_display_state_reflects_channel_and_slot() {
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    assert_eq!(
        brightness_state(&rig.manual),
        DisplayState {
            percent: 41.0,
            supported: true,
            awake: true,
            pending: false,
            failed: false,
        }
    );
    assert_eq!(rig.manual.display_state("m1", 2, Feature::Brightness), None);
    assert_eq!(
        rig.manual.display_state("missing", 1, Feature::Brightness),
        None
    );

    // A pending set_brightness action on the monitor shows as pending until it clears.
    let pending = vec![action(7, 1, "m1", "set_brightness", 41.0)];
    rig.observe(&obs_pending(2, vec![base()], pending));
    assert!(brightness_state(&rig.manual).pending);
    rig.observe(&obs(3, vec![base()]));
    assert!(!brightness_state(&rig.manual).pending);

    // A rejected submission marks the slot failed.
    rig.set("m1", Feature::Brightness, 62.0);
    let token = rig.submit_one();
    rig.submitted(token, false);
    let state = brightness_state(&rig.manual);
    assert!(state.failed);
    assert_eq!(state.percent, 41.0);

    // A sleeping monitor is neither supported nor awake, and its draft is gone.
    rig.observe(&obs(4, vec![mon("m1", 1, "asleep", 41.0, 20.0)]));
    let state = brightness_state(&rig.manual);
    assert!(!state.supported);
    assert!(!state.awake);
    assert!(!state.failed);

    // An unknown observed value reads as NaN.
    let mut unknown = base();
    unknown.brightness.observed = None;
    rig.observe(&obs(5, vec![unknown]));
    assert!(brightness_state(&rig.manual).percent.is_nan());
}

/// A readback within tolerance clears the draft while its command is still in flight, and a later
/// failure re-creates the draft at that observed value.
/// Preserved from the Windows C++ original, not a design goal.
#[test]
fn known_quirk_readback_clears_a_draft_while_the_command_is_in_flight_then_failure_recreates_it() {
    let mut rig = accepted_brightness(37.0);
    rig.observe(&obs(2, vec![mon("m1", 1, "awake", 37.2, 20.0)]));
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(false, 37.2, DraftStatus::Ready)
    );

    rig.complete("m1", Feature::Brightness, 1, false, None);
    assert_eq!(
        rig.draft("m1", Feature::Brightness),
        view(true, 37.2, DraftStatus::Failed)
    );
}

#[test]
fn manual_deadline_is_cleared_when_the_queued_target_is_dropped() {
    // The monitor sleeps while its target waits: nothing is left to drain.
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 60.0);
    assert!(rig.next_deadline().is_some());
    rig.observe(&obs(2, vec![mon("m1", 1, "asleep", 41.0, 20.0)]));
    assert_eq!(rig.next_deadline(), None);

    // The monitor disappears from the observation while its target waits.
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base()]));
    rig.set("m1", Feature::Brightness, 60.0);
    rig.observe(&obs(2, Vec::new()));
    assert_eq!(rig.next_deadline(), None);

    // Another monitor's queued target keeps the deadline.
    let mut rig = Rig::new();
    rig.observe(&obs(1, vec![base(), mon("m2", 1, "awake", 41.0, 20.0)]));
    rig.set("m1", Feature::Brightness, 60.0);
    rig.set("m2", Feature::Brightness, 70.0);
    rig.observe(&obs(2, vec![mon("m2", 1, "awake", 41.0, 20.0)]));
    assert_eq!(rig.next_deadline(), Some(1.0 + DRAIN_DELAY));
}

#[test]
fn manual_deadline_is_cleared_when_a_drag_ends_on_the_in_flight_value() {
    // A 0..10 range maps 51 and 52 percent to the same raw value, 5.
    let mut rig = Rig::new();
    let narrow = ChannelObs {
        supported: true,
        observed: Some(5.0),
        min: 0.0,
        max: 10.0,
    };
    rig.observe(&obs(1, vec![mon_range("m1", narrow)]));
    rig.set("m1", Feature::Brightness, 51.0);
    let token = rig.submit_one();
    rig.submitted(token, true);

    // A different target queues behind the in-flight command and arms the drain deadline.
    rig.set("m1", Feature::Brightness, 80.0);
    assert!(rig.next_deadline().is_some());

    // The drag returns to the in-flight raw value: nothing is queued, so no deadline may remain.
    rig.set("m1", Feature::Brightness, 52.0);
    assert_eq!(rig.next_deadline(), None);
    assert!(rig.after_drain().is_empty());
}
