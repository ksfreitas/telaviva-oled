#pragma once
// Volume-key hook: the OS hook thread, the hook-side policy (route, latch,
// queue), a watchdog and the controller that enables it. Only KeyHook.cpp
// includes windows.h.
#include "IKeyHook.h"
#include "KeyRoute.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace telaviva {

// Reinstall when a key has been held and no volume event arrived for this long.
// The contracts name the watchdog but give no number. It is above the longest
// keyboard repeat delay (1000 ms in Windows), so a held key does not trigger a
// spurious reinstall, and well below anything the user would notice.
// Known limitation: a hook Windows removed while no key was held is only noticed
// at the next key press, which then never reaches the callback.
inline constexpr uint64_t kKeyHookWatchdogWindowMs = 1500;

struct KeyQueueEntry {
    uint32_t vk;
    bool isKeyDown;
    uint64_t routeVersion;
    bool shiftDown = false;  // Shift held at the event: the fine step
    bool isRepeat = false;   // key-down while the key was already down (autorepeat)
};

// Single-producer, single-consumer ring. The hook thread pushes; one worker
// pops. Full means the event is dropped, never blocked on.
// C4324 is expected: the alignas padding keeps head and tail on separate cache lines.
#pragma warning(push)
#pragma warning(disable : 4324)
class KeyEventQueue {
public:
    static constexpr std::size_t kCapacity = 64;

    // Producer only. Returns false when full.
    bool push(const KeyQueueEntry& entry)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == kCapacity) {
            return false;
        }
        slots_[tail % kCapacity] = entry;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    bool pop(KeyQueueEntry& out)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = slots_[head % kCapacity];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<KeyQueueEntry, kCapacity> slots_{};
    // Separate cache lines: the producer and consumer write different counters.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
#pragma warning(pop)

// Hook-thread policy: reads the route, latches the decision per key sequence,
// and queues swallowed events for the worker. Makes no OS calls.
class KeyHookPolicy {
public:
    KeyHookPolicy(const KeyRoutePublisher& route, KeyEventQueue& queue);

    // Returns true to swallow. Injected events and non-volume keys pass
    // through untouched and are not queued. Only swallowed events are queued:
    // the worker acts on what the app took over, and Windows keeps the rest.
    bool onEvent(const KeyHookEvent& event);

private:
    const KeyRoutePublisher& route_;
    KeyEventQueue& queue_;
    KeyLatch latch_;
    // Hook thread only: whether each hooked key is down, for autorepeat detection.
    std::array<bool, 3> down_{};
};

// Pure watchdog state. Hook thread only. A key counts as held from its down
// until its up. due() returns true when a key is held and no volume event
// arrived for windowMs, then restarts the window so a dead hook is reinstalled
// once per window.
class KeyWatchdog {
public:
    explicit KeyWatchdog(uint64_t windowMs);

    void onKey(uint64_t nowMs, uint32_t vk, bool isKeyDown);
    bool due(uint64_t nowMs);
    bool held() const { return heldMask_ != 0; }
    void reset();

private:
    uint64_t windowMs_;
    uint64_t lastEventMs_ = 0;
    unsigned heldMask_ = 0;
};

// Enables and disables the hook. Repeated calls with the same value do nothing.
class KeyHookController {
public:
    KeyHookController(IKeyHook& hook, KeyHookCallback callback);

    // Installs when enabled, uninstalls when disabled. Returns true when the
    // hook is in the requested state. A failed install is retried on the next
    // enabled call.
    bool setEnabled(bool enabled);

private:
    IKeyHook& hook_;
    KeyHookCallback callback_;
};

// Dedicated thread with a message loop. WH_KEYBOARD_LL for the three volume
// keys only. Watchdog timer runs only while a key is held. The thread is
// created on the first install and joined on destruction.
class Win32KeyHook final : public IKeyHook {
public:
    // log may be empty. It receives reinstall and failure diagnostics.
    explicit Win32KeyHook(std::function<void(std::string)> log = {});
    ~Win32KeyHook() override;

    Win32KeyHook(const Win32KeyHook&) = delete;
    Win32KeyHook& operator=(const Win32KeyHook&) = delete;

    // Call from one controlling thread only, never from the hook callback.
    bool install(KeyHookCallback callback) override;
    void uninstall() override;
    bool isInstalled() const override;
    uint32_t threadId() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
