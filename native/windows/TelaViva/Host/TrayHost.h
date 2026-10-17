#pragma once
#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace telaviva::host {

class PlatformWindow;

struct TrayStrings {
    std::wstring tooltipDetected;
    std::wstring tooltipAbsent;
    std::wstring tooltipLowLight;
};

enum class TrayState { Detected, Absent, LowLight };

struct TrayCallbacks {
    std::function<void()> onOpenFlyout;
    // Anchor in screen pixels. fromKeyboard is true when the icon was not under the cursor (Shift+F10 or the menu key).
    std::function<void(POINT anchor, bool fromKeyboard)> onContextMenu;
};

// Notification-area icon (Shell_NotifyIcon V4). Its identity is a fixed GUID,
// so Windows keeps the icon's placement across runs.
class TrayHost {
public:
    TrayHost(PlatformWindow& window, TrayCallbacks callbacks, std::function<TrayStrings()> strings);
    ~TrayHost();

    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;

    void setState(TrayState state);

    // Empty RECT when the shell cannot report the icon position.
    RECT iconRect() const;

    void refreshStrings();

private:
    std::optional<LRESULT> onMessage(UINT message, WPARAM wParam, LPARAM lParam);
    NOTIFYICONDATAW identity() const;
    NOTIFYICONDATAW fullData() const;
    const std::wstring& tooltipFor(TrayState state) const;
    HICON loadIconForState() const;
    void addIcon();
    void modifyIcon();
    void replaceIcon();

    PlatformWindow& window_;
    TrayCallbacks callbacks_;
    std::function<TrayStrings()> strings_;
    // Cleared in the destructor; the window handler checks it because the
    // handler stays registered until the window itself is destroyed.
    std::shared_ptr<bool> alive_;
    TrayStrings currentStrings_;
    TrayState state_ = TrayState::Absent;
    HICON icon_ = nullptr;
};

}  // namespace telaviva::host
