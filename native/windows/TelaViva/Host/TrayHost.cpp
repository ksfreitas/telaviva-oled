#include "pch.h"

#include "Host/TrayHost.h"
#include "Host/PlatformWindow.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cwchar>
#include <iterator>
#include <utility>
#include <vector>

namespace telaviva::host {
namespace {

// Fixed per-user identity; the installer relies on it never changing.
constexpr GUID kIconGuid = { 0x7B1E7A9C, 0x3F0B, 0x4E55, { 0x9C, 0x1A, 0x2D, 0x4B, 0x6A, 0x1E, 0x0F, 0x11 } };
constexpr UINT kCallbackMessage = WM_APP + 1;

UINT taskbarCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

// Defaults to a light taskbar when the value cannot be read.
bool taskbarUsesLightTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status != ERROR_SUCCESS || value != 0;
}

std::wstring executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    const std::wstring path(buffer.data(), length);
    return path.substr(0, path.find_last_of(L'\\'));
}

const wchar_t* stateName(TrayState state) {
    switch (state) {
    case TrayState::Detected: return L"detected";
    case TrayState::Absent: return L"absent";
    case TrayState::LowLight: return L"lowlight";
    }
    return L"absent";
}

}  // namespace

TrayHost::TrayHost(PlatformWindow& window, TrayCallbacks callbacks, std::function<TrayStrings()> strings)
    : window_{ window },
      callbacks_{ std::move(callbacks) },
      strings_{ std::move(strings) },
      alive_{ std::make_shared<bool>(true) },
      currentStrings_{ strings_() } {
    window_.addHandler([this, alive = alive_](UINT message, WPARAM wParam, LPARAM lParam) -> std::optional<LRESULT> {
        if (!*alive) {
            return std::nullopt;
        }
        return onMessage(message, wParam, lParam);
    });

    icon_ = loadIconForState();
    addIcon();
}

TrayHost::~TrayHost() {
    *alive_ = false;
    NOTIFYICONDATAW data = identity();
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
    }
}

void TrayHost::setState(TrayState state) {
    state_ = state;
    replaceIcon();
}

RECT TrayHost::iconRect() const {
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = window_.hwnd();
    identifier.guidItem = kIconGuid;

    RECT rect{};
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &rect))) {
        return RECT{};
    }
    return rect;
}

void TrayHost::refreshStrings() {
    currentStrings_ = strings_();
    modifyIcon();
}

std::optional<LRESULT> TrayHost::onMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kCallbackMessage) {
        // V4 packs the event into LOWORD(lParam) and the anchor point into wParam.
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (callbacks_.onOpenFlyout) {
                callbacks_.onOpenFlyout();
            }
            return LRESULT{ 0 };
        case WM_CONTEXTMENU: {
            // The shell passes the anchor in wParam for both a mouse click and the keyboard.
            const POINT anchor{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
            // A mouse click happens over the icon; a keyboard invocation leaves the cursor elsewhere.
            const RECT icon = iconRect();
            POINT cursor{};
            const bool fromMouse = GetCursorPos(&cursor) && !IsRectEmpty(&icon) && PtInRect(&icon, cursor);
            if (callbacks_.onContextMenu) {
                callbacks_.onContextMenu(anchor, !fromMouse);
            }
            return LRESULT{ 0 };
        }
        default:
            return std::nullopt;
        }
    }

    if (message == taskbarCreatedMessage()) {
        // Explorer restarted and dropped the icon; the GUID identity lets us re-add it.
        addIcon();
        return std::nullopt;
    }

    if (message == WM_SETTINGCHANGE && lParam != 0 &&
        wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
        replaceIcon();
        return std::nullopt;
    }

    if (message == WM_DPICHANGED) {
        replaceIcon();
        return std::nullopt;
    }

    return std::nullopt;
}

NOTIFYICONDATAW TrayHost::identity() const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_.hwnd();
    data.uFlags = NIF_GUID;
    data.guidItem = kIconGuid;
    return data;
}

NOTIFYICONDATAW TrayHost::fullData() const {
    NOTIFYICONDATAW data = identity();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    // szTip holds 128 characters including the terminator; keep it within 127.
    wcsncpy_s(data.szTip, std::size(data.szTip), tooltipFor(state_).c_str(), _TRUNCATE);
    return data;
}

const std::wstring& TrayHost::tooltipFor(TrayState state) const {
    switch (state) {
    case TrayState::Detected: return currentStrings_.tooltipDetected;
    case TrayState::Absent: return currentStrings_.tooltipAbsent;
    case TrayState::LowLight: return currentStrings_.tooltipLowLight;
    }
    return currentStrings_.tooltipAbsent;
}

HICON TrayHost::loadIconForState() const {
    // Dark icons for a dark taskbar; the registry flag is 1 for a light taskbar.
    const bool light = taskbarUsesLightTheme();
    const UINT dpi = GetDpiForWindow(window_.hwnd());
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI);

    const std::wstring path = executableDirectory() + L"\\Assets\\tray-" + stateName(state_) +
        (light ? L"-light.ico" : L"-dark.ico");
    return static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
}

void TrayHost::addIcon() {
    NOTIFYICONDATAW data = fullData();
    Shell_NotifyIconW(NIM_ADD, &data);
    // Must follow every NIM_ADD, or the shell keeps the legacy callback format.
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void TrayHost::modifyIcon() {
    NOTIFYICONDATAW data = fullData();
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayHost::replaceIcon() {
    HICON next = loadIconForState();
    HICON previous = std::exchange(icon_, next);
    modifyIcon();
    // The shell has copied the new icon by now, so the old handle can go.
    if (previous != nullptr) {
        DestroyIcon(previous);
    }
}

}  // namespace telaviva::host
