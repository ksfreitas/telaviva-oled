#include "TestHarness.h"

#include "../Platform/IKeyHook.h"
#include "../Platform/KeyHook.h"
#include "../Platform/KeyRoute.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using telaviva::KeyEventQueue;
using telaviva::KeyHookCallback;
using telaviva::KeyHookController;
using telaviva::KeyHookEvent;
using telaviva::KeyHookPolicy;
using telaviva::KeyLatch;
using telaviva::KeyQueueEntry;
using telaviva::KeyRoute;
using telaviva::KeyRoutePublisher;
using telaviva::KeyWatchdog;
using telaviva::shouldSwallow;

namespace {

// Test double for the OS hook: install, uninstall and inject events through the callback.
class FakeKeyHook final : public telaviva::IKeyHook {
public:
    bool install(KeyHookCallback callback) override
    {
        if (refuse) {
            return false;
        }
        ++installs;
        callback_ = std::move(callback);
        installed_ = true;
        return true;
    }

    void uninstall() override
    {
        if (installed_) {
            ++uninstalls;
        }
        installed_ = false;
    }

    bool isInstalled() const override { return installed_; }

    // Fixed id, so tests can check that the id reaches the caller through the seam.
    static constexpr uint32_t kThreadId = 4242;
    uint32_t threadId() const override { return kThreadId; }

    // Returns the callback's decision, or false when not installed.
    bool inject(const KeyHookEvent& event)
    {
        return installed_ && callback_ && callback_(event);
    }

    bool refuse = false;
    int installs = 0;
    int uninstalls = 0;

private:
    bool installed_ = false;
    KeyHookCallback callback_;
};

KeyRoute readyRoute(uint64_t version)
{
    KeyRoute route;
    route.hasOwner = true;
    route.volumeSupported = true;
    route.displayAwake = true;
    route.version = version;
    return route;
}

KeyHookCallback callbackFor(KeyHookPolicy& policy)
{
    return [&policy](const KeyHookEvent& event) { return policy.onEvent(event); };
}

// Sends a full press (down then up) and returns the down decision. The up must match it.
bool tap(FakeKeyHook& hook, uint32_t vk)
{
    const bool down = hook.inject(KeyHookEvent{vk, true, false});
    const bool up = hook.inject(KeyHookEvent{vk, false, false});
    CHECK_EQ(up, down);
    return down;
}

}  // namespace

TEST(key_route_round_trip_keeps_every_field)
{
    KeyRoutePublisher publisher;
    CHECK(!publisher.read().hasOwner);
    CHECK_EQ(publisher.read().version, uint64_t{0});

    KeyRoute route;
    route.hasOwner = true;
    route.volumeSupported = false;
    route.displayAwake = true;
    route.version = uint64_t{123456789012ULL};
    publisher.publish(route);

    const KeyRoute back = publisher.read();
    CHECK_EQ(back.hasOwner, true);
    CHECK_EQ(back.volumeSupported, false);
    CHECK_EQ(back.displayAwake, true);
    CHECK_EQ(back.version, uint64_t{123456789012ULL});
}

TEST(key_route_publish_during_reads_is_never_torn)
{
    KeyRoutePublisher publisher;
    std::atomic<bool> stop{false};
    std::atomic<long long> reads{0};
    std::atomic<long long> torn{0};

    // The three flags are correlated with the version parity, so a mixed value is detectable.
    std::thread writer([&publisher, &stop] {
        uint64_t version = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ++version;
            const bool on = (version & 1) != 0;
            publisher.publish(KeyRoute{on, on, on, version});
        }
    });
    std::thread reader([&publisher, &stop, &reads, &torn] {
        while (!stop.load(std::memory_order_relaxed)) {
            const KeyRoute route = publisher.read();
            const bool on = (route.version & 1) != 0;
            if (route.hasOwner != on || route.volumeSupported != on || route.displayAwake != on) {
                ++torn;
            }
            ++reads;
        }
    });

    std::this_thread::sleep_for(50ms);
    stop = true;
    writer.join();
    reader.join();

    CHECK_EQ(torn.load(), 0LL);
    CHECK(reads.load() > 0);
}

