#include "TestHarness.h"

#include "Platform/AppPaths.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using telaviva::AppPaths;

namespace {

// Unique per process and per call, so parallel runs do not collide.
fs::path uniqueTempBase()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("telaviva-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
}

}  // namespace

TEST(app_paths_files_are_under_root_with_expected_names)
{
    const fs::path root = uniqueTempBase() / "TelaVivaOLED";
    const AppPaths paths(root);
    CHECK(paths.root() == root);
    CHECK(paths.settingsFile() == root / "settings.json");
    CHECK(paths.calibrationSessionFile() == root / "calibration-session.json");
    CHECK(paths.inferenceFailuresFile() == root / "inference-failures.json");
    CHECK(paths.logFile() == root / "telaviva.log");
}

TEST(app_paths_ensure_created_makes_directory_idempotently)
{
    const fs::path base = uniqueTempBase();
    const fs::path root = base / "nested" / "TelaVivaOLED";
    {
        const AppPaths paths(root);
        CHECK(!fs::exists(root));
        CHECK(paths.ensureCreated());
        CHECK(fs::is_directory(root));
        CHECK(paths.ensureCreated());
    }
    std::error_code ec;
    static_cast<void>(fs::remove_all(base, ec));
}

TEST(app_paths_default_root_is_under_local_app_data)
{
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    CHECK(SUCCEEDED(hr));
    if (SUCCEEDED(hr)) {
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> guard(raw, &CoTaskMemFree);
        const fs::path localAppData(raw);
        const fs::path root = AppPaths::defaultRoot();
        CHECK(root.filename() == fs::path(L"TelaVivaOLED"));
        CHECK(root.parent_path() == localAppData);
    }
}
