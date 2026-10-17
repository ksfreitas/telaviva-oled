#include "pch.h"

#include "Host/AppLifecycle.h"
#include "Host/PlatformWindow.h"
#include "Platform/CoreBridge.h"

#include <utility>

using namespace std::chrono_literals;

namespace telaviva::host {

AppLifecycle::AppLifecycle(telaviva::CoreBridge* core, std::function<void()> exitApplication,
    PlatformWindow* uiWindow)
    : core_{ core },
      exitApplication_{ std::move(exitApplication) },
      uiWindow_{ uiWindow },
      shutdownReason_{ L"Stopping TelaViva OLED." } {}

AppLifecycle::~AppLifecycle() = default;

void AppLifecycle::requestQuit() {
    if (quitting_.exchange(true)) {
        return;
    }

    if (core_ == nullptr) {
        postExit();
        return;
    }

    // The 8 s drain runs off the UI thread so the message loop keeps responding.
    drain_ = std::async(std::launch::async, [this] {
        drainCore(8s);
        postExit();
    });
}

std::optional<LRESULT> AppLifecycle::onSessionMessage(UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
    case WM_QUERYENDSESSION:
        requestQuit();
        return static_cast<LRESULT>(TRUE);
    case WM_ENDSESSION:
        if (wParam != 0) {
            endSession();
        }
        return LRESULT{ 0 };
    case kExitRequestedMessage:
        if (exitApplication_) {
            exitApplication_();
        }
        return LRESULT{ 0 };
    default:
        return std::nullopt;
    }
}

void AppLifecycle::setShutdownReason(std::wstring reason) {
    shutdownReason_ = std::move(reason);
}

void AppLifecycle::drainCore(std::chrono::milliseconds deadline) {
    if (core_ == nullptr) {
        return;
    }
    // A throw here must not keep the process alive after the user asked to quit.
    try {
        core_->shutdown(deadline);
    } catch (...) {
    }
}

void AppLifecycle::postExit() {
    if (uiWindow_ != nullptr) {
        PostMessageW(uiWindow_->hwnd(), kExitRequestedMessage, 0, 0);
    } else if (exitApplication_) {
        exitApplication_();
    }
}

void AppLifecycle::endSession() {
    // Windows allows about 5 s before it terminates the process anyway.
    constexpr auto kSessionDeadline = 5s;

    const HWND hwnd = uiWindow_ != nullptr ? uiWindow_->hwnd() : nullptr;
    if (hwnd != nullptr) {
        ShutdownBlockReasonCreate(hwnd, shutdownReason_.c_str());
    }

    if (quitting_.exchange(true)) {
        // A drain started earlier (query or tray quit): wait for it, bounded.
        if (drain_.valid()) {
            drain_.wait_for(kSessionDeadline);
        }
    } else {
        drainCore(kSessionDeadline);
    }

    if (hwnd != nullptr) {
        ShutdownBlockReasonDestroy(hwnd);
    }
}

}  // namespace telaviva::host
