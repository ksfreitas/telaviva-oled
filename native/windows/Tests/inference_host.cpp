#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/FrameAnalyzer.h"
#include "Platform/ICameraBackend.h"
#include "Platform/InferenceHost.h"
#include "Platform/Scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using telaviva::AnalysisStatus;
using telaviva::CameraFrame;
using telaviva::FakeScheduler;
using telaviva::IDetectorApi;
using telaviva::InferenceHost;

namespace {

const wchar_t* const kRuntimePath = L"C:\\TelaViva\\onnxruntime.dll";
const wchar_t* const kModelPath = L"C:\\TelaViva\\face_detection_yunet_2023mar.onnx";

// Records every call and returns scripted statuses. Handles are small integers
// so the tests can tell detectors apart.
class FakeDetectorApi final : public IDetectorApi {
public:
    int createFailures = 0;  // number of upcoming create() calls that return nullptr
    int detectStatus = 0;
    int assessStatus = 0;
    int provider = 0;

    int creates = 0;
    int destroys = 0;
    int detectCalls = 0;
    int assessCalls = 0;
    std::vector<void*> detectHandles;
    std::vector<void*> destroyed;

    void* create(const std::wstring&, const std::wstring&, int, uint32_t) override
    {
        ++creates;
        if (createFailures > 0) {
            --createFailures;
            return nullptr;
        }
        return handleFor(creates);
    }

    int detect(void* handle, const uint8_t*, size_t, int, int, int) override
    {
        ++detectCalls;
        detectHandles.push_back(handle);
        return detectStatus;
    }

    int assessLight(const uint8_t*, size_t, int, int, int) override
    {
        ++assessCalls;
        return assessStatus;
    }

    int selectedProvider(void*) override { return provider; }

    int directmlAvailable() override { return 0; }

    void destroy(void* handle) override
    {
        ++destroys;
        destroyed.push_back(handle);
    }

private:
    static void* handleFor(int n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(n)); }
};

// Allocates bgra to exactly stride * height bytes, so the default frame is valid.
CameraFrame makeFrame(int width, int height, int stride)
{
    CameraFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.bgra.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height), 0);
    return frame;
}

CameraFrame validFrame()
{
    return makeFrame(4, 4, 16);
}

}  // namespace

TEST(inference_host_detect_creates_detector_lazily_and_assess_light_does_not)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    CHECK_EQ(api.creates, 0);
    CHECK(host.assessLight(validFrame()).status == AnalysisStatus::UsableLight);
    CHECK_EQ(api.creates, 0);
    CHECK_EQ(host.activeProvider(), -1);
    host.detect(validFrame());
    CHECK_EQ(api.creates, 1);
}

TEST(inference_host_detect_maps_status_codes)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    api.detectStatus = 0;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Absent);
    api.detectStatus = 1;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    api.detectStatus = 2;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::InsufficientLight);
    api.detectStatus = -2;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Error);
    api.detectStatus = 3;  // TV_INFERENCE_UNAVAILABLE is never a detect result
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Error);
}

TEST(inference_host_detector_is_reused_across_calls)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    host.detect(validFrame());
    host.detect(validFrame());
    CHECK_EQ(api.creates, 1);
    CHECK_EQ(api.destroys, 0);
    CHECK(api.detectHandles.size() == 3 && api.detectHandles[0] == api.detectHandles[2]);
}

TEST(inference_host_idle_timer_destroys_detector_after_30_seconds)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    CHECK_EQ(scheduler.pendingCount(), std::size_t(1));
    CHECK_EQ(scheduler.toleranceLog().back().count(), telaviva::toleranceFor(30s).count());
    scheduler.advance(29999ms);
    CHECK_EQ(api.destroys, 0);
    scheduler.advance(1ms);
    CHECK_EQ(api.destroys, 1);
    CHECK(api.destroyed.size() == 1 && api.destroyed[0] == api.detectHandles[0]);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(0));
}

