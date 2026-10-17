#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/AccelerationProfiles.h"
#include "Platform/AdapterCatalog.h"
#include "Platform/FrameAnalyzer.h"
#include "Platform/ICameraBackend.h"
#include "Platform/InferenceFailureStore.h"
#include "Platform/InferenceHost.h"
#include "Platform/Scheduler.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using telaviva::AccelerationOptions;
using telaviva::AccelerationProfile;
using telaviva::AdapterInfo;
using telaviva::AnalysisStatus;
using telaviva::CameraFrame;
using telaviva::FailureReason;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::IAdapterCatalog;
using telaviva::IDetectorApi;
using telaviva::InferenceFailureStore;
using telaviva::InferenceHost;

namespace {

// Values of TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE and TV_INFERENCE_PROVIDER_CPU.
constexpr int kDirectMl = 2;
constexpr int kCpu = 0;

const wchar_t* const kCpuRuntime = L"C:\\TelaViva\\onnxruntime.dll";
const wchar_t* const kDirectMlRuntime = L"C:\\TelaViva\\directml\\onnxruntime.dll";
const wchar_t* const kModelPath = L"C:\\TelaViva\\face_detection_yunet_2023mar.onnx";
const char* const kApp = "0.1.0";
const char* const kDriver = "32.0.21041.1000";

// Scripted seam. A session is accelerated when its selected provider is DirectML,
// so a DirectML create that registers nothing yields a CPU session, as the ABI does.
class ScriptedDetectorApi final : public IDetectorApi {
public:
    explicit ScriptedDetectorApi(FakeClock& clock) : clock_(clock) {}

    // Accelerator script.
    bool directMlCreateFails = false;
    int selectedForDirectMl = kDirectMl;  // 0 models a registration failure
    int directMlStatus = 1;
    double directMlSeconds = 0.005;
    int directMlErrorOnCall = 0;  // 1-based accelerated detect call that returns -2; 0 never
    int runtimeHasDirectMl = 1;

    // CPU script.
    int cpuStatus = 1;
    // 1-based CPU detect call from which every call returns -2; 0 never. The self-test reference is call 1.
    int cpuErrorFromCall = 0;
    double cpuSeconds = 0.010;

    std::vector<int> createProviders;
    std::vector<std::wstring> createRuntimes;
    std::vector<uint32_t> createDeviceIndices;  // parallel to createProviders
    int directMlDetects = 0;
    int cpuDetects = 0;
    int directMlQueries = 0;
    int destroys = 0;

    void* create(const std::wstring& runtimePath, const std::wstring&, int provider, uint32_t deviceIndex) override
    {
        createProviders.push_back(provider);
        createRuntimes.push_back(runtimePath);
        createDeviceIndices.push_back(deviceIndex);
        if (provider == kDirectMl && directMlCreateFails) {
            return nullptr;
        }
        providers_.push_back(provider == kDirectMl ? selectedForDirectMl : kCpu);
        return handleFor(providers_.size());
    }

    int detect(void* handle, const uint8_t*, size_t, int, int, int) override
    {
        if (providerOf(handle) == kDirectMl) {
            ++directMlDetects;
            if (directMlDetects == directMlErrorOnCall) {
                return -2;
            }
            clock_.advance(directMlSeconds);
            return directMlStatus;
        }
        ++cpuDetects;
        clock_.advance(cpuSeconds);
        if (cpuErrorFromCall != 0 && cpuDetects >= cpuErrorFromCall) {
            return -2;
        }
        return cpuStatus;
    }

    int assessLight(const uint8_t*, size_t, int, int, int) override { return 0; }

    int selectedProvider(void* handle) override { return providerOf(handle); }

    int directmlAvailable() override
    {
        ++directMlQueries;
        return runtimeHasDirectMl;
    }

    void destroy(void*) override { ++destroys; }

    std::size_t createsWith(int provider) const
    {
        std::size_t count = 0;
        for (int p : createProviders) {
            if (p == provider) {
                ++count;
            }
        }
        return count;
    }

private:
    static void* handleFor(std::size_t n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(n)); }

    int providerOf(void* handle) const
    {
        const std::size_t n = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(handle));
        if (n == 0 || n > providers_.size()) {
            return kCpu;
        }
        return providers_[n - 1];
    }

    FakeClock& clock_;
    std::vector<int> providers_;
};

