#pragma once
// Pure camera policy with no WinRT, so the rules can be tested without a device.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace telaviva {

struct CameraFormatCandidate {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string subtype;  // MediaFrameFormat.Subtype, compared case-insensitively
    double fps = 0;
};

inline constexpr std::size_t kNoCameraFormat = static_cast<std::size_t>(-1);

namespace camera_format_detail {

inline char asciiUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

inline bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiUpper(a[i]) != asciiUpper(b[i])) {
            return false;
        }
    }
    return true;
}

// Lower rank wins a tie. -1 means the format is not offered for capture.
inline int subtypeRank(const std::string& subtype)
{
    static constexpr std::array<std::string_view, 4> kPreferred = {"BGRA8", "NV12", "YUY2", "MJPG"};
    for (std::size_t i = 0; i < kPreferred.size(); ++i) {
        if (equalsIgnoreAsciiCase(subtype, kPreferred[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline uint64_t distanceTo640x480(uint32_t width, uint32_t height)
{
    const int64_t dw = static_cast<int64_t>(width) - 640;
    const int64_t dh = static_cast<int64_t>(height) - 480;
    return static_cast<uint64_t>(dw < 0 ? -dw : dw) + static_cast<uint64_t>(dh < 0 ? -dh : dh);
}

}  // namespace camera_format_detail

// Index of the offered format closest to 640x480. Ties go to the preferred
// subtype, then to the higher frame rate. Returns kNoCameraFormat when no
// candidate has a supported subtype and a nonzero size.
inline std::size_t chooseCameraFormat(const std::vector<CameraFormatCandidate>& formats)
{
    std::size_t best = kNoCameraFormat;
    uint64_t bestDistance = 0;
    int bestRank = 0;
    double bestFps = 0;
    for (std::size_t i = 0; i < formats.size(); ++i) {
        const CameraFormatCandidate& format = formats[i];
        const int rank = camera_format_detail::subtypeRank(format.subtype);
        if (rank < 0 || format.width == 0 || format.height == 0) {
            continue;
        }
        const uint64_t distance = camera_format_detail::distanceTo640x480(format.width, format.height);
        const bool better = best == kNoCameraFormat || distance < bestDistance ||
                            (distance == bestDistance &&
                             (rank < bestRank || (rank == bestRank && format.fps > bestFps)));
        if (better) {
            best = i;
            bestDistance = distance;
            bestRank = rank;
            bestFps = format.fps;
        }
    }
    return best;
}

// A source group is infrared only when it offers an infrared source and no color source.
// Windows Hello style groups carry both and stay selectable as color cameras.
inline bool isInfraredOnlySource(bool hasColorSource, bool hasInfraredSource)
{
    return hasInfraredSource && !hasColorSource;
}

// Frame-rate limit. Callers start with lastRetainedSeconds set to negative infinity
// so the first frame is always kept. A target of zero or less keeps every frame.
inline bool shouldRetainFrame(double lastRetainedSeconds, double nowSeconds, double targetFps)
{
    if (targetFps <= 0) {
        return true;
    }
    return nowSeconds - lastRetainedSeconds >= 1.0 / targetFps;
}

inline double negativeInfinitySeconds()
{
    return -std::numeric_limits<double>::infinity();
}

}  // namespace telaviva
