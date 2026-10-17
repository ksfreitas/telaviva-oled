#pragma once

namespace telaviva::host {

// Headless check of the Rust DLLs. Runs before any XAML or WinRT initialization
// and never creates a window. Returns 0 on success, 1 when the core ABI is not 1,
// and 2 when the inference DLL cannot be loaded or lacks tv_inference_detect_bgra.
int runSmokeTest();

}  // namespace telaviva::host
