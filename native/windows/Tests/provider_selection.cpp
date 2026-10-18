#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/AdapterCatalog.h"
#include "Platform/DeviceSelection.h"
#include "Platform/FrameAnalyzer.h"
#include "Platform/ICameraBackend.h"
#include "Platform/InferenceFailureStore.h"
#include "Platform/InferenceHost.h"
#include "Platform/ProviderSelection.h"
#include "Platform/Scheduler.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using telaviva::AccelerationOptions;
using telaviva::AdapterInfo;
using telaviva::AdapterKind;
using telaviva::AnalysisStatus;
using telaviva::CameraFrame;
using telaviva::DeviceState;
using telaviva::FailureKey;
using telaviva::FailureReason;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::IAdapterCatalog;
using telaviva::IDetectorApi;
using telaviva::InferenceFailureStore;
using telaviva::InferenceHost;
using telaviva::InferenceReport;
using telaviva::SelfTestSample;
using telaviva::adapterDeviceId;
using telaviva::judgeSelfTest;
using telaviva::kMaxAcceleratedLatencySeconds;

namespace {

// Values of TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE and TV_INFERENCE_PROVIDER_CPU.
constexpr int kDirectMl = 2;
constexpr int kCpu = 0;

// One runtime path for the whole host: CPU, accelerated and self-test reference sessions alike.
const wchar_t* const kRuntime = L"C:\\TelaViva\\onnxruntime.dll";
const wchar_t* const kModelPath = L"C:\\TelaViva\\face_detection_yunet_2023mar.onnx";
const char* const kApp = "0.1.0";
const char* const kDriver = "32.0.21041.1000";
const char* const kNvidiaDriver = "552.12.0.0";
// Ids as buildDeviceOptions makes them: the integrated AMD adapter and the discrete NVIDIA adapter, both ordinal 0.
const char* const kIntegratedId = "gpu:1002:164e:0";
const char* const kDiscreteId = "gpu:10de:2684:0";

// Scripted seam. A session is accelerated when its selected provider is DirectML,
// so a DirectML create that registers nothing yields a CPU session, as the ABI does.
class ScriptedDetectorApi final : public IDetectorApi {
public:
    explicit ScriptedDetectorApi(FakeClock& clock) : clock_(clock) {}

    // Accelerator script. Create results are scripted per provider and DXGI index.
    bool directMlCreateFails = false;     // every DirectML create returns nullptr
    int directMlFailingIndex = -1;        // the DirectML create for this index returns nullptr; -1 never
    int selectedForDirectMl = kDirectMl;  // 0 models a registration failure
    int directMlStatus = 1;
    double directMlSeconds = 0.005;
    int directMlErrorOnCall = 0;  // 1-based accelerated detect call that returns -2; 0 never

    // CPU script.
    int cpuStatus = 1;
    double cpuSeconds = 0.010;

    std::vector<int> createProviders;
    std::vector<std::wstring> createRuntimes;
    std::vector<uint32_t> createDeviceIndices;  // parallel to createProviders
    int directMlDetects = 0;
    int cpuDetects = 0;
    int destroys = 0;