TEST(key_route_should_swallow_decision_table)
{
    // Only the all-true row swallows; enabled state is handled by installing the hook.
    for (unsigned mask = 0; mask < 8; ++mask) {
        KeyRoute route;
        route.hasOwner = (mask & 1) != 0;
        route.volumeSupported = (mask & 2) != 0;
        route.displayAwake = (mask & 4) != 0;
        CHECK_EQ(shouldSwallow(route), mask == 7);
    }
}

TEST(key_latch_down_repeats_and_up_share_one_decision)
{
    KeyLatch latch;
    CHECK(latch.decide(telaviva::kVkVolumeUp, true, true));
    // Repeats and the up keep the key-down decision even though the route changed.
    CHECK(latch.decide(telaviva::kVkVolumeUp, true, false));
    CHECK(latch.decide(telaviva::kVkVolumeUp, true, false));
    CHECK(latch.decide(telaviva::kVkVolumeUp, false, false));
    // The up cleared the latch, so the next press reads the route again.
    CHECK(!latch.decide(telaviva::kVkVolumeUp, true, false));
    CHECK(!latch.decide(telaviva::kVkVolumeUp, false, true));
}

TEST(key_latch_route_change_mid_sequence_keeps_decision)
{
    KeyLatch latch;
    CHECK(!latch.decide(telaviva::kVkVolumeUp, true, false));
    // The route became swallowing mid-press; the press stays passed through.
    CHECK(!latch.decide(telaviva::kVkVolumeUp, true, true));
    CHECK(!latch.decide(telaviva::kVkVolumeUp, false, true));
    // A new press picks up the new route.
    CHECK(latch.decide(telaviva::kVkVolumeUp, true, true));
    CHECK(latch.decide(telaviva::kVkVolumeUp, false, false));
}

TEST(key_latch_up_without_down_passes_through)
{
    KeyLatch latch;
    CHECK(!latch.decide(telaviva::kVkVolumeMute, false, true));
    CHECK(!latch.decide(telaviva::kVkVolumeMute, false, true));
}

TEST(key_latch_ignores_other_keys)
{
    KeyLatch latch;
    CHECK(!latch.decide(0x41, true, true));
    CHECK(!latch.decide(0x41, false, true));
    // An up for a volume key with no down seen passes through.
    CHECK(!latch.decide(telaviva::kVkVolumeDown, false, true));
}

TEST(key_latch_keys_are_independent)
{
    KeyLatch latch;
    CHECK(latch.decide(telaviva::kVkVolumeUp, true, true));
    CHECK(!latch.decide(telaviva::kVkVolumeDown, true, false));
    CHECK(latch.decide(telaviva::kVkVolumeUp, true, false));
    CHECK(!latch.decide(telaviva::kVkVolumeDown, false, true));
    CHECK(latch.decide(telaviva::kVkVolumeUp, false, false));
}

TEST(key_event_queue_full_drops_and_recovers)
{
    KeyEventQueue queue;
    const KeyQueueEntry entry{telaviva::kVkVolumeUp, true, 1};
    for (std::size_t i = 0; i < KeyEventQueue::kCapacity; ++i) {
        CHECK(queue.push(entry));
    }
    CHECK(!queue.push(entry));

    KeyQueueEntry out{};
    CHECK(queue.pop(out));
    CHECK(queue.push(entry));

    std::size_t drained = 0;
    while (queue.pop(out)) {
        ++drained;
    }
    CHECK_EQ(drained, KeyEventQueue::kCapacity);
    CHECK(!queue.pop(out));
}