TEST(inference_host_detect_rearms_idle_timer_on_every_use)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    scheduler.advance(20s);
    host.detect(validFrame());
    CHECK_EQ(scheduler.pendingCount(), std::size_t(1));
    // 40 s after the first use but only 20 s after the second: still loaded.
    scheduler.advance(20s);
    CHECK_EQ(api.destroys, 0);
    scheduler.advance(10s);
    CHECK_EQ(api.destroys, 1);
}

TEST(inference_host_detect_after_idle_release_recreates_detector)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    scheduler.advance(30s);
    CHECK_EQ(api.destroys, 1);
    host.detect(validFrame());
    CHECK_EQ(api.creates, 2);
}

TEST(inference_host_detect_after_release_idle_recreates_detector)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    host.releaseIdle();
    CHECK_EQ(api.destroys, 1);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(0));
    host.detect(validFrame());
    CHECK_EQ(api.creates, 2);
    CHECK_EQ(host.activeProvider(), 0);
}

TEST(inference_host_create_failure_returns_error_then_retries)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    api.createFailures = 1;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Error);
    CHECK_EQ(api.creates, 1);
    CHECK_EQ(api.detectCalls, 0);
    CHECK_EQ(host.activeProvider(), -1);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(0));
    api.detectStatus = 1;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(api.creates, 2);
    CHECK_EQ(api.detectCalls, 1);
}

TEST(inference_host_invalid_frames_return_error_without_api_calls)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);

    CameraFrame shortBuffer = makeFrame(4, 4, 16);
    shortBuffer.bgra.resize(63);
    const std::vector<CameraFrame> invalid = {
        makeFrame(0, 0, 0),    // zero size
        makeFrame(0, 4, 16),   // zero width
        makeFrame(4, 0, 16),   // zero height
        makeFrame(4, 4, 15),   // stride shorter than width * 4
        shortBuffer,           // bytes shorter than stride * height
    };
    for (const CameraFrame& frame : invalid) {
        CHECK(host.detect(frame).status == AnalysisStatus::Error);
        CHECK(host.assessLight(frame).status == AnalysisStatus::Error);
    }
    CHECK_EQ(api.creates, 0);
    CHECK_EQ(api.detectCalls, 0);
    CHECK_EQ(api.assessCalls, 0);

    // The exact-size boundary is still accepted.
    CHECK(host.detect(makeFrame(4, 4, 16)).status == AnalysisStatus::Absent);
    CHECK_EQ(api.detectCalls, 1);
}

TEST(inference_host_assess_light_maps_status_codes)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    api.assessStatus = 0;
    CHECK(host.assessLight(validFrame()).status == AnalysisStatus::UsableLight);
    api.assessStatus = 1;
    CHECK(host.assessLight(validFrame()).status == AnalysisStatus::UsableLight);
    api.assessStatus = 2;
    CHECK(host.assessLight(validFrame()).status == AnalysisStatus::InsufficientLight);
    api.assessStatus = -3;
    CHECK(host.assessLight(validFrame()).status == AnalysisStatus::Error);
    CHECK_EQ(api.assessCalls, 4);
    CHECK_EQ(api.detectCalls, 0);
    CHECK_EQ(api.creates, 0);
}

TEST(inference_host_release_idle_destroys_at_once)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    host.releaseIdle();
    CHECK_EQ(api.destroys, 1);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(0));
    // Releasing with no detector is a no-op, and the old timer cannot destroy twice.
    host.releaseIdle();
    scheduler.advance(30s);
    CHECK_EQ(api.destroys, 1);
}

TEST(inference_host_destructor_destroys_detector_and_cancels_timer)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    {
        InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
        host.detect(validFrame());
    }
    CHECK_EQ(api.destroys, 1);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(0));
}

TEST(inference_host_active_provider_tracks_loaded_detector)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    CHECK_EQ(host.activeProvider(), -1);
    host.detect(validFrame());
    CHECK_EQ(host.activeProvider(), 0);
    host.releaseIdle();
    CHECK_EQ(host.activeProvider(), -1);
}
