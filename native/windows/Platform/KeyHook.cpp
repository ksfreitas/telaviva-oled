#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "KeyHook.h"

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace telaviva {

static_assert(kVkVolumeMute == static_cast<uint32_t>(VK_VOLUME_MUTE), "VK_VOLUME_MUTE mismatch");
static_assert(kVkVolumeDown == static_cast<uint32_t>(VK_VOLUME_DOWN), "VK_VOLUME_DOWN mismatch");
static_assert(kVkVolumeUp == static_cast<uint32_t>(VK_VOLUME_UP), "VK_VOLUME_UP mismatch");

// ---- Hook-side policy and watchdog (no OS calls) ----

KeyHookPolicy::KeyHookPolicy(const KeyRoutePublisher& route, KeyEventQueue& queue)
    : route_(route), queue_(queue)
{
}

bool KeyHookPolicy::onEvent(const KeyHookEvent& event)
{
    const int slot = volumeKeySlot(event.vk);
    if (event.isInjected || slot < 0) {
        return false;
    }
    // Autorepeat sends key-downs while the key stays down, so a down seen while already down is a repeat.
    const bool isRepeat = event.isKeyDown && down_[static_cast<std::size_t>(slot)];
    down_[static_cast<std::size_t>(slot)] = event.isKeyDown;
    // One route read per event: the decision and the queued version come from the same snapshot.
    const KeyRoute route = route_.read();
    const bool swallow = latch_.decide(event.vk, event.isKeyDown, shouldSwallow(route));
    if (swallow) {
        queue_.push(KeyQueueEntry{event.vk, event.isKeyDown, route.version, event.shiftDown, isRepeat});
    }
    return swallow;
}

KeyWatchdog::KeyWatchdog(uint64_t windowMs) : windowMs_(windowMs) {}

void KeyWatchdog::onKey(uint64_t nowMs, uint32_t vk, bool isKeyDown)
{
    const int slot = volumeKeySlot(vk);
    if (slot < 0) {
        return;
    }
    lastEventMs_ = nowMs;
    const unsigned bit = 1u << static_cast<unsigned>(slot);
    if (isKeyDown) {
        heldMask_ |= bit;
    } else {
        heldMask_ &= ~bit;
    }
}

bool KeyWatchdog::due(uint64_t nowMs)
{
    if (heldMask_ == 0 || nowMs - lastEventMs_ < windowMs_) {
        return false;
    }
    lastEventMs_ = nowMs;
    return true;
}

void KeyWatchdog::reset()
{
    heldMask_ = 0;
}

KeyHookController::KeyHookController(IKeyHook& hook, KeyHookCallback callback)
    : hook_(hook), callback_(std::move(callback))
{
}

bool KeyHookController::setEnabled(bool enabled)
{
    if (enabled) {
        if (hook_.isInstalled()) {
            return true;
        }
        return hook_.install(callback_);
    }
    if (hook_.isInstalled()) {
        hook_.uninstall();
    }
    return !hook_.isInstalled();
}

// ---- Win32 hook ----

namespace {

constexpr UINT kCommandMessage = WM_APP + 1;
constexpr WPARAM kCommandInstall = 1;
constexpr WPARAM kCommandUninstall = 2;
constexpr WPARAM kCommandQuit = 3;
// Half the window, so a stall is seen within at most 1.5 windows of the last event.
constexpr UINT kWatchdogTickMs = static_cast<UINT>(kKeyHookWatchdogWindowMs / 2);

// State touched only by the hook thread. The LL hook procedure runs on that
// thread, so it reaches this state through a thread-local pointer.
struct HookState {
    KeyHookCallback callback;
    KeyWatchdog watchdog{kKeyHookWatchdogWindowMs};
    HHOOK hook = nullptr;
    UINT_PTR timerId = 0;
    std::atomic<bool> installed{false};
    std::function<void(std::string)> log;
};

thread_local HookState* tlsState = nullptr;

void logLine(const HookState& state, const std::string& line)
{
    if (state.log) {
        state.log(line);
    }
}

// Trivial by design: one route-policy call and at most one timer arm. The
// watchdog counts this event first so the timer sees it.
LRESULT CALLBACK lowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    HookState* state = tlsState;
    if (code == HC_ACTION && state != nullptr && state->callback) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool isDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool isUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        // Injected keys are the app's own output; passing them through avoids a loop.
        const bool injected = (info->flags & LLKHF_INJECTED) != 0;
        if ((isDown || isUp) && !injected && volumeKeySlot(info->vkCode) >= 0) {
            state->watchdog.onKey(GetTickCount64(), info->vkCode, isDown);
            // The fine step needs Shift, and this is the only point where the hook sees it.
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool swallow = state->callback(KeyHookEvent{info->vkCode, isDown, false, shift});
            if (state->watchdog.held() && state->timerId == 0) {
                state->timerId = SetTimer(nullptr, 0, kWatchdogTickMs, nullptr);
            }
            if (swallow) {
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void stopTimer(HookState& state)
{
    if (state.timerId != 0) {
        KillTimer(nullptr, state.timerId);
        state.timerId = 0;
    }
}

bool installHook(HookState& state)
{
    if (state.hook != nullptr) {
        return true;
    }
    state.hook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (state.hook == nullptr) {
        state.installed = false;
        logLine(state, "key hook: SetWindowsHookExW failed, error " + std::to_string(GetLastError()));
        return false;
    }
    state.installed = true;
    return true;
}

void unhookOnly(HookState& state)
{
    if (state.hook != nullptr) {
        UnhookWindowsHookEx(state.hook);
        state.hook = nullptr;
    }
    state.installed = false;
}

void onWatchdogTick(HookState& state)
{
    if (state.watchdog.due(GetTickCount64())) {
        logLine(state, "key hook: no callback while a volume key was held; reinstalling");
        unhookOnly(state);
        // Clears the held state too: a key-up lost with the old hook must not keep the timer alive.
        state.watchdog.reset();
        installHook(state);
    }
    if (!state.watchdog.held()) {
        stopTimer(state);
    }
}

}  // namespace

struct Win32KeyHook::Impl {
    explicit Impl(std::function<void(std::string)> log)
    {
        state.log = std::move(log);
    }

    ~Impl()
    {
        if (thread.joinable()) {
            sendCommand(kCommandQuit);
            thread.join();
        }
    }

    bool install(KeyHookCallback callback)
    {
        if (!callback) {
            return false;
        }
        try {
            if (!startThread()) {
                return false;
            }
        } catch (const std::system_error& error) {
            logLine(state, std::string("key hook: thread start failed: ") + error.what());
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = std::move(callback);
        }
        return sendCommand(kCommandInstall);
    }

    void uninstall()
    {
        if (thread.joinable()) {
            sendCommand(kCommandUninstall);
        }
    }

    // The thread is created once and kept until destruction, idle in GetMessage.
    bool startThread()
    {
        if (thread.joinable()) {
            return true;
        }
        auto ready = std::make_shared<std::promise<DWORD>>();
        std::future<DWORD> readyValue = ready->get_future();
        thread = std::thread([this, ready] { run(ready); });
        threadId = readyValue.get();
        if (threadId == 0) {
            thread.join();
            return false;
        }
        return true;
    }

    // Ownership of the reply promise passes to the hook thread through the message.
    bool sendCommand(WPARAM command)
    {
        auto* reply = new std::promise<bool>();
        std::future<bool> result = reply->get_future();
        if (!PostThreadMessageW(threadId, kCommandMessage, command, reinterpret_cast<LPARAM>(reply))) {
            delete reply;
            return false;
        }
        return result.get();
    }

    void run(const std::shared_ptr<std::promise<DWORD>>& ready)
    {
        tlsState = &state;
        MSG msg{};
        // Creating the queue here means the caller's first PostThreadMessage cannot be lost.
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        ready->set_value(GetCurrentThreadId());

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && msg.message == WM_TIMER) {
                if (msg.wParam == state.timerId) {
                    onWatchdogTick(state);
                }
                continue;
            }
            if (msg.message != kCommandMessage) {
                continue;
            }
            std::unique_ptr<std::promise<bool>> reply(reinterpret_cast<std::promise<bool>*>(msg.lParam));
            bool ok = true;
            bool quit = false;
            if (msg.wParam == kCommandInstall) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.callback = std::move(pending);
                }
                ok = installHook(state);
            } else if (msg.wParam == kCommandUninstall) {
                stopTimer(state);
                unhookOnly(state);
                state.watchdog.reset();
            } else if (msg.wParam == kCommandQuit) {
                stopTimer(state);
                unhookOnly(state);
                quit = true;
            }
            reply->set_value(ok);
            if (quit) {
                break;
            }
        }
        tlsState = nullptr;
    }

    HookState state;
    std::mutex mutex;
    KeyHookCallback pending;
    std::thread thread;
    DWORD threadId = 0;
};

Win32KeyHook::Win32KeyHook(std::function<void(std::string)> log)
    : impl_(std::make_unique<Impl>(std::move(log)))
{
}

Win32KeyHook::~Win32KeyHook() = default;

bool Win32KeyHook::install(KeyHookCallback callback)
{
    return impl_->install(std::move(callback));
}

void Win32KeyHook::uninstall()
{
    impl_->uninstall();
}

bool Win32KeyHook::isInstalled() const
{
    return impl_->state.installed.load();
}

uint32_t Win32KeyHook::threadId() const
{
    // Written by the controlling thread when the hook thread starts, and read there too.
    return impl_->threadId;
}

}  // namespace telaviva
