#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/AdapterCatalog.h"
#include "Platform/DeviceSelection.h"
#include "Platform/FrameAnalyzer.h"
#include "Platform/ICameraBackend.h"
#include "Platform/IClock.h"
#include "Platform/InferenceFailureStore.h"
#include "Platform/InferenceHost.h"
#include "Platform/Scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
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

constexpr int kCpuProvider = 0;
constexpr int kDirectMlProvider = 2;

// Records every call and returns scripted statuses. Handles are small integers
// so the tests can tell detectors apart. Each session reports the provider it was
// created with; DirectML sessions answer with acceleratedDetectStatus, the rest with detectStatus.
class FakeDetectorApi final : public IDetectorApi {
public:
    int createFailures = 0;  // number of upcoming create() calls that return nullptr
    int detectStatus = 0;    // detect result on CPU sessions
    int acceleratedDetectStatus = 0;  // detect result on DirectML sessions
    int assessStatus = 0;
    telaviva::FakeClock* clock = nullptr;  // when set, every detect advances it by stepSeconds
    double stepSeconds = 0.0;

    int creates = 0;
    int destroys = 0;
    int detectCalls = 0;
    int assessCalls = 0;
    std::vector<void*> detectHandles;
    std::vector<void*> destroyed;
    std::vector<int> createdProviders;  // one per create() call, failed ones included
    std::vector<uint32_t> createdDeviceIndexes;

    void* create(const std::wstring&, const std::wstring&, int provider, uint32_t deviceIndex) override
    {
        ++creates;
        createdProviders.push_back(provider);
        createdDeviceIndexes.push_back(deviceIndex);
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
        if (clock != nullptr) {
            clock->advance(stepSeconds);
        }
        return providerOf(handle) == kDirectMlProvider ? acceleratedDetectStatus : detectStatus;
    }

    int assessLight(const uint8_t*, size_t, int, int, int) override
    {
        ++assessCalls;
        return assessStatus;
    }

    int selectedProvider(void* handle) override { return providerOf(handle); }

    int directmlAvailable() override { return 0; }

    void destroy(void* handle) override
    {
        ++destroys;
        destroyed.push_back(handle);
    }

    // Device index of the most recent DirectML session, or the maximum value when there was none.
    uint32_t lastDirectMlDeviceIndex() const
    {
        for (std::size_t i = createdProviders.size(); i > 0; --i) {
            if (createdProviders[i - 1] == kDirectMlProvider) {
                return createdDeviceIndexes[i - 1];
            }
        }
        return std::numeric_limits<uint32_t>::max();
    }

private:
    static void* handleFor(int n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(n)); }

    int providerOf(const void* handle) const
    {
        const std::size_t index = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(handle));
        if (index == 0 || index > createdProviders.size()) {
            return -1;
        }
        return createdProviders[index - 1];
    }
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

// Fresh directory under the temp folder, removed when the test ends.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const char* name) : path_(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~ScratchDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Adapter list with a DXCore kind per adapter, as the Windows catalog reports it.
class ScriptedCatalog final : public telaviva::IAdapterCatalog {
public:
    std::vector<telaviva::AdapterInfo> list;
    std::vector<telaviva::AdapterInfo> adapters() override { return list; }
};

const char* const kDriver = "31.0.101.5186";
const char* const kAppVersion = "0.1.0";
const char* const kIntegratedId = "gpu:8086:9a49:0";
const char* const kIntegratedLabel = "Intel Iris Xe Graphics";
const char* const kDiscreteId = "gpu:10de:2684:0";
const char* const kDiscreteLabel = "NVIDIA GeForce RTX 4090";

telaviva::AdapterInfo testAdapter(uint32_t vendorId, uint32_t deviceId, const char* description, uint32_t index,
                                  telaviva::AdapterKind kind)
{
    telaviva::AdapterInfo adapter;
    adapter.vendorId = vendorId;
    adapter.deviceId = deviceId;
    adapter.description = description;
    adapter.driverVersion = kDriver;
    adapter.index = index;
    adapter.kind = kind;
    return adapter;
}

// One integrated GPU at DXGI index 0 and one discrete GPU at index 1.
ScriptedCatalog twoGpuCatalog()
{
    ScriptedCatalog catalog;
    catalog.list.push_back(testAdapter(0x8086, 0x9a49, kIntegratedLabel, 0, telaviva::AdapterKind::Integrated));
    catalog.list.push_back(testAdapter(0x10de, 0x2684, kDiscreteLabel, 1, telaviva::AdapterKind::Discrete));
    return catalog;
}

