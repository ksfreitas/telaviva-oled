#include "SelftestFrame.h"

#include <cstddef>
#include <fstream>
#include <cstdint>
#include <iterator>
#include <system_error>
#include <utility>

namespace telaviva {

namespace {

constexpr std::size_t kHeaderSize = 16;
constexpr uint32_t kMaxDimension = 4096;
constexpr uint32_t kMaxStride = kMaxDimension * 4;
constexpr uint8_t kMagic[4] = {'T', 'V', 'S', 'F'};

uint32_t readLe32(const uint8_t* at)
{
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
           (static_cast<uint32_t>(at[3]) << 24);
}

SelftestFrameLoadResult rejected(std::string reason)
{
    SelftestFrameLoadResult result;
    result.error = std::move(reason);
    return result;
}

}  // namespace

SelftestFrameLoadResult parseSelftestFrame(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < kHeaderSize) {
        return rejected("file is shorter than the 16-byte header");
    }
    for (std::size_t i = 0; i < sizeof kMagic; ++i) {
        if (bytes[i] != kMagic[i]) {
            return rejected("magic is not TVSF");
        }
    }
    const uint32_t width = readLe32(bytes.data() + 4);
    const uint32_t height = readLe32(bytes.data() + 8);
    const uint32_t stride = readLe32(bytes.data() + 12);
    if (width == 0 || height == 0) {
        return rejected("width or height is zero");
    }
    if (width > kMaxDimension || height > kMaxDimension) {
        return rejected("width or height exceeds 4096");
    }
    if (stride < width * 4 || stride > kMaxStride) {
        return rejected("stride is smaller than width * 4 or larger than 16384");
    }
    // 64-bit arithmetic: the largest accepted frame is far below the size_t limit on 64-bit builds.
    const uint64_t pixelBytes = static_cast<uint64_t>(stride) * static_cast<uint64_t>(height);
    if (static_cast<uint64_t>(bytes.size()) != kHeaderSize + pixelBytes) {
        return rejected("file size does not match the header dimensions");
    }

    SelftestFrameLoadResult result;
    result.frame.width = static_cast<int>(width);
    result.frame.height = static_cast<int>(height);
    result.frame.stride = static_cast<int>(stride);
    result.frame.bgra.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), bytes.end());
    return result;
}

SelftestFrameLoadResult loadSelftestFrame(const std::filesystem::path& file)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        return rejected("file cannot be read");
    }
    // Refuse anything larger than the biggest accepted frame before reading it.
    const std::uintmax_t maxSize = kHeaderSize + static_cast<std::uintmax_t>(kMaxStride) * kMaxDimension;
    if (size > maxSize) {
        return rejected("file is larger than any accepted frame");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return rejected("file cannot be opened");
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return rejected("file cannot be read");
    }
    return parseSelftestFrame(bytes);
}

}  // namespace telaviva
