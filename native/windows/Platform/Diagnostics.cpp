#include "Diagnostics.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace telaviva {
namespace {

constexpr std::uintmax_t kMaxLogBytes = 1024 * 1024;

const char* levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Notice:
        return "NOTICE";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    }
    return "DEBUG";
}

std::string utcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const int millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &seconds);

    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return buffer;
}

}  // namespace

Diagnostics::Diagnostics(std::filesystem::path logFile, LogLevel level)
    : logFile_(std::move(logFile)), level_(level)
{
}

void Diagnostics::setLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

void Diagnostics::log(LogLevel level, std::string_view message)
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<int>(level) > static_cast<int>(level_)) {
            return;
        }
        rollIfNeeded();
        std::ofstream out(logFile_, std::ios::binary | std::ios::app);
        if (!out) {
            return;
        }
        out << utcTimestamp() << " [" << levelName(level) << "] " << message << '\n';
    } catch (...) {
        // Logging must never take the app down, so the line is dropped.
    }
}

// Caller holds mutex_. Keeps one previous file so the newest history survives.
void Diagnostics::rollIfNeeded()
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(logFile_, ec);
    if (ec || size <= kMaxLogBytes) {
        return;
    }
    std::filesystem::path backup = logFile_;
    backup += ".1";
    static_cast<void>(std::filesystem::remove(backup, ec));
    std::filesystem::rename(logFile_, backup, ec);
}

std::string shortId(std::string_view identifier)
{
    // FNV-1a 32-bit: small, dependency-free and deterministic across runs.
    std::uint32_t hash = 2166136261u;
    for (const char c : identifier) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    char buffer[12];
    std::snprintf(buffer, sizeof buffer, "id-%08x", static_cast<unsigned int>(hash));
    return buffer;
}

}  // namespace telaviva
