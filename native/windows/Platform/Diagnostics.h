#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace telaviva {

// Ordered from most to least severe; a message is written when its level is
// at or above the configured one, i.e. its value is <= the configured value.
enum class LogLevel { Error, Info, Debug };

// Appends UTF-8 lines to a log file. Thread-safe. Never throws.
class Diagnostics {
public:
    explicit Diagnostics(std::filesystem::path logFile, LogLevel level = LogLevel::Error);

    void setLevel(LogLevel level);

    // Camera frames and device identifiers must never be passed to log().
    // Use shortId() for anything that identifies a device or camera.
    void log(LogLevel level, std::string_view message);

private:
    void rollIfNeeded();

    std::mutex mutex_;
    std::filesystem::path logFile_;
    LogLevel level_;
};

// Stable "id-xxxxxxxx" token (FNV-1a 32-bit, lowercase hex) that lets logs
// correlate a device without writing its identifier.
std::string shortId(std::string_view identifier);

}  // namespace telaviva
