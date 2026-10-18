#include "pch.h"

#include "App.xaml.h"
#include "Host/DisplayDiagnostics.h"
#include "Host/SmokeTest.h"
#include "Host/Strings.h"

#include <shellapi.h>

#include <string>
#include <string_view>

namespace {

std::string toUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// Usage: --print-string <lang> <key> <outfile>. Returns 3 when the key is missing,
// 4 for a usage or file error. The result goes to a file because console output
// is not reliable for this GUI-subsystem exe.
int runPrintString(int count, LPWSTR* args)
{
    if (count < 3) {
        return 4;
    }
    const std::string lang = toUtf8(args[0]);
    const std::string key = toUtf8(args[1]);
    const std::wstring outfile = args[2];

    telaviva::host::setLanguage(lang);
    const std::wstring text = telaviva::host::Str(key);
    const std::string utf8 = toUtf8(text);

    const HANDLE file = CreateFileW(outfile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 4;
    }
    DWORD written = 0;
    const DWORD expected = static_cast<DWORD>(utf8.size());
    const BOOL ok = WriteFile(file, utf8.data(), expected, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != expected) {
        return 4;
    }
    return text.starts_with(L"[") ? 3 : 0;
}

}  // namespace

// Replaces the XAML-generated wWinMain (DISABLE_XAML_GENERATED_MAIN is defined).
// The smoke test, print-string and display diagnostic modes return before any
// apartment or XAML setup, so they never open a window.
// Usage: --probe-laptop <outfile>. Read-only laptop detection, JSON to outfile.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool smokeTest = false;
    int printStringExit = -1;
    int displayExit = -1;
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view{ argv[i] } == L"--smoke-test") {
                smokeTest = true;
                break;
            }
            if (std::wstring_view{ argv[i] } == L"--print-string") {
                printStringExit = runPrintString(argc - i - 1, argv + i + 1);
                break;
            }
            if (std::wstring_view{ argv[i] } == L"--list-displays") {
                displayExit = (argc - i - 1 < 1) ? 4 : telaviva::host::runListDisplays(argv[i + 1]);
                break;
            }
            if (std::wstring_view{ argv[i] } == L"--probe-ddc") {
                displayExit = (argc - i - 1 < 1) ? 4 : telaviva::host::runProbeDdc(argv[i + 1]);
                break;
            }
            if (std::wstring_view{ argv[i] } == L"--probe-laptop") {
                displayExit = (argc - i - 1 < 1) ? 4 : telaviva::host::runProbeLaptop(argv[i + 1]);
                break;
            }
            if (std::wstring_view{ argv[i] } == L"--list-cameras") {
                displayExit = (argc - i - 1 < 1) ? 4 : telaviva::host::runListCameras(argv[i + 1]);
                break;
            }
        }
        LocalFree(argv);
    }

    if (printStringExit >= 0) {
        return printStringExit;
    }

    if (displayExit >= 0) {
        return displayExit;
    }

    if (smokeTest) {
        return telaviva::host::runSmokeTest();
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    ::winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
        ::winrt::make<::winrt::TelaViva::implementation::App>();
    });
    return 0;
}
