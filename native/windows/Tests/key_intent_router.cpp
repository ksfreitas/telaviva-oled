// Tests for the volume and mute key router. Ported from the volume and mute cases of
// native/macos/Tests/HardwareKeyRouterTests.swift; brightness keys are out of scope on Windows.
#include "TestHarness.h"
#include "Fakes.h"
#include "../Platform/KeyIntentRouter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace telaviva {
namespace {

const std::string kA = "monitor-a";

// Returned by pressOnce when the press wrote nothing.
constexpr double kNoWrite = -1.0;

VolumeObservation observed(double percent, uint64_t generation = 1, const std::string& monitorId = kA)
{
    VolumeObservation observation;
    observation.monitorId = monitorId;
    observation.connectionGeneration = generation;
    observation.percent = percent;
    observation.supported = true;
    observation.awake = true;
    return observation;
}

KeyEvent keyEvent(VolumeKey key, KeyPhase phase = KeyPhase::Down, bool isRepeat = false,
                  bool fine = false, bool eligible = true)
{
    KeyEvent event;
    event.key = key;
    event.phase = phase;
    event.isRepeat = isRepeat;
    event.isFineStep = fine;
    event.modifiersEligible = eligible;
    return event;
}

KeyRouteConfig route(double step = kKeyDefaultStep, const std::string& owner = kA)
{
    KeyRouteConfig config;
    config.enabled = true;
    config.ownerMonitorId = owner;
    config.step = step;
    return config;
}

KeyIntent firstIntent(const std::vector<KeyIntent>& intents)
{
    return intents.empty() ? KeyIntent{} : intents.front();
}

struct Consumed {
    bool down = false;
    bool repeat = false;
    bool up = false;
};

// Drives one full down, repeat and up sequence and reports what was consumed.
Consumed hold(KeyIntentRouter& router, VolumeKey key)
{
    Consumed result;
    result.down = router.handleKey(keyEvent(key, KeyPhase::Down));
    result.repeat = router.handleKey(keyEvent(key, KeyPhase::Down, true));
    result.up = router.handleKey(keyEvent(key, KeyPhase::Up));
    return result;
}

bool consumedAny(const Consumed& consumed)
{
    return consumed.down || consumed.repeat || consumed.up;
}

// One press on a fresh router that owns a monitor at the given level. Returns the written target, or kNoWrite.
double pressOnce(VolumeKey key, double percent, double step, bool fine = false)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route(step));
    router.observe(observed(percent));
    CHECK(router.handleKey(keyEvent(key, KeyPhase::Down, false, fine)));
    const std::vector<KeyIntent> intents = router.takeIntents();
    return intents.size() == 1 ? intents[0].targetVolume : kNoWrite;
}

// Holds a key on a fresh router with the given owner and observation; reports consumption.
// Asserts that no write is handed out unless expectWrite is set.
Consumed holdFor(const std::string& owner, const VolumeObservation& observation, VolumeKey key,
                 bool expectWrite = false)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route(kKeyDefaultStep, owner));
    router.observe(observation);
    const Consumed consumed = hold(router, key);
    if (expectWrite) {
        CHECK(!router.takeIntents().empty());
    } else {
        CHECK(router.takeIntents().empty());
    }
    return consumed;
}

TEST(key_step_policy)
{
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, true, false, 5.0), 55.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, false, false, 5.0), 45.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, true, false, 15.0), 65.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, false, false, 1.0), 49.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, true, true, 15.0), 51.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, false, true, 15.0), 49.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(50.0, false, true, 1.0), 49.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(95.0, true, false, 15.0), 100.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(5.0, false, false, 15.0), 0.0);
    CHECK_EQ(KeyIntentRouter::steppedVolume(100.0, true, true, 15.0), 100.0);
    CHECK_EQ(kKeyDefaultStep, 5.0);
    CHECK_EQ(kKeyMinStep, 1.0);
    CHECK_EQ(kKeyMaxStep, 15.0);
    CHECK_EQ(kKeyFineStep, 1.0);
    CHECK_EQ(KeyIntentRouter::clampedStep(0.0), 1.0);
    CHECK_EQ(KeyIntentRouter::clampedStep(99.0), 15.0);
    CHECK_EQ(KeyIntentRouter::clampedStep(std::numeric_limits<double>::quiet_NaN()), 5.0);
    CHECK_EQ(KeyIntentRouter::clampedStep(std::numeric_limits<double>::infinity()), 5.0);
    CHECK_EQ(KeyIntentRouter::clampedStep(7.4), 7.0);
    CHECK_EQ(KeyIntentRouter::clampedStep(7.6), 8.0);
}