    void* create(const std::wstring& runtimePath, const std::wstring&, int provider, uint32_t deviceIndex) override
    {
        createProviders.push_back(provider);
        createRuntimes.push_back(runtimePath);
        createDeviceIndices.push_back(deviceIndex);
        const bool indexFails = directMlFailingIndex >= 0 && deviceIndex == static_cast<uint32_t>(directMlFailingIndex);
        if (provider == kDirectMl && (directMlCreateFails || indexFails)) {
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
        return cpuStatus;
    }

    int assessLight(const uint8_t*, size_t, int, int, int) override { return 0; }

    int selectedProvider(void* handle) override { return providerOf(handle); }

    int directmlAvailable() override { return 1; }

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

// Integrated AMD adapter, hardware, with the given DXGI index.
AdapterInfo integratedAdapter(uint32_t index)
{
    AdapterInfo adapter;
    adapter.vendorId = 0x1002;
    adapter.deviceId = 0x164e;
    adapter.description = "Test Radeon";
    adapter.driverVersion = kDriver;
    adapter.index = index;
    adapter.luid = 1;
    adapter.kind = AdapterKind::Integrated;
    return adapter;
}

// Discrete NVIDIA adapter, hardware, with the given DXGI index.
AdapterInfo discreteAdapter(uint32_t index)
{
    AdapterInfo adapter;
    adapter.vendorId = 0x10DE;
    adapter.deviceId = 0x2684;
    adapter.description = "Test GeForce";
    adapter.driverVersion = kNvidiaDriver;
    adapter.index = index;
    adapter.luid = 2;
    adapter.kind = AdapterKind::Discrete;
    return adapter;
}

// Software renderer that reports the integrated adapter's ids. It takes no ordinal and never matches.
AdapterInfo softwareAdapter(uint32_t index)
{
    AdapterInfo adapter = integratedAdapter(index);
    adapter.description = "Test software renderer";
    adapter.software = true;
    adapter.kind = AdapterKind::Unknown;
    return adapter;
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

// Acceleration on: the given catalog and store, the app version, a self-test frame and a fake clock.
AccelerationOptions acceleratedOptions(IAdapterCatalog& catalog, InferenceFailureStore& store, FakeClock& clock,
                                       const char* app = kApp)
{
    AccelerationOptions options;
    options.adapters = &catalog;
    options.failures = &store;
    options.clock = &clock;
    options.appVersion = app;
    options.selfTestFrame = validFrame();
    return options;
}

// Marker key for the integrated adapter, as the host writes it.
FailureKey integratedKey(const char* driver = kDriver, const char* app = kApp)
{
    return FailureKey{kIntegratedId, driver, app};
}

// True when every create() call received the one runtime path the host was constructed with.
bool everyCreateUsesRuntime(const ScriptedDetectorApi& api, const wchar_t* runtime)
{
    for (const std::wstring& path : api.createRuntimes) {
        if (path != runtime) {
            return false;
        }
    }
    return !api.createRuntimes.empty();
}

}  // namespace

TEST(provider_selection_accelerated_within_limit_passes_even_when_slower_than_cpu)
{
    // Speed relative to the CPU is not part of validation: exactly the limit passes against 0.010 s.
    // The limit is inclusive.
    CHECK(!judgeSelfTest(SelfTestSample{1, kMaxAcceleratedLatencySeconds}, SelfTestSample{1, 0.010}).has_value());
    // A sample below the limit passes as well.
    CHECK(!judgeSelfTest(SelfTestSample{1, kMaxAcceleratedLatencySeconds - 0.001}, SelfTestSample{1, 0.010}).has_value());
}

TEST(provider_selection_accelerated_over_limit_is_self_test_slow)
{
    CHECK(judgeSelfTest(SelfTestSample{1, kMaxAcceleratedLatencySeconds + 0.001}, SelfTestSample{1, 0.010}) ==
          FailureReason::SelfTestSlow);
}

TEST(provider_selection_accelerated_not_present_is_self_test_mismatch)
{
    CHECK(judgeSelfTest(SelfTestSample{0, 0.005}, SelfTestSample{1, 0.010}) == FailureReason::SelfTestMismatch);
    CHECK(judgeSelfTest(SelfTestSample{2, 0.005}, SelfTestSample{1, 0.010}) == FailureReason::SelfTestMismatch);
}

TEST(provider_selection_cpu_not_present_is_self_test_mismatch)
{
    CHECK(judgeSelfTest(SelfTestSample{1, 0.005}, SelfTestSample{0, 0.010}) == FailureReason::SelfTestMismatch);
}

TEST(provider_selection_negative_status_is_self_test_mismatch)
{
    CHECK(judgeSelfTest(SelfTestSample{-2, 0.005}, SelfTestSample{1, 0.010}) == FailureReason::SelfTestMismatch);
    CHECK(judgeSelfTest(SelfTestSample{1, 0.005}, SelfTestSample{-2, 0.010}) == FailureReason::SelfTestMismatch);
}

TEST(provider_selection_auto_uses_integrated_adapter_index_and_ends_accelerated)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    // The discrete adapter is first in DXGI order, so the integrated adapter is at index 1.
    const std::vector<AdapterInfo> adapters = {discreteAdapter(0), integratedAdapter(1)};
    ScriptedCatalog catalog(adapters);
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(!host.report().decided);
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    // Accelerated create carries the integrated index; the CPU reference follows it.
    CHECK(api.createProviders.size() == 2 && api.createProviders[0] == kDirectMl &&
          api.createDeviceIndices[0] == 1u && api.createProviders[1] == kCpu);
    CHECK_EQ(api.directMlDetects, 2);  // self-test call, then the real frame
    CHECK_EQ(api.cpuDetects, 1);
    CHECK(everyCreateUsesRuntime(api, kRuntime));
    const InferenceReport report = host.report();
    CHECK(report.decided);
    CHECK(report.state == DeviceState::Ok);
    CHECK(!report.reason.has_value());
    CHECK(report.requestedId == "auto");
    CHECK(report.activeId == adapterDeviceId(adapters, 1));
    CHECK(report.activeId == kIntegratedId);
    CHECK(report.activeLabel == adapters[1].description);
    CHECK(store.records().empty());
}

TEST(provider_selection_auto_with_only_discrete_adapter_never_creates_directml_session)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({discreteAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(0));
    CHECK_EQ(api.directMlDetects, 0);
    CHECK(everyCreateUsesRuntime(api, kRuntime));
    const InferenceReport report = host.report();
    CHECK(report.state == DeviceState::AutoCpu);
    CHECK(report.activeId == "cpu");
    CHECK(!report.reason.has_value());
    CHECK(store.records().empty());
}

TEST(provider_selection_software_adapter_at_index_zero_does_not_shift_integrated_index)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    // The software adapter takes DXGI index 0 but no ordinal, so the integrated adapter keeps id ordinal 0.
    ScriptedCatalog catalog({softwareAdapter(0), integratedAdapter(1)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(api.createProviders.size() == 2 && api.createProviders[0] == kDirectMl &&
          api.createDeviceIndices[0] == 1u);
    CHECK(host.report().activeId == kIntegratedId);
}

TEST(provider_selection_explicit_discrete_adapter_ignores_its_marker_and_uses_own_index)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    const std::vector<AdapterInfo> adapters = {discreteAdapter(0), integratedAdapter(1)};
    ScriptedCatalog catalog(adapters);
    TempDir dir;
    InferenceFailureStore store(dir.path());
    // Same driver as the live adapter, so clearIfChanged keeps it; an explicit choice must still ignore it.
    store.record(FailureKey{kDiscreteId, kNvidiaDriver, kApp}, FailureReason::RuntimeError);
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));
    host.setPreference(kDiscreteId);

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(api.createProviders.size() == 2 && api.createProviders[0] == kDirectMl &&
          api.createDeviceIndices[0] == 0u);
    const InferenceReport report = host.report();
    CHECK(report.state == DeviceState::Ok);
    CHECK(report.activeId == kDiscreteId);
    CHECK(report.activeLabel == adapters[0].description);
    CHECK(!report.reason.has_value());
    CHECK(store.records().size() == 1);
}