class ScriptedCatalog final : public IAdapterCatalog {
public:
    explicit ScriptedCatalog(std::vector<AdapterInfo> adapters) : adapters_(std::move(adapters)) {}

    std::vector<AdapterInfo> adapters() override
    {
        ++queries;
        return adapters_;
    }

    int queries = 0;

private:
    std::vector<AdapterInfo> adapters_;
};

AdapterInfo amdAdapter()
{
    AdapterInfo adapter;
    adapter.vendorId = 0x1002;
    adapter.deviceId = 0x164e;
    adapter.description = "Test Radeon";
    adapter.driverVersion = kDriver;
    return adapter;
}

AdapterInfo nvidiaAdapter()
{
    AdapterInfo adapter;
    adapter.vendorId = 0x10DE;
    adapter.deviceId = 0x2684;
    adapter.description = "Test GeForce";
    adapter.driverVersion = "552.12.0.0";
    return adapter;
}

AdapterInfo softwareAdapter()
{
    AdapterInfo adapter = amdAdapter();
    adapter.description = "Test software renderer";
    adapter.software = true;
    return adapter;
}

AccelerationProfile amdProfile()
{
    AccelerationProfile profile;
    profile.id = "amd-test";
    profile.provider = "directml";
    profile.vendorId = 0x1002;
    profile.deviceIds = {0x164e};
    profile.minDriver = kDriver;
    profile.maxDriver = "";
    profile.measurement = "specs/archived/windows-version/validation/measurements.md#amd-test";
    profile.measuredOn = "2026-10-17";
    return profile;
}

// Unique empty directory per test, removed on destruction.
class TempDir {
public:
    TempDir() : path_(nextPath())
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    static std::filesystem::path nextPath()
    {
        static int counter = 0;
        ++counter;
        return std::filesystem::temp_directory_path() / ("telaviva-provider-selection-" + std::to_string(counter));
    }

    std::filesystem::path path_;
};

CameraFrame validFrame()
{
    CameraFrame frame;
    frame.width = 4;
    frame.height = 4;
    frame.stride = 16;
    frame.bgra.assign(64, 0);
    return frame;
}

// Acceleration on: DirectML runtime, the given profiles, a self-test frame and a fake clock.
AccelerationOptions acceleratedOptions(IAdapterCatalog& catalog, InferenceFailureStore& store, FakeClock& clock,
                                       std::vector<AccelerationProfile> profiles, const char* app = kApp)
{
    AccelerationOptions options;
    options.directmlRuntimePath = kDirectMlRuntime;
    options.profiles = std::move(profiles);
    options.adapters = &catalog;
    options.failures = &store;
    options.clock = &clock;
    options.appVersion = app;
    options.selfTestFrame = validFrame();
    return options;
}

telaviva::FailureKey amdKey(const char* driver = kDriver, const char* app = kApp)
{
    return telaviva::FailureKey{"amd-test", driver, app};
}

}  // namespace

TEST(provider_selection_empty_allowlist_uses_cpu_without_accelerator_calls)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath, acceleratedOptions(catalog, store, clock, {}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(0));
    CHECK_EQ(api.directMlQueries, 0);
    CHECK_EQ(catalog.queries, 0);
    CHECK(store.records().empty());
    CHECK(api.createRuntimes.size() == 1 && api.createRuntimes[0] == kCpuRuntime);
}

TEST(provider_selection_matching_profile_keeps_directml_when_self_test_passes)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    // Accelerated session, then the CPU reference used only by the self-test.
    CHECK(api.createProviders.size() == 2 && api.createProviders[0] == kDirectMl &&
          api.createProviders[1] == kCpu);
    CHECK(api.createRuntimes.size() == 2 && api.createRuntimes[0] == kDirectMlRuntime &&
          api.createRuntimes[1] == kDirectMlRuntime);
    CHECK_EQ(api.directMlDetects, 2);  // self-test call, then the real frame
    CHECK_EQ(api.cpuDetects, 1);
    CHECK(store.records().empty());
    CHECK_EQ(catalog.queries, 1);
}

TEST(provider_selection_register_failure_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.selectedForDirectMl = kCpu;  // the ABI reports CPU when registration fails
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.directMlDetects, 0);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::RegisterFailed);
    CHECK(store.records().size() == 1 && store.records()[0].key == amdKey());
}

TEST(provider_selection_session_failure_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlCreateFails = true;
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SessionFailed);
}

