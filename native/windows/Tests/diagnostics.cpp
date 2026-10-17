#include "TestHarness.h"

#include "Platform/Diagnostics.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using telaviva::Diagnostics;
using telaviva::LogLevel;
using telaviva::shortId;

namespace {

fs::path uniqueTempDir()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("telaviva-diag-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
}

std::string readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST(diagnostics_skips_messages_above_configured_level)
{
    const fs::path dir = uniqueTempDir();
    static_cast<void>(fs::create_directories(dir));
    const fs::path file = dir / "telaviva.log";
    {
        Diagnostics diagnostics(file, LogLevel::Info);
        diagnostics.log(LogLevel::Error, "error-line");
        diagnostics.log(LogLevel::Info, "info-line");
        diagnostics.log(LogLevel::Debug, "debug-line");
    }
    const std::string text = readAll(file);
    CHECK(text.find("error-line") != std::string::npos);
    CHECK(text.find("info-line") != std::string::npos);
    CHECK(text.find("debug-line") == std::string::npos);

    std::error_code ec;
    static_cast<void>(fs::remove_all(dir, ec));
}

TEST(diagnostics_default_level_is_error_and_setLevel_changes_it)
{
    const fs::path dir = uniqueTempDir();
    static_cast<void>(fs::create_directories(dir));
    const fs::path file = dir / "telaviva.log";
    {
        Diagnostics diagnostics(file);
        diagnostics.log(LogLevel::Info, "default-info");
        diagnostics.setLevel(LogLevel::Debug);
        diagnostics.log(LogLevel::Debug, "after-debug");
    }
    const std::string text = readAll(file);
    CHECK(text.find("default-info") == std::string::npos);
    CHECK(text.find("after-debug") != std::string::npos);

    std::error_code ec;
    static_cast<void>(fs::remove_all(dir, ec));
}

TEST(diagnostics_line_has_iso_utc_timestamp_and_level)
{
    const fs::path dir = uniqueTempDir();
    static_cast<void>(fs::create_directories(dir));
    const fs::path file = dir / "telaviva.log";
    {
        Diagnostics diagnostics(file, LogLevel::Error);
        diagnostics.log(LogLevel::Error, "stamped");
    }
    const std::string text = readAll(file);
    // Shape: 2026-10-17T12:34:56.789Z [ERROR] stamped
    CHECK(text.size() > 24);
    if (text.size() > 24) {
        CHECK(text[4] == '-');
        CHECK(text[10] == 'T');
        CHECK(text[13] == ':');
        CHECK(text[19] == '.');
        CHECK(text[23] == 'Z');
    }
    CHECK(text.find("[ERROR] stamped\n") != std::string::npos);

    std::error_code ec;
    static_cast<void>(fs::remove_all(dir, ec));
}

TEST(diagnostics_rolls_file_over_at_256_kib)
{
    const fs::path dir = uniqueTempDir();
    static_cast<void>(fs::create_directories(dir));
    const fs::path file = dir / "telaviva.log";
    const fs::path backup = dir / "telaviva.log.1";

    // A stale backup must be replaced, not kept.
    {
        std::ofstream stale(backup, std::ios::binary);
        stale << "stale";
    }

    const std::string message(1000, 'x');
    {
        Diagnostics diagnostics(file, LogLevel::Info);
        // About 1 KB per line, so 300 lines is well past one 256 KiB file.
        for (int i = 0; i < 300; ++i) {
            diagnostics.log(LogLevel::Info, message);
        }
    }
    CHECK(fs::exists(backup));
    if (fs::exists(backup)) {
        CHECK(fs::file_size(backup) > std::uintmax_t{256} * 1024);
    }
    CHECK(fs::exists(file));
    if (fs::exists(file)) {
        CHECK(fs::file_size(file) <= std::uintmax_t{256} * 1024);
    }

    std::error_code ec;
    static_cast<void>(fs::remove_all(dir, ec));
}

TEST(diagnostics_never_throws_when_directory_is_missing)
{
    const fs::path file = uniqueTempDir() / "missing" / "telaviva.log";
    bool threw = false;
    try {
        Diagnostics diagnostics(file, LogLevel::Debug);
        diagnostics.log(LogLevel::Error, "dropped");
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
}

TEST(diagnostics_short_id_is_deterministic_and_eleven_characters)
{
    const std::string first = shortId("USB\\VID_1234&PID_5678\\SERIAL0001");
    const std::string second = shortId("USB\\VID_1234&PID_5678\\SERIAL0001");
    CHECK_EQ(first.size(), std::size_t{11});
    CHECK(first.rfind("id-", 0) == 0);
    CHECK_EQ(first, second);
}

TEST(diagnostics_short_id_matches_fnv1a_vectors)
{
    CHECK_EQ(shortId(""), std::string("id-811c9dc5"));
    CHECK_EQ(shortId("a"), std::string("id-e40c292c"));
}

TEST(diagnostics_short_id_never_contains_input)
{
    const std::string secret = "device-serial-ABC123";
    const std::string id = shortId(secret);
    CHECK(id.find(secret) == std::string::npos);
    CHECK(id.find("ABC123") == std::string::npos);
    CHECK(id.find("serial") == std::string::npos);
}
