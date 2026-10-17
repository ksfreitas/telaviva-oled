#include "pch.h"

#include "Host/PlatformWindow.h"

#include <stdexcept>

namespace telaviva::host {
namespace {

constexpr wchar_t kWindowClassName[] = L"TelaVivaPlatformWindow";
constexpr wchar_t kMutexName[] = L"Local\\TelaVivaOLED.SingleInstance";
constexpr wchar_t kActivateMessageName[] = L"TelaVivaOLED.Activate";

}  // namespace

PlatformWindow::PlatformWindow() {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &PlatformWindow::windowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&cls) == 0) {
        throw std::runtime_error{ "RegisterClassExW failed for the platform window" };
    }

    // The window stays hidden and is kept out of Alt+Tab and the taskbar.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClassName, L"TelaViva OLED", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this);
    if (hwnd_ == nullptr) {
        UnregisterClassW(kWindowClassName, cls.hInstance);
        throw std::runtime_error{ "CreateWindowExW failed for the platform window" };
    }
}

PlatformWindow::~PlatformWindow() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
    UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
}

void PlatformWindow::addHandler(MessageHandler handler) {
    handlers_.push_back(std::move(handler));
}

LRESULT CALLBACK PlatformWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* self = reinterpret_cast<PlatformWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    const LRESULT result = self->dispatch(hwnd, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        // Nothing may reach the object after its window is gone.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return result;
}

LRESULT PlatformWindow::dispatch(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    for (auto& handler : handlers_) {
        if (auto result = handler(message, wParam, lParam)) {
            return *result;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

SingleInstance::SingleInstance() {
    mutex_ = CreateMutexW(nullptr, FALSE, kMutexName);
    // Read the error code before any other call can overwrite it.
    first_ = GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_ != nullptr) {
        CloseHandle(mutex_);
    }
}

void SingleInstance::notifyFirstInstance() {
    if (HWND first = FindWindowW(kWindowClassName, nullptr)) {
        PostMessageW(first, activateMessage(), 0, 0);
    }
}

UINT SingleInstance::activateMessage() {
    static const UINT message = RegisterWindowMessageW(kActivateMessageName);
    return message;
}

}  // namespace telaviva::host
