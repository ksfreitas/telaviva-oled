#pragma once
#include <optional>
#include <string>

namespace telaviva {

// Storage for the launch-at-login command. Production uses the per-user Run key;
// tests substitute a fake that can fail on demand.
struct ILaunchRegistry {
    virtual ~ILaunchRegistry() = default;
    // Current Run value, nullopt when absent.
    virtual std::optional<std::wstring> read() = 0;
    virtual bool write(const std::wstring& value, std::wstring& error) = 0;
    // Absent already counts as success.
    virtual bool remove(std::wstring& error) = 0;
};

struct LaunchResult {
    bool ok = false;
    bool enabled = false;  // the verified state after the call
    std::wstring error;
};

class LaunchAtLogin {
public:
    LaunchAtLogin(ILaunchRegistry& registry, std::wstring executablePath);

    // True only when the stored value equals the expected command.
    bool isEnabled() const;

    // Reports success only after the stored value has been read back.
    LaunchResult setEnabled(bool enable);

    // "<path>" --background, with the path quoted so spaces survive the shell split.
    static std::wstring expectedCommand(const std::wstring& executablePath);

private:
    ILaunchRegistry& registry_;
    std::wstring executablePath_;
};

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "TelaVivaOLED", REG_SZ.
// Per-user, so no elevation is needed.
class RunKeyRegistry : public ILaunchRegistry {
public:
    std::optional<std::wstring> read() override;
    bool write(const std::wstring& value, std::wstring& error) override;
    bool remove(std::wstring& error) override;
};

}  // namespace telaviva
