#include "pch.h"

#include "SmokeTest.h"

#include "Platform/CoreBridge.h"

#include <cstdio>

namespace telaviva::host {

namespace {

// The console is attached to the parent only so the result is visible when run
// from a terminal; a GUI-subsystem exe has no stdout otherwise.
void attachParentConsole()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        _wfreopen_s(&stream, L"CONOUT$", L"w", stdout);
    }
}

}  // namespace

int runSmokeTest()
{
    attachParentConsole();

    const uint32_t coreAbi = CoreBridge::abiVersion();
    if (coreAbi != 1) {
        std::printf("TelaViva smoke test: core ABI %u, expected 1\n", coreAbi);
        return 1;
    }

    HMODULE inference = LoadLibraryW(L"telaviva_inference.dll");
    if (inference == nullptr) {
        std::printf("TelaViva smoke test: inference DLL could not be loaded (error %lu)\n", GetLastError());
        return 2;
    }

    const bool hasDetect = GetProcAddress(inference, "tv_inference_detect_bgra") != nullptr;
    FreeLibrary(inference);
    if (!hasDetect) {
        std::printf("TelaViva smoke test: inference DLL lacks tv_inference_detect_bgra\n");
        return 2;
    }

    std::printf("TelaViva smoke test: core ABI 1, inference DLL ok\n");
    return 0;
}

}  // namespace telaviva::host
