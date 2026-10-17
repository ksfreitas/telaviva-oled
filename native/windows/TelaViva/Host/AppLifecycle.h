#pragma once
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <string>

namespace telaviva {
class CoreBridge;
}

namespace telaviva::host {

class PlatformWindow;

// Posted to the platform window when the drain finishes, so the exit runs on the UI thread.
inline constexpr UINT kExitRequestedMessage = WM_APP + 2;

// Owns the quit path. All methods except the drain itself run on the UI thread.
class AppLifecycle {
public:
    // uiWindow receives the exit request on the UI thread. When it is null the
    // exit callback runs on the calling thread.
    AppLifecycle(telaviva::CoreBridge* core, std::function<void()> exitApplication,
        PlatformWindow* uiWindow = nullptr);
    ~AppLifecycle();

    AppLifecycle(const AppLifecycle&) = delete;
    AppLifecycle& operator=(const AppLifecycle&) = delete;

    // Idempotent: only the first call starts the drain.
    void requestQuit();

    // Returns a value only for the messages it handles. Register it as a PlatformWindow handler.
    std::optional<LRESULT> onSessionMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void setShutdownReason(std::wstring reason);

private:
    void drainCore(std::chrono::milliseconds deadline);
    void postExit();
    void endSession();

    telaviva::CoreBridge* core_;
    std::function<void()> exitApplication_;
    PlatformWindow* uiWindow_;
    std::atomic<bool> quitting_{ false };
    std::future<void> drain_;
    std::wstring shutdownReason_;
};

}  // namespace telaviva::host