TEST(key_watchdog_fires_only_when_held_and_silent)
{
    KeyWatchdog watchdog(1000);
    // Nothing held: nothing to watch.
    CHECK(!watchdog.due(5000));

    watchdog.onKey(5000, telaviva::kVkVolumeUp, true);
    CHECK(watchdog.held());
    CHECK(!watchdog.due(5999));
    CHECK(watchdog.due(6000));
    // The window restarts at the due point, so a dead hook is reinstalled once per window.
    CHECK(!watchdog.due(6500));
    CHECK(watchdog.due(7000));

    // Releasing the key stops the watch.
    watchdog.onKey(7100, telaviva::kVkVolumeUp, false);
    CHECK(!watchdog.held());
    CHECK(!watchdog.due(20000));
}

TEST(key_watchdog_ignores_other_keys_and_reset_clears_held)
{
    KeyWatchdog watchdog(1000);
    watchdog.onKey(0, 0x41, true);
    CHECK(!watchdog.held());
    CHECK(!watchdog.due(10000));

    watchdog.onKey(0, telaviva::kVkVolumeMute, true);
    CHECK(watchdog.held());
    watchdog.reset();
    CHECK(!watchdog.held());
    CHECK(!watchdog.due(10000));
}

TEST(key_hook_controller_installs_only_when_enabled)
{
    FakeKeyHook hook;
    KeyHookController controller(hook, [](const KeyHookEvent&) { return false; });

    CHECK(controller.setEnabled(false));
    CHECK(!hook.isInstalled());
    CHECK_EQ(hook.installs, 0);

    CHECK(controller.setEnabled(true));
    CHECK(hook.isInstalled());
    CHECK_EQ(hook.installs, 1);
    CHECK(controller.setEnabled(true));
    CHECK_EQ(hook.installs, 1);

    CHECK(controller.setEnabled(false));
    CHECK(!hook.isInstalled());
    CHECK_EQ(hook.uninstalls, 1);
    CHECK(controller.setEnabled(false));
    CHECK_EQ(hook.uninstalls, 1);
}

TEST(key_hook_controller_reports_refused_install_and_retries)
{
    FakeKeyHook hook;
    hook.refuse = true;
    KeyHookController controller(hook, [](const KeyHookEvent&) { return false; });

    CHECK(!controller.setEnabled(true));
    CHECK(!hook.isInstalled());

    hook.refuse = false;
    CHECK(controller.setEnabled(true));
    CHECK(hook.isInstalled());
}

TEST(key_hook_swallow_follows_route_state)
{
    KeyRoutePublisher routes;
    KeyEventQueue queue;
    KeyHookPolicy policy(routes, queue);
    FakeKeyHook hook;
    KeyHookController controller(hook, callbackFor(policy));
    CHECK(controller.setEnabled(true));

    // Without an owner the key passes through.
    routes.publish(KeyRoute{});
    CHECK(!tap(hook, telaviva::kVkVolumeUp));

    // Owner without volume support, and display off, both pass through.
    KeyRoute noVolume = readyRoute(2);
    noVolume.volumeSupported = false;
    routes.publish(noVolume);
    CHECK(!tap(hook, telaviva::kVkVolumeUp));

    KeyRoute asleep = readyRoute(3);
    asleep.displayAwake = false;
    routes.publish(asleep);
    CHECK(!tap(hook, telaviva::kVkVolumeDown));

    routes.publish(readyRoute(4));
    CHECK(tap(hook, telaviva::kVkVolumeUp));
    CHECK(tap(hook, telaviva::kVkVolumeMute));
}

TEST(key_hook_repeats_latched_and_up_matches_down)
{
    KeyRoutePublisher routes;
    KeyEventQueue queue;
    KeyHookPolicy policy(routes, queue);
    FakeKeyHook hook;
    KeyHookController controller(hook, callbackFor(policy));
    CHECK(controller.setEnabled(true));

    routes.publish(readyRoute(1));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, true, false}));

    // The route went passive during the press: the repeat and the up still follow the down.
    routes.publish(KeyRoute{});
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, true, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, true, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, false, false}));

    // The next press reads the passive route.
    CHECK(!hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, true, false}));
    CHECK(!hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, false, false}));
}

