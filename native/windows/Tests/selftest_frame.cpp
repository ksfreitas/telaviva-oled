#include "TestHarness.h"

#include "Platform/SelftestFrame.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

using telaviva::loadSelftestFrame;
using telaviva::parseSelftestFrame;

namespace {

void putLe32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

// Header with the given fields followed by pixelBytes bytes of a recognizable pattern.
std::vector<uint8_t> makeFile(uint32_t width, uint32_t height, uint32_t stride, std::size_t pixelBytes)
{
    std::vector<uint8_t> out = {'T', 'V', 'S', 'F'};
    putLe32(out, width);
    putLe32(out, height);
    putLe32(out, stride);
    for (std::size_t i = 0; i < pixelBytes; ++i) {
        out.push_back(static_cast<uint8_t>(i & 0xFFu));
    }
    return out;
}

}  // namespace

TEST(selftest_frame_valid_file_yields_frame_with_pixels_after_header)
{
    // 320x240 with a tight stride, the shape the packaging script writes.
    const std::vector<uint8_t> bytes = makeFile(320, 240, 320 * 4, 320 * 4 * 240);
    const telaviva::SelftestFrameLoadResult result = parseSelftestFrame(bytes);
    CHECK(result.ok());
    CHECK(result.frame.width == 320 && result.frame.height == 240 && result.frame.stride == 1280);
    CHECK(result.frame.bgra.size() == 1280u * 240u);
    CHECK(result.frame.bgra.size() > 0 && result.frame.bgra[0] == 0 && result.frame.bgra[1] == 1);
}

TEST(selftest_frame_accepts_padded_stride)
{
    const std::vector<uint8_t> bytes = makeFile(4, 2, 32, 64);
    const telaviva::SelftestFrameLoadResult result = parseSelftestFrame(bytes);
    CHECK(result.ok());
    CHECK(result.frame.stride == 32 && result.frame.bgra.size() == 64u);
}

TEST(selftest_frame_bad_magic_is_rejected)
{
    std::vector<uint8_t> bytes = makeFile(4, 2, 16, 32);
    bytes[0] = 'X';
    CHECK(!parseSelftestFrame(bytes).ok());
}

TEST(selftest_frame_short_header_is_rejected)
{
    const std::vector<uint8_t> bytes = {'T', 'V', 'S', 'F', 4, 0, 0, 0};
    CHECK(!parseSelftestFrame(bytes).ok());
}

TEST(selftest_frame_size_mismatch_is_rejected)
{
    // One byte short, and one byte long, of the header-declared size.
    CHECK(!parseSelftestFrame(makeFile(4, 2, 16, 31)).ok());
    CHECK(!parseSelftestFrame(makeFile(4, 2, 16, 33)).ok());
}

TEST(selftest_frame_zero_dimensions_are_rejected)
{
    CHECK(!parseSelftestFrame(makeFile(0, 2, 0, 0)).ok());
    CHECK(!parseSelftestFrame(makeFile(4, 0, 16, 0)).ok());
}

TEST(selftest_frame_oversized_dimensions_are_rejected)
{
    CHECK(!parseSelftestFrame(makeFile(4097, 2, 4097 * 4, 0)).ok());
    CHECK(!parseSelftestFrame(makeFile(4, 4097, 16, 0)).ok());
    // Stride above the cap is rejected even when the byte count would match.
    CHECK(!parseSelftestFrame(makeFile(4, 1, 16388, 16388)).ok());
}

TEST(selftest_frame_stride_below_row_width_is_rejected)
{
    CHECK(!parseSelftestFrame(makeFile(4, 2, 15, 30)).ok());
}

TEST(selftest_frame_missing_file_is_an_error)
{
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "telaviva-selftest-missing.bgra";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    CHECK(!loadSelftestFrame(missing).ok());
}
