#pragma once
#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

namespace telaviva::host {

// Handlers run in registration order for every message; the first one that
// returns a value wins. Add handlers before the message loop starts.
using MessageHandler = std::function<std::optional<LRESULT>(UINT, WPARAM, LPARAM)>;

// Hidden top-level window (not HWND_MESSAGE): message-only windows do not
// receive broadcasts such as WM_DISPLAYCHANGE and WM_DEVICECHANGE.
class PlatformWindow {
public:
    PlatformWindow();
    ~PlatformWindow();

    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;

    HWND hwnd() const { return hwnd_; }
    void addHandler(MessageHandler handler);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT dispatch(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_{};
    std::vector<MessageHandler> handlers_;
};

// Keeps one instance per session through a named mutex.
class SingleInstance {
public:
    SingleInstance();
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool isFirst() const { return first_; }

    // Posts the activate message to the first instance's PlatformWindow.
    static void notifyFirstInstance();
    static UINT activateMessage();

private:
    HANDLE mutex_{};
    bool first_{};
};

}  // namespace telaviva::host
