#include "TestHarness.h"

#include "Platform/CameraFormatSelection.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace {

telaviva::CameraFormatCandidate makeFormat(uint32_t width, uint32_t height, const std::string& subtype, double fps = 30)
{
    telaviva::CameraFormatCandidate format;
    format.width = width;
    format.height = height;
    format.subtype = subtype;
    format.fps = fps;
    return format;
}

}  // namespace

TEST(camera_format_prefers_size_closest_to_640x480)
{
    const std::vector<telaviva::CameraFormatCandidate> formats = {
        makeFormat(1920, 1080, "NV12"), makeFormat(640, 480, "YUY2"), makeFormat(1280, 720, "MJPG")};
    CHECK_EQ(telaviva::chooseCameraFormat(formats), std::size_t(1));
}

TEST(camera_format_tie_prefers_bgra_over_mjpg)
{
    const std::vector<telaviva::CameraFormatCandidate> formats = {
        makeFormat(640, 480, "MJPG"), makeFormat(640, 480, "BGRA8")};
    CHECK_EQ(telaviva::chooseCameraFormat(formats), std::size_t(1));
}

TEST(camera_format_tie_prefers_higher_fps_within_subtype)
{
    const std::vector<telaviva::CameraFormatCandidate> formats = {
        makeFormat(640, 480, "NV12", 15), makeFormat(640, 480, "NV12", 30)};
    CHECK_EQ(telaviva::chooseCameraFormat(formats), std::size_t(1));
}

TEST(camera_format_subtype_match_ignores_case)
{
    const std::vector<telaviva::CameraFormatCandidate> formats = {makeFormat(640, 480, "yuy2")};
    CHECK_EQ(telaviva::chooseCameraFormat(formats), std::size_t(0));
}

TEST(camera_format_skips_unsupported_subtype)
{
    const std::vector<telaviva::CameraFormatCandidate> formats = {
        makeFormat(640, 480, "D16"), makeFormat(1280, 720, "YUY2")};
    CHECK_EQ(telaviva::chooseCameraFormat(formats), std::size_t(1));
}

TEST(camera_format_none_usable_gives_no_format)
{
    const std::vector<telaviva::CameraFormatCandidate> empty;
    CHECK_EQ(telaviva::chooseCameraFormat(empty), telaviva::kNoCameraFormat);

    const std::vector<telaviva::CameraFormatCandidate> unsupported = {makeFormat(640, 480, "D16")};
    CHECK_EQ(telaviva::chooseCameraFormat(unsupported), telaviva::kNoCameraFormat);
}

TEST(camera_format_skips_zero_size)
{
    const std::vector<telaviva::CameraFormatCandidate> formats = {
        makeFormat(0, 0, "NV12"), makeFormat(320, 240, "NV12")};
    CHECK_EQ(telaviva::chooseCameraFormat(formats), std::size_t(1));
}

TEST(camera_format_infrared_only_when_no_color_source)
{
    CHECK(telaviva::isInfraredOnlySource(false, true));
    CHECK(!telaviva::isInfraredOnlySource(true, true));
    CHECK(!telaviva::isInfraredOnlySource(true, false));
    CHECK(!telaviva::isInfraredOnlySource(false, false));
}

TEST(camera_frame_rate_zero_target_keeps_every_frame)
{
    const double first = telaviva::negativeInfinitySeconds();
    CHECK(telaviva::shouldRetainFrame(first, 10.0, 0.0));
    CHECK(telaviva::shouldRetainFrame(10.0, 10.0, 0.0));
    CHECK(telaviva::shouldRetainFrame(10.0, 10.0, -1.0));
}

TEST(camera_frame_rate_first_frame_is_retained)
{
    CHECK(telaviva::shouldRetainFrame(telaviva::negativeInfinitySeconds(), 0.0, 2.0));
}

TEST(camera_frame_rate_limits_to_target)
{
    // Target 2 fps means at least 0.5 s between kept frames.
    CHECK(!telaviva::shouldRetainFrame(10.0, 10.25, 2.0));
    CHECK(!telaviva::shouldRetainFrame(10.0, 10.49, 2.0));
    CHECK(telaviva::shouldRetainFrame(10.0, 10.5, 2.0));
    CHECK(telaviva::shouldRetainFrame(10.0, 11.0, 2.0));
}

TEST(camera_frame_rate_nan_target_keeps_nothing)
{
    // A NaN target is never a valid setting, so the limiter drops frames instead of flooding.
    CHECK(!telaviva::shouldRetainFrame(0.0, 100.0, std::numeric_limits<double>::quiet_NaN()));
}
