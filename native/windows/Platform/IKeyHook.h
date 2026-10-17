#pragma once
// Seam between the volume-key hook and its users, so tests can replace the OS hook.
#include <cstdint>
#include <functional>

namespace telaviva {

// Only VK_VOLUME_UP, VK_VOLUME_DOWN and VK_VOLUME_MUTE reach the callback.
struct KeyHookEvent {
    uint32_t vk;
    bool isKeyDown;
    bool isInjected;
    bool shiftDown = false;  // Shift state at the event, read by the hook callback
};

// Returns true to swallow the key. Runs on the hook thread, so it must not
// block, call the core, touch DDC, or take a lock another thread can hold.
using KeyHookCallback = std::function<bool(const KeyHookEvent&)>;

class IKeyHook {
public:
    virtual ~IKeyHook() = default;

    // Returns false when the OS refused the hook. Installing while installed
    // replaces the callback and returns true.
    virtual bool install(KeyHookCallback callback) = 0;
    virtual void uninstall() = 0;
    virtual bool isInstalled() const = 0;
    // Id of the thread that runs the callback; 0 until that thread has started.
    virtual uint32_t threadId() const = 0;
};

}  // namespace telaviva