telaviva::AccelerationOptions acceleratedOptions(telaviva::IAdapterCatalog& catalog,
                                                 telaviva::InferenceFailureStore* failures,
                                                 telaviva::FakeClock& clock)
{
    telaviva::AccelerationOptions options;
    options.adapters = &catalog;
    options.failures = failures;
    options.clock = &clock;
    options.appVersion = kAppVersion;
    options.selfTestFrame = validFrame();
    return options;
}

// CPU reference and DirectML sessions both answer present; each detect takes 10 ms.
void arrangeAccelerated(FakeDetectorApi& api, telaviva::FakeClock& clock)
{
    api.detectStatus = 1;
    api.acceleratedDetectStatus = 1;
    api.clock = &clock;
    api.stepSeconds = 0.01;
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

TEST(inference_host_report_starts_undecided_then_reports_auto_cpu)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    const telaviva::InferenceReport before = host.report();
    CHECK(before.requestedId == "auto");
    CHECK(!before.decided);

    host.detect(validFrame());
    const telaviva::InferenceReport after = host.report();
    CHECK(after.requestedId == "auto");
    CHECK(after.activeId == "cpu");
    CHECK(after.activeLabel == "CPU");
    CHECK(after.state == telaviva::DeviceState::AutoCpu);
    CHECK(!after.reason.has_value());
    CHECK(after.decided);
}

TEST(inference_host_cpu_preference_reports_ok_after_detect)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.setPreference("cpu");
    CHECK(host.report().requestedId == "cpu");
    CHECK(!host.report().decided);

    host.detect(validFrame());
    const telaviva::InferenceReport report = host.report();
    CHECK(report.requestedId == "cpu");
    CHECK(report.activeId == "cpu");
    CHECK(report.state == telaviva::DeviceState::Ok);
    CHECK(report.decided);
}

TEST(inference_host_set_preference_change_releases_session_and_redecides)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    CHECK_EQ(scheduler.pendingCount(), std::size_t(1));
    const int destroysBefore = api.destroys;
    const int createsBefore = api.creates;

    host.setPreference("cpu");
    CHECK_EQ(api.destroys, destroysBefore + 1);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(0));
    CHECK(!host.report().decided);
    CHECK(host.report().requestedId == "cpu");
    CHECK_EQ(host.activeProvider(), -1);

    host.detect(validFrame());
    CHECK_EQ(api.creates, createsBefore + 1);
    CHECK(host.report().decided);
    CHECK(host.report().requestedId == "cpu");
}

TEST(inference_host_set_preference_same_value_is_noop)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    host.detect(validFrame());
    const int destroysBefore = api.destroys;
    const int createsBefore = api.creates;
    const std::vector<void*> handlesBefore = api.detectHandles;

    host.setPreference("auto");  // the default preference
    CHECK_EQ(api.destroys, destroysBefore);
    CHECK_EQ(scheduler.pendingCount(), std::size_t(1));
    CHECK(host.report().decided);

    host.detect(validFrame());
    CHECK_EQ(api.creates, createsBefore);
    CHECK(api.detectHandles.size() == handlesBefore.size() + 1 && api.detectHandles.back() == handlesBefore.back());
}

TEST(inference_host_gpu_preference_switches_between_auto_and_explicit)
{
    telaviva::FakeClock clock;
    FakeDetectorApi api;
    arrangeAccelerated(api, clock);
    ScriptedCatalog catalog = twoGpuCatalog();
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath, acceleratedOptions(catalog, nullptr, clock));

    // Automatic picks the integrated GPU.
    host.detect(validFrame());
    telaviva::InferenceReport report = host.report();
    CHECK(report.requestedId == "auto");
    CHECK(report.activeId == kIntegratedId);
    CHECK(report.activeLabel == kIntegratedLabel);
    CHECK(report.state == telaviva::DeviceState::Ok);
    CHECK(report.decided);
    CHECK_EQ(api.lastDirectMlDeviceIndex(), 0u);

    // An explicit discrete GPU replaces it.
    host.setPreference(kDiscreteId);
    CHECK(!host.report().decided);
    host.detect(validFrame());
    report = host.report();
    CHECK(report.requestedId == kDiscreteId);
    CHECK(report.activeId == kDiscreteId);
    CHECK(report.activeLabel == kDiscreteLabel);
    CHECK(report.state == telaviva::DeviceState::Ok);
    CHECK(report.decided);
    CHECK_EQ(api.lastDirectMlDeviceIndex(), 1u);

    // Back to automatic.
    host.setPreference("auto");
    host.detect(validFrame());
    report = host.report();
    CHECK(report.requestedId == "auto");
    CHECK(report.activeId == kIntegratedId);
    CHECK(report.activeLabel == kIntegratedLabel);
    CHECK(report.state == telaviva::DeviceState::Ok);
    CHECK_EQ(api.lastDirectMlDeviceIndex(), 0u);
}