TEST(configured_step_bounds_apply_to_presses)
{
    CHECK_EQ(pressOnce(VolumeKey::Down, 50.0, 0.0), 49.0);
    CHECK_EQ(pressOnce(VolumeKey::Down, 50.0, 99.0), 35.0);
    CHECK_EQ(pressOnce(VolumeKey::Down, 50.0, std::numeric_limits<double>::quiet_NaN()), 45.0);
    CHECK_EQ(pressOnce(VolumeKey::Down, 50.0, 7.4), 43.0);
    CHECK_EQ(pressOnce(VolumeKey::Up, 50.0, 15.0), 65.0);
    // Shift+Option moves exactly one point whatever the configured step is.
    CHECK_EQ(pressOnce(VolumeKey::Up, 50.0, 15.0, true), 51.0);
    CHECK_EQ(pressOnce(VolumeKey::Down, 50.0, 15.0, true), 49.0);
}

TEST(volume_boundaries_consume_without_write)
{
    CHECK_EQ(pressOnce(VolumeKey::Up, 100.0, 15.0), kNoWrite);
    CHECK_EQ(pressOnce(VolumeKey::Down, 0.0, 15.0), kNoWrite);
    CHECK_EQ(pressOnce(VolumeKey::Up, 95.0, 15.0), 100.0);
    CHECK_EQ(pressOnce(VolumeKey::Down, 5.0, 15.0), 0.0);

    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(100.0));
    const Consumed consumed = hold(router, VolumeKey::Up);
    CHECK(consumed.down && consumed.repeat && consumed.up);
    CHECK(router.takeIntents().empty());
}

TEST(owner_selects_only_an_eligible_observation)
{
    CHECK(!holdFor("", observed(50.0), VolumeKey::Up).down);
    CHECK(!holdFor(kA, observed(50.0, 1, "monitor-b"), VolumeKey::Up).down);
    VolumeObservation unsupported = observed(50.0);
    unsupported.supported = false;
    CHECK(!holdFor(kA, unsupported, VolumeKey::Up).down);
    VolumeObservation asleep = observed(50.0);
    asleep.awake = false;
    CHECK(!holdFor(kA, asleep, VolumeKey::Up).down);
    CHECK(!holdFor(kA, observed(std::numeric_limits<double>::quiet_NaN()), VolumeKey::Up).down);
    CHECK(!holdFor(kA, observed(101.0), VolumeKey::Up).down);
    CHECK(!holdFor(kA, observed(-1.0), VolumeKey::Up).down);
    CHECK(holdFor(kA, observed(50.0), VolumeKey::Up, true).down);
}

TEST(no_owner_passes_whole_sequence_through)
{
    CHECK(!consumedAny(holdFor("", observed(50.0), VolumeKey::Up)));
    CHECK(!consumedAny(holdFor("", observed(50.0), VolumeKey::Down)));
    CHECK(!consumedAny(holdFor("", observed(50.0), VolumeKey::Mute)));
}

TEST(held_key_coalesces_repeats_into_one_follow_up_write)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    CHECK(first.monitorId == kA);
    CHECK(first.targetVolume == 55.0);
    CHECK(first.generation == uint64_t{1});
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    // The release has ineligible modifiers, but the latched press stays consumed.
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up, false, false, false)));
    CHECK(router.takeIntents().empty());
    router.intentFinished(first, true);
    const std::vector<KeyIntent> second = router.takeIntents();
    CHECK(second.size() == 1 && second[0].targetVolume == 65.0);
    router.intentFinished(firstIntent(second), true);
    CHECK(router.takeIntents().empty());
}

TEST(latch_decides_whole_sequence_per_key)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    // An ineligible press passes through, and so do its repeat and release.
    CHECK(!router.handleKey(keyEvent(VolumeKey::Down, KeyPhase::Down, false, false, false)));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Down, KeyPhase::Down, true)));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Down, KeyPhase::Up)));
    CHECK(router.takeIntents().empty());
    // A handled press stays consumed even when its release has ineligible modifiers.
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up, false, false, false)));
    const std::vector<KeyIntent> intents = router.takeIntents();
    CHECK(intents.size() == 1 && intents[0].targetVolume == 55.0);
    // A release without a press passes through.
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
}

TEST(orphan_repeat_passes_through)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(router.takeIntents().empty());
}

