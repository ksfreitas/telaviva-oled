#pragma once
// Turns session, power and display window messages into app events. The app
// registers handleMessage as a handler on its hidden window. This header
// does not depend on the app's own headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Scheduler.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace telaviva {

// onDisplayState, onResume and onSessionLock run on the UI thread from
// handleMessage. onTopologyChanged runs on a scheduler thread, so it must be
// thread-safe.
struct SessionEventCallbacks {
    std::function<void(bool displayOn)> onDisplayState;
    std::function<void()> onResume;
    std::function<void()> onTopologyChanged;
    std::function<void(bool locked)> onSessionLock;
};

class SessionEvents {
public:
    SessionEvents(IScheduler& scheduler, SessionEventCallbacks callbacks,
                  std::chrono::milliseconds topologyDebounce = std::chrono::milliseconds(500));
    // Cancels a pending topology call so it cannot fire after destruction.
    ~SessionEvents();

    SessionEvents(const SessionEvents&) = delete;
    SessionEvents& operator=(const SessionEvents&) = delete;

    // Returns std::nullopt for messages it does not consume or that must also
    // reach other handlers. Never throws.
    //
    // Topology triggers are coalesced: a trigger that arrives while one is
    // pending is absorbed and the first trigger's deadline is kept, so a
    // steady stream of events still fires once per debounce window.
    std::optional<LRESULT> handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Thin wrappers over the OS registrations (not unit-tested). registerFor
    // replaces any earlier registration; unregisterFor releases the last one.
    static bool registerFor(HWND window);
    static void unregisterFor();

private:
    struct Topology;

    std::optional<LRESULT> handlePower(WPARAM wParam, LPARAM lParam);
    void requestTopology();

    IScheduler& scheduler_;
    std::chrono::milliseconds topologyDebounce_;
    SessionEventCallbacks callbacks_;
    // Shared with scheduled timers, which may run on a scheduler thread.
    std::shared_ptr<Topology> topology_;
};

}  // namespace telaviva
