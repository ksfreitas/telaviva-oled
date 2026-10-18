#pragma once

#include <string>

namespace telaviva::host {

// Headless, read-only diagnostics for spike S2. Both write UTF-8 JSON (no BOM)
// to outFile and never write a VCP value. Return 0 on success and 4 when the
// file cannot be written. Run before any XAML or WinRT initialization.
int runListDisplays(const std::wstring& outFile);
int runProbeDdc(const std::wstring& outFile);

// --probe-laptop: runs the real laptop detection once and records the result as
// JSON. It reads the WMI panel brightness at most once and never writes it.
int runProbeLaptop(const std::wstring& outFile);

// --list-cameras: raw frame source groups, VideoCapture devices and the backend's enumerate() result, as
// JSON. Opens no camera. A section that fails is written as null.
int runListCameras(const std::wstring& outFile);

}  // namespace telaviva::host