TEST(held_hold_is_cancelled_when_owner_leaves_mid_hold)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    CHECK(first.targetVolume == 55.0);
    // The sound output moved to another device while the key is held.
    router.configure(route(kKeyDefaultStep, ""));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    router.intentFinished(first, true);
    CHECK(router.takeIntents().empty());
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));

    // A press that passed through stays passed through even if the output returns mid-hold.
    KeyIntentRouter late(clock);
    late.configure(route(kKeyDefaultStep, ""));
    late.observe(observed(50.0));
    CHECK(!late.handleKey(keyEvent(VolumeKey::Up)));
    late.configure(route());
    CHECK(!late.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(!late.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    CHECK(late.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(late.takeIntents().size() == 1);
}

TEST(stale_generation_cancels_hold)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    // The monitor reconnected with a new generation.
    router.observe(observed(50.0, 2));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    router.intentFinished(first, true);
    CHECK(router.takeIntents().empty());
}

TEST(rejected_submission_leaves_release_consumed)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.intentFinished(first, false);
    CHECK(!router.isIntentCurrent(first));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    CHECK(router.takeIntents().empty());
    // The next press starts again from the readback.
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 55.0);
}

TEST(manual_change_invalidates_handed_off_intent)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent old = firstIntent(router.takeIntents());
    CHECK(router.isIntentCurrent(old));
    router.manualControlChanged(kA, 1, 20.0, 1);
    CHECK(!router.isIntentCurrent(old));
    router.intentFinished(old, false);
    CHECK(router.takeIntents().empty());
    // The held key continues from the slider value.
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    const KeyIntent current = firstIntent(router.takeIntents());
    CHECK(current.targetVolume == 25.0);
    CHECK(router.isIntentCurrent(current));

    router.shutdown();
    CHECK(!router.isIntentCurrent(current));
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Down)));
}

TEST(manual_rebase_is_used_by_next_key)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    CHECK(first.targetVolume == 55.0);
    router.intentFinished(first, true);
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));

    router.manualControlChanged(kA, 1, 20.0, 1);
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent next = firstIntent(router.takeIntents());
    CHECK(next.targetVolume == 25.0);
    router.intentFinished(next, false);
}

TEST(late_slider_acceptance_cannot_rebase_newer_key_target)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    router.manualControlChanged(kA, 1, 20.0, 42);
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    CHECK(first.targetVolume == 25.0);
    router.manualControlAccepted(kA, 1, 20.0, 42);
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    router.intentFinished(first, true);
    const KeyIntent second = firstIntent(router.takeIntents());
    CHECK(second.targetVolume == 30.0);
    router.intentFinished(second, false);
}

TEST(rebasing_is_bounded_and_coalesces_superseded_epochs)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    for (uint64_t revision = 1; revision <= 100; ++revision) {
        router.manualControlChanged(kA, 1, 40.0, revision);
        CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    }
    // One write stays in flight; nothing more is handed out while it runs.
    CHECK(router.takeIntents().empty());
    router.intentFinished(first, false);
    const std::vector<KeyIntent> latest = router.takeIntents();
    CHECK(latest.size() == 1 && latest[0].targetVolume == 45.0);
    router.intentFinished(firstIntent(latest), true);
    CHECK(router.takeIntents().empty());
}

TEST(external_readback_rebases_idle_intent)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.intentFinished(first, true);
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    // The level changed elsewhere while idle, beyond the readback tolerance.
    router.observe(observed(56.0));
    router.observe(observed(30.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 35.0);
}

TEST(readback_within_tolerance_does_not_rebase)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.intentFinished(first, true);
    router.observe(observed(55.4));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 60.0);
}

