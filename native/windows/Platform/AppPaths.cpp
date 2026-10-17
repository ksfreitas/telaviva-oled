#include "AppPaths.h"

#include <windows.h>
#include <shlobj.h>

#include <memory>
#include <system_error>
#include <utility>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace telaviva {

AppPaths::AppPaths(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path AppPaths::defaultRoot()
{
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    if (FAILED(hr)) {
        throw std::system_error(static_cast<int>(hr), std::system_category(),
                                "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed");
    }
    // The shell allocates the path, so it must be released with CoTaskMemFree.
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> guard(raw, &CoTaskMemFree);
    return std::filesystem::path(raw) / L"TelaVivaOLED";
}

const std::filesystem::path& AppPaths::root() const
{
    return root_;
}

std::filesystem::path AppPaths::settingsFile() const
{
    return root_ / L"settings.json";
}

std::filesystem::path AppPaths::calibrationSessionFile() const
{
    return root_ / L"calibration-session.json";
}

std::filesystem::path AppPaths::inferenceFailuresFile() const
{
    return root_ / L"inference-failures.json";
}

std::filesystem::path AppPaths::logFile() const
{
    return root_ / L"telaviva.log";
}

bool AppPaths::ensureCreated() const
{
    if (root_.empty()) {
        return false;
    }
    std::error_code ec;
    static_cast<void>(std::filesystem::create_directories(root_, ec));
    if (ec) {
        return false;
    }
    return std::filesystem::is_directory(root_, ec) && !ec;
}

}  // namespace telaviva
