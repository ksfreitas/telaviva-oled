#pragma once
// Loader for selftest-face.bgra, the frame the DirectML self-test runs on. The file is
// written by tools/windows/make-selftest-frame.ps1 and ships next to TelaViva.exe.
// Errors are returned, never thrown; a rejected file means no self-test frame, so
// acceleration stays off.
#include "ICameraBackend.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace telaviva {

// Layout: 4 bytes magic "TVSF", then uint32 width, height and stride (little endian),
// then stride * height bytes of top-down BGRA8. Nothing may follow the pixels.
struct SelftestFrameLoadResult {
    CameraFrame frame;
    std::string error;  // empty when the file was accepted
    bool ok() const { return error.empty(); }
};

// Strict checks: magic, nonzero dimensions no larger than 4096, stride at least width * 4
// and at most 4096 * 4, and a byte count equal to header plus stride * height.
SelftestFrameLoadResult parseSelftestFrame(const std::vector<uint8_t>& bytes);

// Reads the whole file, then applies parseSelftestFrame. A missing or unreadable file is an error.
SelftestFrameLoadResult loadSelftestFrame(const std::filesystem::path& file);

}  // namespace telaviva