TEST(inference_host_active_provider_reports_accelerated_session)
{
    telaviva::FakeClock clock;
    FakeDetectorApi api;
    arrangeAccelerated(api, clock);
    ScriptedCatalog catalog = twoGpuCatalog();
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath, acceleratedOptions(catalog, nullptr, clock));
    CHECK_EQ(host.activeProvider(), -1);
    host.detect(validFrame());
    CHECK_EQ(host.activeProvider(), kDirectMlProvider);
    scheduler.advance(30s);
    CHECK_EQ(host.activeProvider(), -1);
}

TEST(inference_host_idle_release_reopens_accelerated_without_self_test)
{
    telaviva::FakeClock clock;
    FakeDetectorApi api;
    arrangeAccelerated(api, clock);
    ScriptedCatalog catalog = twoGpuCatalog();
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath, acceleratedOptions(catalog, nullptr, clock));
    host.detect(validFrame());
    CHECK_EQ(host.activeProvider(), kDirectMlProvider);

    scheduler.advance(30s);
    CHECK_EQ(host.activeProvider(), -1);

    // Only the accelerated session is created again: no CPU reference, and one frame detect.
    const int createsBefore = api.creates;
    const int detectsBefore = api.detectCalls;
    host.detect(validFrame());
    CHECK_EQ(api.creates, createsBefore + 1);
    CHECK_EQ(api.detectCalls, detectsBefore + 1);
    CHECK_EQ(host.activeProvider(), kDirectMlProvider);
    CHECK(host.report().state == telaviva::DeviceState::Ok);
    CHECK(host.report().activeId == kIntegratedId);
}

TEST(inference_host_runtime_error_on_automatic_gpu_retries_on_cpu_with_marker)
{
    ScratchDirectory dir("telaviva-inference-host-auto-runtime");
    telaviva::InferenceFailureStore store(dir.path());
    telaviva::FakeClock clock;
    FakeDetectorApi api;
    arrangeAccelerated(api, clock);
    ScriptedCatalog catalog = twoGpuCatalog();
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath, acceleratedOptions(catalog, &store, clock));
    host.detect(validFrame());
    CHECK_EQ(host.activeProvider(), kDirectMlProvider);

    api.acceleratedDetectStatus = -1;
    // The frame is retried on the CPU session, so the caller gets that result, not an error.
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpuProvider);

    const telaviva::InferenceReport report = host.report();
    CHECK(report.activeId == "cpu");
    CHECK(report.state == telaviva::DeviceState::AutoCpu);
    CHECK(report.reason.has_value() && *report.reason == telaviva::FailureReason::RuntimeError);
    CHECK(report.decided);
    CHECK(store.hasMarker({kIntegratedId, kDriver, kAppVersion}));
}

TEST(inference_host_runtime_error_on_explicit_gpu_fails_without_marker)
{
    ScratchDirectory dir("telaviva-inference-host-explicit-runtime");
    telaviva::InferenceFailureStore store(dir.path());
    telaviva::FakeClock clock;
    FakeDetectorApi api;
    arrangeAccelerated(api, clock);
    ScriptedCatalog catalog = twoGpuCatalog();
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath, acceleratedOptions(catalog, &store, clock));
    host.setPreference(kIntegratedId);
    host.detect(validFrame());
    CHECK_EQ(host.activeProvider(), kDirectMlProvider);

    api.acceleratedDetectStatus = -1;
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpuProvider);

    const telaviva::InferenceReport report = host.report();
    CHECK(report.activeId == "cpu");
    CHECK(report.state == telaviva::DeviceState::Failed);
    CHECK(report.reason.has_value() && *report.reason == telaviva::FailureReason::RuntimeError);
    CHECK(store.records().empty());
}

TEST(inference_host_report_is_readable_between_detects)
{
    FakeDetectorApi api;
    FakeScheduler scheduler;
    InferenceHost host(api, scheduler, kRuntimePath, kModelPath);
    // report() never creates or destroys a session.
    CHECK(!host.report().decided);
    CHECK_EQ(api.creates, 0);
    for (int i = 0; i < 3; ++i) {
        host.detect(validFrame());
        CHECK(host.report().decided);
        CHECK(host.report().activeId == "cpu");
        CHECK_EQ(host.activeProvider(), kCpuProvider);
    }
    CHECK_EQ(api.creates, 1);
    CHECK_EQ(api.destroys, 0);
}