TEST(provider_selection_auto_register_failure_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.selectedForDirectMl = kCpu;  // the ABI reports CPU when registration fails
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.directMlDetects, 0);
    CHECK_EQ(api.createsWith(kCpu), std::size_t(1));
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::RegisterFailed);
    CHECK(store.records().size() == 1 && store.records()[0].key == integratedKey());
    const InferenceReport report = host.report();
    CHECK(report.state == DeviceState::AutoCpu);
    CHECK(report.reason.has_value() && *report.reason == FailureReason::RegisterFailed);
}

TEST(provider_selection_explicit_register_failure_fails_without_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.selectedForDirectMl = kCpu;
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));
    host.setPreference(kIntegratedId);

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.directMlDetects, 0);
    CHECK(store.records().empty());
    const InferenceReport report = host.report();
    CHECK(report.state == DeviceState::Failed);
    CHECK(report.reason.has_value() && *report.reason == FailureReason::RegisterFailed);
    CHECK(report.activeId == "cpu");
}

TEST(provider_selection_auto_self_test_mismatch_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlStatus = 0;  // absent on DirectML, present on CPU
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestMismatch);
    CHECK(store.records().size() == 1 && store.records()[0].key == integratedKey());
    // Both self-test sessions are destroyed; the real frame runs on a new CPU session.
    CHECK_EQ(api.destroys, 2);
    CHECK(host.report().state == DeviceState::AutoCpu);
}

TEST(provider_selection_auto_self_test_slow_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlSeconds = kMaxAcceleratedLatencySeconds + 0.05;  // over the limit, whatever the CPU takes
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestSlow);
    CHECK(store.records().size() == 1 && store.records()[0].key == integratedKey());
    CHECK(host.report().state == DeviceState::AutoCpu);
}