TEST(key_hook_ignores_injected_and_other_keys)
{
    KeyRoutePublisher routes;
    KeyEventQueue queue;
    KeyHookPolicy policy(routes, queue);
    FakeKeyHook hook;
    KeyHookController controller(hook, callbackFor(policy));
    CHECK(controller.setEnabled(true));
    routes.publish(readyRoute(1));

    // The app's own injected volume keys must never be swallowed or queued.
    CHECK(!hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, true, true}));
    CHECK(!hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, false, true}));
    CHECK(!hook.inject(KeyHookEvent{0x41, true, false}));

    KeyQueueEntry entry{};
    CHECK(!queue.pop(entry));
    // Their presence did not latch anything: a real press still works.
    CHECK(tap(hook, telaviva::kVkVolumeUp));
}

TEST(key_hook_queues_only_swallowed_events)
{
    KeyRoutePublisher routes;
    KeyEventQueue queue;
    KeyHookPolicy policy(routes, queue);
    FakeKeyHook hook;
    KeyHookController controller(hook, callbackFor(policy));
    CHECK(controller.setEnabled(true));

    // Pass-through presses reach Windows and are not queued.
    routes.publish(KeyRoute{});
    CHECK(!tap(hook, telaviva::kVkVolumeDown));
    KeyQueueEntry entry{};
    CHECK(!queue.pop(entry));

    // Swallowed presses are queued with the route version they were decided on.
    routes.publish(readyRoute(7));
    CHECK(tap(hook, telaviva::kVkVolumeUp));
    CHECK(queue.pop(entry));
    CHECK_EQ(entry.vk, telaviva::kVkVolumeUp);
    CHECK_EQ(entry.isKeyDown, true);
    CHECK_EQ(entry.routeVersion, uint64_t{7});
    CHECK(queue.pop(entry));
    CHECK_EQ(entry.vk, telaviva::kVkVolumeUp);
    CHECK_EQ(entry.isKeyDown, false);
    CHECK_EQ(entry.routeVersion, uint64_t{7});
    CHECK(!queue.pop(entry));
}

TEST(key_hook_queue_carries_shift_state)
{
    KeyRoutePublisher routes;
    KeyEventQueue queue;
    KeyHookPolicy policy(routes, queue);
    FakeKeyHook hook;
    KeyHookController controller(hook, callbackFor(policy));
    CHECK(controller.setEnabled(true));
    routes.publish(readyRoute(9));

    // Shift held on the press: the fine step rides on both the down and the up.
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, true, false, true}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeUp, false, false, true}));
    // Without Shift the queued entry says so.
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeDown, true, false, false}));

    KeyQueueEntry entry{};
    CHECK(queue.pop(entry));
    CHECK(entry.shiftDown);
    CHECK(queue.pop(entry));
    CHECK(entry.shiftDown);
    CHECK(queue.pop(entry));
    CHECK(!entry.shiftDown);
    CHECK(!queue.pop(entry));
}

TEST(key_hook_marks_autorepeat_downs)
{
    KeyRoutePublisher routes;
    KeyEventQueue queue;
    KeyHookPolicy policy(routes, queue);
    FakeKeyHook hook;
    KeyHookController controller(hook, callbackFor(policy));
    CHECK(controller.setEnabled(true));
    routes.publish(readyRoute(3));

    // A press, two autorepeat downs, the release, then a new press with its release.
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeMute, true, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeMute, true, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeMute, true, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeMute, false, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeMute, true, false}));
    CHECK(hook.inject(KeyHookEvent{telaviva::kVkVolumeMute, false, false}));

    const bool expectedRepeat[] = {false, true, true, false, false, false};
    for (const bool repeat : expectedRepeat) {
        KeyQueueEntry entry{};
        CHECK(queue.pop(entry));
        CHECK_EQ(entry.isRepeat, repeat);
    }
    KeyQueueEntry rest{};
    CHECK(!queue.pop(rest));
}

TEST(key_hook_thread_id_reaches_the_caller_through_the_seam)
{
    FakeKeyHook hook;
    const telaviva::IKeyHook& seam = hook;
    CHECK_EQ(seam.threadId(), FakeKeyHook::kThreadId);
}
