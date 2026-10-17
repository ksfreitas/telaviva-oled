#include "TestHarness.h"

#include "FakeCamera.h"
#include "Platform/CameraStatus.h"
#include "Platform/ICameraBackend.h"

#include <cstddef>
#include <cstdint>
#include <string>

using telaviva::CameraFailure;
using telaviva::CameraStatusCode;
using telaviva::FakeCamera;
using telaviva::ICameraBackendCallbacks;
using telaviva::CameraFrame;
using telaviva::classifyCameraFailure;
using telaviva::cameraStatusString;

namespace {

constexpr uint32_t kAccessDenied = 0x80070005u;

CameraFailure failure(bool devicePresent = true, bool deadline = false, bool startFailed = false)
{
    CameraFailure value;
    value.devicePresent = devicePresent;
    value.deadlineExceeded = deadline;
    value.startFailed = startFailed;
    return value;
}

struct RecordingCallbacks final : ICameraBackendCallbacks {
    int frames = 0;
    int failures = 0;
    int devicesChanged = 0;
    int lastWidth = 0;
    uint32_t lastHresult = 0;

    void onFrame(const CameraFrame& frame) override
    {
        ++frames;
        lastWidth = frame.width;
    }

    void onFailure(const CameraFailure& value) override
    {
        ++failures;
        lastHresult = value.hresult;
    }

    void onDevicesChanged() override { ++devicesChanged; }
};

}  // namespace

TEST(camera_status_access_denied_is_permission)
{
    CameraFailure value = failure();
    value.hresult = kAccessDenied;
    CHECK(classifyCameraFailure(value) == CameraStatusCode::Permission);
}

TEST(camera_status_missing_device_is_unavailable)
{
    CHECK(classifyCameraFailure(failure(false)) == CameraStatusCode::Unavailable);
}

TEST(camera_status_deadline_is_busy)
{
    CHECK(classifyCameraFailure(failure(true, true)) == CameraStatusCode::Busy);
}

TEST(camera_status_start_failure_with_device_is_busy)
{
    CHECK(classifyCameraFailure(failure(true, false, true)) == CameraStatusCode::Busy);
}

TEST(camera_status_conversion_failure_is_error)
{
    CameraFailure value = failure();
    value.conversionFailed = true;
    CHECK(classifyCameraFailure(value) == CameraStatusCode::Error);
}

TEST(camera_status_inference_failure_is_error)
{
    CameraFailure value = failure();
    value.inferenceFailed = true;
    CHECK(classifyCameraFailure(value) == CameraStatusCode::Error);
}

TEST(camera_status_unknown_hresult_is_error)
{
    CameraFailure value = failure();
    value.hresult = 0x80004005u;
    CHECK(classifyCameraFailure(value) == CameraStatusCode::Error);
}

TEST(camera_status_precedence)
{
    // Permission beats a missing device.
    CameraFailure denied = failure(false);
    denied.hresult = kAccessDenied;
    CHECK(classifyCameraFailure(denied) == CameraStatusCode::Permission);

    // A missing device beats a deadline and a start failure.
    CHECK(classifyCameraFailure(failure(false, true, true)) == CameraStatusCode::Unavailable);

    // Busy beats the error flags when the device is present.
    CameraFailure mixed = failure(true, true, false);
    mixed.conversionFailed = true;
    CHECK(classifyCameraFailure(mixed) == CameraStatusCode::Busy);
}

TEST(camera_status_strings)
{
    // Compare as std::string: a const char* comparison would check pointers.
    CHECK_EQ(std::string(cameraStatusString(CameraStatusCode::Permission)), std::string("permission"));
    CHECK_EQ(std::string(cameraStatusString(CameraStatusCode::Unavailable)), std::string("unavailable"));
    CHECK_EQ(std::string(cameraStatusString(CameraStatusCode::Busy)), std::string("busy"));
    CHECK_EQ(std::string(cameraStatusString(CameraStatusCode::Error)), std::string("error"));
}

TEST(fake_camera_open_records_id_and_fps)
{
    FakeCamera camera;
    RecordingCallbacks callbacks;
    CHECK(camera.open("camera-a", 2.0, callbacks));
    CHECK_EQ(camera.openCalls().size(), static_cast<std::size_t>(1));
    if (!camera.openCalls().empty()) {
        CHECK_EQ(camera.openCalls()[0].id, std::string("camera-a"));
        CHECK(camera.openCalls()[0].targetFps == 2.0);
    }
    CHECK(camera.isOpen());
    CHECK_EQ(camera.counters().open, 1);
}

TEST(fake_camera_failed_open_reports_failure)
{
    FakeCamera camera;
    RecordingCallbacks callbacks;
    CameraFailure denied = failure();
    denied.hresult = kAccessDenied;
    camera.setOpenFailure(denied);

    CHECK(!camera.open("camera-a", 2.0, callbacks));
    CHECK(!camera.isOpen());
    CHECK_EQ(callbacks.failures, 1);
    CHECK_EQ(callbacks.lastHresult, kAccessDenied);
}

TEST(fake_camera_frames_reach_callbacks)
{
    FakeCamera camera;
    RecordingCallbacks callbacks;
    CHECK(camera.open("camera-a", 2.0, callbacks));
    camera.emitFrame(640, 480, 0x40);
    camera.emitFrame(320, 240, 0x10);
    CHECK_EQ(callbacks.frames, 2);
    CHECK_EQ(callbacks.lastWidth, 320);
    CHECK_EQ(camera.counters().frames, 2);
}

TEST(fake_camera_close_clears_open)
{
    FakeCamera camera;
    RecordingCallbacks callbacks;
    CHECK(camera.open("camera-a", 2.0, callbacks));
    camera.close();
    CHECK(!camera.isOpen());
    CHECK_EQ(camera.counters().close, 1);

    // Events after close are dropped, so the callbacks see nothing more.
    camera.emitFrame(640, 480, 0x40);
    CHECK_EQ(callbacks.frames, 0);
}
