#pragma once
#include <filesystem>

namespace telaviva {

// File locations under %LOCALAPPDATA%\TelaVivaOLED, the same user-level data
// folder the core's config_path uses.
class AppPaths {
public:
    // Throws std::system_error if the Local AppData folder cannot be resolved.
    // Never falls back to a relative path.
    explicit AppPaths(std::filesystem::path root = defaultRoot());

    static std::filesystem::path defaultRoot();

    const std::filesystem::path& root() const;
    std::filesystem::path settingsFile() const;
    std::filesystem::path calibrationSessionFile() const;
    std::filesystem::path autoBrightnessHoldFile() const;
    std::filesystem::path builtinProtectionSessionFile() const;
    std::filesystem::path inferenceFailuresFile() const;
    std::filesystem::path logFile() const;

    // Returns true when the root directory exists after the call.
    bool ensureCreated() const;

private:
    std::filesystem::path root_;
};

}  // namespace telaviva