TEST(step_change_applies_to_next_press_and_held_repeat)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    CHECK(first.targetVolume == 55.0);
    router.intentFinished(first, true);
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));

    router.configure(route(12.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent second = firstIntent(router.takeIntents());
    CHECK(second.targetVolume == 67.0);

    // The key is still held while the step changes again; the repeat uses the step current at that repeat.
    router.configure(route(3.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    CHECK(router.takeIntents().empty());
    router.intentFinished(second, true);
    const KeyIntent third = firstIntent(router.takeIntents());
    CHECK(third.targetVolume == 70.0);
    router.intentFinished(third, true);
}

TEST(own_write_outstanding_blocks_rebase)
{
    FakeClock clock;
    clock.setSteadySeconds(1.0);
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.actionAccepted(kA, 1, 7);
    router.intentFinished(first, true);
    // The readback still shows the old level while our write is outstanding.
    router.observe(observed(20.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 60.0);
}

TEST(settle_window_ignores_readback_then_external_change_rebases)
{
    FakeClock clock;
    clock.setSteadySeconds(1.0);
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.actionAccepted(kA, 1, 7);
    router.intentFinished(first, true);
    clock.setSteadySeconds(1.1);
    router.actionCompleted(kA, 1, 7);  // settle window until 2.1 s
    clock.setSteadySeconds(1.5);
    router.observe(observed(20.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    const KeyIntent second = firstIntent(router.takeIntents());
    CHECK(second.targetVolume == 60.0);
    router.intentFinished(second, true);

    clock.setSteadySeconds(2.2);
    router.observe(observed(20.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 25.0);
}

TEST(unfinished_write_expires_after_ten_seconds)
{
    FakeClock clock;
    clock.setSteadySeconds(1.0);
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.actionAccepted(kA, 1, 7);
    router.intentFinished(first, true);

    clock.setSteadySeconds(10.999);  // one millisecond before expiry
    router.observe(observed(20.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Down, true)));
    const KeyIntent second = firstIntent(router.takeIntents());
    CHECK(second.targetVolume == 60.0);
    router.intentFinished(second, true);

    clock.setSteadySeconds(11.001);  // one millisecond after expiry
    router.observe(observed(20.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 25.0);
}

TEST(completion_before_acceptance_cancels_it)
{
    FakeClock clock;
    clock.setSteadySeconds(1.0);
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    const KeyIntent first = firstIntent(router.takeIntents());
    router.actionCompleted(kA, 1, 9);
    router.actionAccepted(kA, 1, 9);  // cancels the early completion
    router.intentFinished(first, true);

    clock.setSteadySeconds(2.1);
    router.observe(observed(20.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 25.0);
}

TEST(mute_restore_uses_volume_step)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route(12.0));
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(muted.targetVolume == 0.0);
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);

    router.observe(observed(0.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(firstIntent(router.takeIntents()).targetVolume == 62.0);
}

TEST(mute_writes_zero_once)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(muted.targetVolume == 0.0);
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Down, true)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);
    CHECK(router.takeIntents().empty());
}

TEST(mute_restores_remembered_level)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);

    router.observe(observed(0.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent restored = firstIntent(router.takeIntents());
    CHECK(restored.targetVolume == 50.0);
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(restored, true);
}

TEST(volume_up_while_muted_steps_from_remembered_level)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);

    router.observe(observed(0.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));
    const KeyIntent unmuted = firstIntent(router.takeIntents());
    CHECK(unmuted.targetVolume == 55.0);
    router.intentFinished(unmuted, true);
}

TEST(volume_down_while_muted_writes_nothing)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);

    router.observe(observed(0.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Down)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Down, KeyPhase::Up)));
    CHECK(router.takeIntents().empty());
}

TEST(mute_at_zero_with_nothing_remembered_writes_nothing)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(0.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    CHECK(router.takeIntents().empty());
}

TEST(external_change_discards_remembered_mute_level)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);

    router.observe(observed(30.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent rewritten = firstIntent(router.takeIntents());
    CHECK(rewritten.targetVolume == 0.0);
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(rewritten, true);
}

TEST(output_round_trip_forgets_remembered_mute_level)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);

    router.configure(route(kKeyDefaultStep, ""));
    router.configure(route());
    router.observe(observed(0.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    CHECK(router.takeIntents().empty());
}

TEST(mute_and_volume_pass_through_without_supported_owner)
{
    VolumeObservation unsupported = observed(50.0);
    unsupported.supported = false;
    VolumeObservation asleep = observed(50.0);
    asleep.awake = false;
    CHECK(!consumedAny(holdFor("", observed(50.0), VolumeKey::Mute)));
    CHECK(!consumedAny(holdFor(kA, unsupported, VolumeKey::Mute)));
    CHECK(!consumedAny(holdFor(kA, asleep, VolumeKey::Mute)));
    CHECK(!consumedAny(holdFor(kA, observed(std::numeric_limits<double>::quiet_NaN()), VolumeKey::Mute)));
    CHECK(!consumedAny(holdFor(kA, unsupported, VolumeKey::Up)));
}

TEST(mute_with_disallowed_modifiers_passes_through)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Down, false, false, false)));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up, false, false, false)));
    CHECK(router.takeIntents().empty());
}

TEST(held_mute_cancelled_by_output_switch)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute)));
    const KeyIntent muted = firstIntent(router.takeIntents());
    router.configure(route(kKeyDefaultStep, ""));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Down, true)));
    CHECK(router.handleKey(keyEvent(VolumeKey::Mute, KeyPhase::Up)));
    router.intentFinished(muted, true);
    CHECK(router.takeIntents().empty());
}

TEST(disable_and_shutdown_clear_owner)
{
    FakeClock clock;
    KeyIntentRouter router(clock);
    router.configure(route());
    router.observe(observed(50.0));
    KeyRouteConfig disabled = route();
    disabled.enabled = false;
    router.configure(disabled);
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up, KeyPhase::Up)));

    router.shutdown();
    router.configure(route());
    router.observe(observed(50.0));
    CHECK(!router.handleKey(keyEvent(VolumeKey::Up)));
    CHECK(router.takeIntents().empty());
}

}  // namespace
}  // namespace telaviva