TEST(provider_selection_explicit_self_test_mismatch_fails_without_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlStatus = 0;
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));
    host.setPreference(kIntegratedId);

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().empty());
    const InferenceReport report = host.report();
    CHECK(report.state == DeviceState::Failed);
    CHECK(report.reason.has_value() && *report.reason == FailureReason::SelfTestMismatch);
}

TEST(provider_selection_explicit_self_test_slow_fails_without_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlSeconds = kMaxAcceleratedLatencySeconds + 0.05;
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));
    host.setPreference(kIntegratedId);

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().empty());
    const InferenceReport report = host.report();
    CHECK(report.state == DeviceState::Failed);
    CHECK(report.reason.has_value() && *report.reason == FailureReason::SelfTestSlow);
}

TEST(provider_selection_second_host_skips_integrated_adapter_with_marker)
{
    FakeClock clock;
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    {
        // The first host fails the self-test on the integrated adapter and writes the marker.
        ScriptedDetectorApi first(clock);
        first.directMlStatus = 0;
        InferenceHost host(first, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));
        CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    }
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SelfTestMismatch);

    // A second host over the same store does not try the marked adapter again.
    ScriptedDetectorApi second(clock);
    InferenceHost host(second, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(second.createsWith(kDirectMl), std::size_t(0));
    CHECK_EQ(second.directMlDetects, 0);
    CHECK(everyCreateUsesRuntime(second, kRuntime));
    CHECK(host.report().state == DeviceState::AutoCpu);
    CHECK(store.records().size() == 1);
}

TEST(provider_selection_marker_for_other_driver_is_cleared_and_adapter_retried)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(integratedKey("31.0.0.1"), FailureReason::RuntimeError);
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(host.report().state == DeviceState::Ok);
    CHECK(store.records().empty());
}

TEST(provider_selection_marker_for_other_app_version_is_cleared_and_adapter_retried)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(integratedKey(kDriver, "0.0.9"), FailureReason::SelfTestMismatch);
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK(host.report().state == DeviceState::Ok);
    CHECK(store.records().empty());
}

TEST(provider_selection_auto_session_failure_falls_back_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlFailingIndex = 0;  // the DirectML create for the integrated adapter fails
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(1));
    CHECK_EQ(api.directMlDetects, 0);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::SessionFailed);
    CHECK(store.records().size() == 1 && store.records()[0].key == integratedKey());
    CHECK(host.report().state == DeviceState::AutoCpu);
}

TEST(provider_selection_auto_runtime_error_switches_to_cpu_and_records_marker)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    api.directMlErrorOnCall = 3;  // self-test is call 1, the first real frame is call 2
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    // The failing call is retried on CPU, so presence is not reported absent.
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kCpu);
    CHECK(store.records().size() == 1 && store.records()[0].reason == FailureReason::RuntimeError);
    CHECK(store.records().size() == 1 && store.records()[0].key == integratedKey());
    CHECK(host.report().state == DeviceState::AutoCpu);
    CHECK(host.report().reason.has_value() && *host.report().reason == FailureReason::RuntimeError);

    // Later frames stay on CPU for the rest of the session.
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(api.directMlDetects, 3);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(1));
}

TEST(provider_selection_idle_release_recreates_accelerated_session_without_second_self_test)
{
    FakeClock clock;
    ScriptedDetectorApi api(clock);
    FakeScheduler scheduler;
    ScriptedCatalog catalog({integratedAdapter(0)});
    TempDir dir;
    InferenceFailureStore store(dir.path());
    InferenceHost host(api, scheduler, kRuntime, kModelPath, acceleratedOptions(catalog, store, clock));

    host.detect(validFrame());
    host.releaseIdle();
    CHECK(host.detect(validFrame()).status == AnalysisStatus::Present);
    CHECK_EQ(host.activeProvider(), kDirectMl);
    CHECK_EQ(api.createsWith(kDirectMl), std::size_t(2));
    CHECK_EQ(api.cpuDetects, 1);  // the reference from the first self-test only
    CHECK(everyCreateUsesRuntime(api, kRuntime));
    CHECK(host.report().state == DeviceState::Ok);
    CHECK(store.records().empty());
}
