#include "LaunchAtLogin.h"

#include <windows.h>

#include <utility>

#pragma comment(lib, "Advapi32.lib")

namespace telaviva {

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"TelaVivaOLED";
constexpr wchar_t kBackgroundArgument[] = L" --background";

// Closes the handle on scope exit so every early return releases it.
class RegistryKey {
public:
    RegistryKey() = default;
    ~RegistryKey()
    {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY get() const { return key_; }
    HKEY* receive() { return &key_; }

private:
    HKEY key_ = nullptr;
};

std::wstring describeError(LONG code)
{
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, static_cast<DWORD>(code),
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (length != 0 && buffer != nullptr) {
        text.assign(buffer, length);
        LocalFree(buffer);
    }
    // System messages end with CR/LF, which would break the one-line error text.
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    std::wstring message = L"Windows error " + std::to_wstring(code);
    if (!text.empty()) {
        message += L": " + text;
    }
    return message;
}

// Puts the user's previous Run value back when the stored value no longer matches it,
// so a failed enable does not leave a half-written entry behind.
void restoreIfChanged(ILaunchRegistry& registry, const std::optional<std::wstring>& previous, std::wstring& error)
{
    if (registry.read() == previous) {
        return;
    }
    std::wstring restoreError;
    const bool restored = previous ? registry.write(*previous, restoreError) : registry.remove(restoreError);
    if (!restored) {
        error += L" (restoring the previous value failed: " + restoreError + L")";
    }
}

}  // namespace

LaunchAtLogin::LaunchAtLogin(ILaunchRegistry& registry, std::wstring executablePath)
    : registry_(registry), executablePath_(std::move(executablePath))
{
}

bool LaunchAtLogin::isEnabled() const
{
    const std::optional<std::wstring> stored = registry_.read();
    return stored.has_value() && *stored == expectedCommand(executablePath_);
}

LaunchResult LaunchAtLogin::setEnabled(bool enable)
{
    LaunchResult result;
    const std::optional<std::wstring> previous = registry_.read();

    if (enable) {
        const std::wstring expected = expectedCommand(executablePath_);
        std::wstring writeError;
        const bool wrote = registry_.write(expected, writeError);
        // A successful write call is not proof on its own; the read-back decides.
        const bool verified = wrote && registry_.read() == std::optional<std::wstring>(expected);
        if (verified) {
            result.ok = true;
            result.enabled = true;
            return result;
        }
        if (wrote) {
            result.error = L"The Run value did not match after writing";
        } else {
            result.error = writeError.empty() ? L"The Run value could not be written" : writeError;
        }
        restoreIfChanged(registry_, previous, result.error);
    } else {
        std::wstring removeError;
        const bool removed = registry_.remove(removeError);
        if (!registry_.read().has_value()) {
            result.ok = true;
            result.enabled = false;
            return result;
        }
        if (removed) {
            result.error = L"The Run value is still present after removal";
        } else {
            result.error = removeError.empty() ? L"The Run value could not be removed" : removeError;
        }
    }

    result.ok = false;
    result.enabled = isEnabled();
    return result;
}

std::wstring LaunchAtLogin::expectedCommand(const std::wstring& executablePath)
{
    return L"\"" + executablePath + L"\"" + kBackgroundArgument;
}

std::optional<std::wstring> RunKeyRegistry::read()
{
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, key.receive()) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.get(), kValueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    const LONG status = RegQueryValueExW(key.get(), kValueName, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(value.data()), &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }

    value.resize(bytes / sizeof(wchar_t));
    // REG_SZ data is stored with a terminating NUL that is not part of the command.
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

bool RunKeyRegistry::write(const std::wstring& value, std::wstring& error)
{
    RegistryKey key;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_SET_VALUE, nullptr, key.receive(), nullptr);
    if (status != ERROR_SUCCESS) {
        error = describeError(status);
        return false;
    }

    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    status = RegSetValueExW(key.get(), kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            bytes);
    if (status != ERROR_SUCCESS) {
        error = describeError(status);
        return false;
    }
    return true;
}

bool RunKeyRegistry::remove(std::wstring& error)
{
    RegistryKey key;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, key.receive());
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) {
        error = describeError(status);
        return false;
    }

    status = RegDeleteValueW(key.get(), kValueName);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    error = describeError(status);
    return false;
}

}  // namespace telaviva
