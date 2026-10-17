#pragma once
// Route table for the volume-key hook and the per-key-sequence latch. No OS
// headers: the hook thread reads the route, the app thread publishes it.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace telaviva {

// Win32 VK_VOLUME_* values, repeated here so this header stays free of windows.h.
// KeyHook.cpp asserts they match the SDK constants.
inline constexpr uint32_t kVkVolumeMute = 0xAD;
inline constexpr uint32_t kVkVolumeDown = 0xAE;
inline constexpr uint32_t kVkVolumeUp = 0xAF;

// Index 0..2 for the three hooked keys, -1 for everything else.
constexpr int volumeKeySlot(uint32_t vk)
{
    return vk == kVkVolumeMute ? 0 : vk == kVkVolumeDown ? 1 : vk == kVkVolumeUp ? 2 : -1;
}

// Immutable snapshot of what the hook needs. version is caller-owned (the core
// revision) and wraps at 2^61 in the published word.
struct KeyRoute {
    bool hasOwner = false;
    bool volumeSupported = false;
    bool displayAwake = false;
    uint64_t version = 0;
};

// The hook swallows only when the route has an owner, the owner supports
// volume and the display is awake. hardware_keys_enabled is applied by
// installing the hook at all, not here.
constexpr bool shouldSwallow(const KeyRoute& route)
{
    return route.hasOwner && route.volumeSupported && route.displayAwake;
}

// Publishes a whole KeyRoute as one std::atomic<uint64_t>, so the reader never
// sees a mix of two publications. Chosen over a seqlock: a single word needs no
// retry loop, so read() is wait-free, and over std::atomic<std::shared_ptr>,
// which MSVC does not implement lock-free. Layout: bit 0 hasOwner, bit 1
// volumeSupported, bit 2 displayAwake, bits 3..63 version (61 bits).
class KeyRoutePublisher {
public:
    // Any thread.
    void publish(const KeyRoute& route)
    {
        uint64_t word = (route.version & kVersionMask) << kVersionShift;
        if (route.hasOwner) {
            word |= kOwnerBit;
        }
        if (route.volumeSupported) {
            word |= kVolumeBit;
        }
        if (route.displayAwake) {
            word |= kAwakeBit;
        }
        word_.store(word, std::memory_order_release);
    }

    // Hook thread. Wait-free.
    KeyRoute read() const
    {
        const uint64_t word = word_.load(std::memory_order_acquire);
        KeyRoute route;
        route.hasOwner = (word & kOwnerBit) != 0;
        route.volumeSupported = (word & kVolumeBit) != 0;
        route.displayAwake = (word & kAwakeBit) != 0;
        route.version = word >> kVersionShift;
        return route;
    }

private:
    static constexpr uint64_t kOwnerBit = 1u << 0;
    static constexpr uint64_t kVolumeBit = 1u << 1;
    static constexpr uint64_t kAwakeBit = 1u << 2;
    static constexpr unsigned kVersionShift = 3;
    static constexpr uint64_t kVersionMask = (uint64_t{1} << 61) - 1;

    std::atomic<uint64_t> word_{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "KeyRoutePublisher needs a lock-free word");

// Keeps the decision taken at key-down for each hooked key until its key-up, so
// repeats and the up use the same answer even if the route changed meanwhile.
// Hook thread only; not thread-safe.
class KeyLatch {
public:
    // Returns true to swallow this event. routeSwallows is shouldSwallow() of
    // the route read for this event. A key-up with no latched key-down returns
    // false: the down went to Windows, so the up must too.
    bool decide(uint32_t vk, bool isKeyDown, bool routeSwallows)
    {
        const int slot = volumeKeySlot(vk);
        if (slot < 0) {
            return false;
        }
        Slot& latch = slots_[static_cast<std::size_t>(slot)];
        if (isKeyDown) {
            if (!latch.latched) {
                latch.latched = true;
                latch.swallow = routeSwallows;
            }
            return latch.swallow;
        }
        const bool result = latch.latched && latch.swallow;
        latch = Slot{};
        return result;
    }

    void reset()
    {
        slots_ = {};
    }

private:
    struct Slot {
        bool latched = false;
        bool swallow = false;
    };

    std::array<Slot, 3> slots_{};
};

}  // namespace telaviva