TEST(provider_selection_self_test_mismatch_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlStatus = 0;  // absent on DirectML, present on CPU
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestMismatch);
    // Both self-test sessions are destroyed; the real frame runs on a new CPU session.
    CHECK_EQ(api.destroys, 2);
}

TEST(provider_selection_self_test_error_counts_as_mismatch)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlErrorOnCall = 1;  // the self-test call itself fails
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestMismatch);
}

TEST(provider_selection_self_test_slower_than_cpu_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlSeconds = 0.020;  // CPU reference takes 0.010 s on the same frame
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestSlow);
}

TEST(provider_selection_runtime_error_on_later_detect_switches_to_cpu_for_process)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlErrorOnCall = 3;  // self-test is call 1, the first real frame is call 2
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    // The failing call is retried on CPU, so presence is not reported absent.
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::RuntimeError);
    CHECK(store.records().size() == 1 && store.records()[0].key == amdKey());

    // Later frames stay on CPU for the rest of the process.
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(api.directMlDetects, 3);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(1));
}

TEST(provider_selection_runtime_error_with_failing_cpu_retry_reports_error_not_absent)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlErrorOnCall = 2;  // self-test is call 1, the first real frame is call 2
    api.cpuErrorFromCall = 2;  // the self-test reference (call 1) succeeds; the retry (call 2) fails
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Error);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::RuntimeError);
}

TEST(provider_selection_marker_for_same_key_uses_cpu_without_trying)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(amdKey(), FailureReason::SelfTestSlow);
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(0));
    CHECK_EQ(api.directMlDetects, 0);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestSlow);
}

TEST(provider_selection_marker_for_other_driver_is_cleared_and_acceleration_retried)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(amdKey("31.0.0.1"), FailureReason::RuntimeError);
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(store.records().empty());
}

TEST(provider_selection_marker_for_other_app_version_is_cleared_and_acceleration_retried)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(amdKey(kDriver, "0.0.9"), FailureReason::SelfTestMismatch);
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(store.records().empty());
}

TEST(provider_selection_directml_request_never_changes_cpu_default_without_matching_adapter)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    // The profile's vendor is present only as a software adapter, and the hardware adapter is another vendor.
    ScriptedCatalog catalog({nvidiaAdapter(), softwareAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(0));
    CHECK_EQ(api.directMlDetects, 0);
    CHECK_EQ(api.directMlQueries, 0);
    CHECK(store.records().empty());
    CHECK(api.createRuntimes.size() == 1 && api.createRuntimes[0] == kCpuRuntime);
}

TEST(provider_selection_idle_release_recreates_accelerated_session_without_second_self_test)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({amdAdapter()});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    host.detect(validFrame());
    host.releaseIdle();
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(2));
    CHECK_EQ(api.cpuDetects, 1);  // the reference from the first self-test only
    CHECK(store.records().empty());
}

TEST(provider_selection_directml_session_gets_matched_adapter_index_and_never_index_zero_non_match)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    // The non-matching adapter sits at index 0; the profile's adapter is at index 1 in DXGI order.
    AdapterInfo other = nvidiaAdapter();
    other.index = 0;
    AdapterInfo matched = amdAdapter();
    matched.index = 1;
    ScriptedCatalog catalog({other, matched});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    // Accelerated create carries the matched index; the CPU reference always carries 0.
    CHECK(api.createProviders.size() == 2 && api.createDeviceIndices.size() == 2 &&
          api.createProviders[0] == kDirectMl && api.createDeviceIndices[0] == 1u &&
          api.createProviders[1] == kCpu && api.createDeviceIndices[1] == 0u);
    for (std::size_t i = 0; i < api.createProviders.size(); ++i) {
        CHECK(api.createProviders[i] != kDirectMl || api.createDeviceIndices[i] != 0u);
    }
}

TEST(provider_selection_software_adapter_at_index_zero_does_not_shift_matched_index)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    AdapterInfo software = softwareAdapter();
    software.index = 0;
    AdapterInfo matched = amdAdapter();
    matched.index = 1;
    ScriptedCatalog catalog({software, matched});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kCpuRuntime, kModelPath,
                       acceleratedOptions(catalog, store, clock, {amdProfile()}));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(api.createProviders.size() == 2 && api.createDeviceIndices.size() == 2);
    CHECK(api.createProviders[0] == kDirectMl && api.createDeviceIndices[0] == 1u);
}
