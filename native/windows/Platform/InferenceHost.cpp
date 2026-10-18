#include "InferenceHost.h"

#include <telaviva_inference.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace telaviva {

namespace {

constexpr int kProviderCpu = TV_INFERENCE_PROVIDER_CPU;
constexpr int kProviderDirectMl = TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE;
// The ABI ignores the device index for CPU; 0 is the documented placeholder.
constexpr uint32_t kCpuDeviceIndex = 0;

// The ABI takes UTF-8 paths; a failed conversion is treated as a failed create.
bool toUtf8(const std::wstring& text, std::string& out)
{
    out.clear();
    if (text.empty()) {
        return true;
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int wideLength = static_cast<int>(text.size());
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(needed));
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), wideLength, out.data(), needed, nullptr, nullptr);
    return written == needed;
}

// Rejects frames the ABI would read out of bounds. Checked before any API call,
// so a bad frame never creates or touches the detector.
bool hasValidLayout(const CameraFrame& frame)
{
    if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0) {
        return false;
    }
    if (frame.stride < static_cast<int64_t>(frame.width) * 4) {
        return false;
    }
    const uint64_t needed = static_cast<uint64_t>(frame.stride) * static_cast<uint64_t>(frame.height);
    return static_cast<uint64_t>(frame.bgra.size()) >= needed;
}

std::size_t frameLength(const CameraFrame& frame)
{
    return static_cast<std::size_t>(static_cast<uint64_t>(frame.stride) * static_cast<uint64_t>(frame.height));
}

// "0x1002" for a PCI id, for the engine line.
std::string hexId(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%04x", static_cast<unsigned>(value));
    return text;
}

// Driver of the catalog adapter with the given device id, empty when none has it.
std::string driverOf(const std::vector<AdapterInfo>& adapters, const std::string& id)
{
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (adapterDeviceId(adapters, i) == id) {
            return adapters[i].driverVersion;
        }
    }
    return {};
}

AnalysisStatus mapDetect(int code)
{
    switch (code) {
    case 0:
        return AnalysisStatus::Absent;
    case 1:
        return AnalysisStatus::Present;
    case 2:
        return AnalysisStatus::InsufficientLight;
    default:
        // Negative errors, and any status the detect call never returns.
        return AnalysisStatus::Error;
    }
}

AnalysisStatus mapLight(int code)
{
    switch (code) {
    case 0:
    case 1:
        return AnalysisStatus::UsableLight;
    case 2:
        return AnalysisStatus::InsufficientLight;
    default:
        return AnalysisStatus::Error;
    }
}

}  // namespace

void* TvInferenceApi::create(const std::wstring& runtimePath, const std::wstring& modelPath, int provider,
                             uint32_t deviceIndex)
{
    std::string runtime;
    std::string model;
    if (!toUtf8(runtimePath, runtime) || !toUtf8(modelPath, model)) {
        return nullptr;
    }
    TvInferenceHandle* handle = nullptr;
    const int32_t status = tv_inference_create_with_provider_device(runtime.c_str(), model.c_str(),
                                                                    static_cast<int32_t>(provider), deviceIndex,
                                                                    &handle);
    return status == 0 ? static_cast<void*>(handle) : nullptr;
}

int TvInferenceApi::detect(void* handle, const uint8_t* bgra, size_t len, int width, int height, int stride)
{
    TvInferenceResult result{};
    const int32_t code =
        tv_inference_detect_bgra(static_cast<TvInferenceHandle*>(handle), bgra, len, static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height), static_cast<size_t>(stride), &result);
    // Negative codes are errors; otherwise the status is in result, as the macOS shell reads it.
    if (code < 0) {
        return code;
    }
    return result.status;
}

int TvInferenceApi::assessLight(const uint8_t* bgra, size_t len, int width, int height, int stride)
{
    return tv_inference_assess_light_bgra(bgra, len, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                          static_cast<size_t>(stride));
}

int TvInferenceApi::selectedProvider(void* handle)
{
    return tv_inference_selected_provider(static_cast<const TvInferenceHandle*>(handle));
}

int TvInferenceApi::directmlAvailable()
{
    // Only 1 means available; 0 and any error mean DirectML cannot be registered.
    return tv_inference_directml_available() == 1 ? 1 : 0;
}

void TvInferenceApi::destroy(void* handle)
{
    tv_inference_destroy(static_cast<TvInferenceHandle*>(handle));
}

InferenceHost::InferenceHost(IDetectorApi& api, IScheduler& scheduler, std::wstring runtimePath,
                             std::wstring modelPath, AccelerationOptions acceleration,
                             std::chrono::milliseconds idleRelease)
    : api_(api),
      scheduler_(scheduler),
      runtimePath_(std::move(runtimePath)),
      modelPath_(std::move(modelPath)),
      idleRelease_(idleRelease),
      acceleration_(std::move(acceleration))
{
}

InferenceHost::~InferenceHost()
{
    releaseIdle();
}

FrameAnalysis InferenceHost::detect(const CameraFrame& frame)
{
    FrameAnalysis analysis;
    if (!hasValidLayout(frame)) {
        return analysis;
    }

    TimerId stale = kInvalidTimerId;
    int code = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureSessionLocked()) {
            return analysis;
        }
        code = api_.detect(handle_, frame.bgra.data(), frameLength(frame), frame.width, frame.height, frame.stride);
        if (code < 0 && mode_ == Mode::Accelerated) {
            code = fallBackToCpuLocked(frame);
        }

        // Re-arm after every use, so the detector lives idleRelease after the last call.
        stale = idleTimer_;
        const uint64_t generation = ++armGeneration_;
        idleTimer_ = scheduler_.scheduleOnce(idleRelease_, toleranceFor(idleRelease_),
                                             [this, generation] { onIdle(generation); });
    }

    // Cancelled outside the lock: a timer callback may be waiting on mutex_,
    // and cancel() waits for running callbacks.
    if (stale != kInvalidTimerId) {
        scheduler_.cancel(stale);
    }
    analysis.status = mapDetect(code);
    return analysis;
}

FrameAnalysis InferenceHost::assessLight(const CameraFrame& frame)
{
    // Light assessment needs no session, so it neither creates nor re-arms the detector.
    FrameAnalysis analysis;
    if (!hasValidLayout(frame)) {
        return analysis;
    }
    const int code = api_.assessLight(frame.bgra.data(), frameLength(frame), frame.width, frame.height, frame.stride);
    analysis.status = mapLight(code);
    return analysis;
}

void InferenceHost::releaseIdle()
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stale = idleTimer_;
        idleTimer_ = kInvalidTimerId;
        ++armGeneration_;
        destroyLocked();
    }
    if (stale != kInvalidTimerId) {
        scheduler_.cancel(stale);
    }
}

void InferenceHost::setPreference(const std::string& id)
{
    TimerId stale = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == preference_) {
            return;
        }
        preference_ = id;
        stale = idleTimer_;
        idleTimer_ = kInvalidTimerId;
        ++armGeneration_;
        destroyLocked();
        mode_ = Mode::Undecided;
        report_ = InferenceReport{};
        report_.requestedId = id;
        report_.decided = false;
    }
    if (stale != kInvalidTimerId) {
        scheduler_.cancel(stale);
    }
}

InferenceReport InferenceHost::report() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

int InferenceHost::activeProvider() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ == nullptr) {
        return -1;
    }
    return api_.selectedProvider(handle_);
}

void InferenceHost::onIdle(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != armGeneration_) {
        return;
    }
    idleTimer_ = kInvalidTimerId;
    destroyLocked();
}

void InferenceHost::destroyLocked()
{
    if (handle_ == nullptr) {
        return;
    }
    void* handle = handle_;
    handle_ = nullptr;
    api_.destroy(handle);
}

bool InferenceHost::ensureSessionLocked()
{
    if (handle_ != nullptr) {
        return true;
    }
    if (mode_ == Mode::Undecided) {
        decideLocked();
    } else if (mode_ == Mode::Accelerated) {
        // Reopening after an idle release reuses the decision and skips the self-test.
        FailureReason failure = FailureReason::SessionFailed;
        handle_ = openAcceleratedLocked(failure);
        if (handle_ == nullptr) {
            rejectAcceleratedLocked(failure, " on reopen");
        }
    }
    if (handle_ != nullptr) {
        return true;
    }
    handle_ = api_.create(runtimePath_, modelPath_, kProviderCpu, kCpuDeviceIndex);
    return handle_ != nullptr;
}

void InferenceHost::decideLocked()
{
    // The catalog is read once per decision. Without a catalog there is no GPU candidate.
    const std::vector<AdapterInfo> adapters =
        acceleration_.adapters != nullptr ? acceleration_.adapters->adapters() : std::vector<AdapterInfo>{};
    const auto hasMarker = [this, &adapters](const std::string& id) {
        if (acceleration_.failures == nullptr) {
            return false;
        }
        const std::string driver = driverOf(adapters, id);
        // Drops markers written for another driver or app version, so a change makes the adapter eligible again.
        acceleration_.failures->clearIfChanged(driver, acceleration_.appVersion);
        return acceleration_.failures->hasMarker(FailureKey{id, driver, acceleration_.appVersion});
    };
    choice_ = resolveDevice(preference_, adapters, hasMarker);

    if (!choice_.useGpu) {
        reportCpuLocked(choice_.state, std::nullopt);
        logLocked("inference engine: cpu (preference " + preference_ + ")");
        return;
    }
    if (acceleration_.clock == nullptr || !hasValidLayout(acceleration_.selfTestFrame)) {
        // A missing clock or an invalid self-test frame means no GPU: CPU, reported as an automatic fallback.
        reportCpuLocked(DeviceState::AutoCpu, std::nullopt);
        logLocked("inference engine: cpu (preference " + preference_ +
                  ", clock or self-test frame is not configured)");
        return;
    }

    activeKey_ = FailureKey{choice_.id, choice_.adapter.driverVersion, acceleration_.appVersion};
    if (acceleration_.failures != nullptr) {
        // Drops markers written for another driver or app version. Markers are optional.
        acceleration_.failures->clearIfChanged(choice_.adapter.driverVersion, acceleration_.appVersion);
    }

    FailureReason failure = FailureReason::SessionFailed;
    void* accelerated = openAcceleratedLocked(failure);
    if (accelerated == nullptr) {
        rejectAcceleratedLocked(failure, "");
        return;
    }

    const std::optional<FailureReason> verdict = selfTestLocked(accelerated);
    if (verdict.has_value()) {
        api_.destroy(accelerated);
        rejectAcceleratedLocked(*verdict, "");
        return;
    }

    handle_ = accelerated;
    mode_ = Mode::Accelerated;
    report_.requestedId = preference_;
    report_.activeId = choice_.id;
    report_.activeLabel = choice_.adapter.description;
    report_.state = choice_.state;
    report_.reason = std::nullopt;
    report_.decided = true;
    logLocked("inference engine: directml (adapter vendor " + hexId(choice_.adapter.vendorId) + " device " +
              hexId(choice_.adapter.deviceId) + ", driver " + choice_.adapter.driverVersion + ")");
}

void InferenceHost::reportCpuLocked(DeviceState state, std::optional<FailureReason> reason)
{
    mode_ = Mode::Cpu;
    report_.requestedId = preference_;
    report_.activeId = "cpu";
    report_.activeLabel = "CPU";
    report_.state = state;
    report_.reason = reason;
    report_.decided = true;
}

void InferenceHost::rejectAcceleratedLocked(FailureReason reason, const char* when)
{
    recordLocked(reason);
    if (choice_.viaAuto) {
        const DeviceState state = choice_.state == DeviceState::Unavailable ? DeviceState::Unavailable
                                                                            : DeviceState::AutoCpu;
        reportCpuLocked(state, reason);
    } else {
        reportCpuLocked(DeviceState::Failed, reason);
    }
    logLocked("inference engine: cpu (preference " + preference_ + ", DirectML " +
              std::string(failureReasonName(reason)) + when + (choice_.viaAuto ? "; marker recorded" : "") + ")");
}

void* InferenceHost::openAcceleratedLocked(FailureReason& failure)
{
    // The index comes from the DXGI match in decideLocked, never from a fixed value.
    void* accelerated = api_.create(runtimePath_, modelPath_, kProviderDirectMl, choice_.adapter.index);
    if (accelerated == nullptr) {
        failure = FailureReason::SessionFailed;
        return nullptr;
    }
    if (api_.selectedProvider(accelerated) != kProviderDirectMl) {
        // The ABI returns a CPU session when registration fails, so the handle is not usable.
        api_.destroy(accelerated);
        failure = FailureReason::RegisterFailed;
        logLocked("inference engine: DirectML registration returned cpu (runtime directml available " +
                  std::to_string(api_.directmlAvailable()) + ")");
        return nullptr;
    }
    return accelerated;
}

std::optional<FailureReason> InferenceHost::selfTestLocked(void* accelerated)
{
    const SelfTestSample acceleratedSample = timedSelfTestLocked(accelerated);
    void* reference = api_.create(runtimePath_, modelPath_, kProviderCpu, kCpuDeviceIndex);
    if (reference == nullptr) {
        // A failed reference session is an error, and an error counts as a failed self-test.
        return FailureReason::SelfTestMismatch;
    }
    const SelfTestSample cpuSample = timedSelfTestLocked(reference);
    api_.destroy(reference);
    return judgeSelfTest(acceleratedSample, cpuSample);
}

SelfTestSample InferenceHost::timedSelfTestLocked(void* handle)
{
    const CameraFrame& fixture = acceleration_.selfTestFrame;
    SelfTestSample sample;
    const double started = acceleration_.clock->steadySeconds();
    sample.status = api_.detect(handle, fixture.bgra.data(), frameLength(fixture), fixture.width, fixture.height,
                                fixture.stride);
    sample.seconds = acceleration_.clock->steadySeconds() - started;
    return sample;
}

int InferenceHost::fallBackToCpuLocked(const CameraFrame& frame)
{
    recordLocked(FailureReason::RuntimeError);
    destroyLocked();
    reportCpuLocked(choice_.viaAuto ? DeviceState::AutoCpu : DeviceState::Failed, FailureReason::RuntimeError);
    logLocked("inference engine: cpu (preference " + preference_ + ", DirectML runtime error" +
              (choice_.viaAuto ? "; marker recorded" : "") + ")");
    // Retry the failing frame on CPU, so the caller never sees a false absence.
    handle_ = api_.create(runtimePath_, modelPath_, kProviderCpu, kCpuDeviceIndex);
    if (handle_ == nullptr) {
        return TV_INFERENCE_ERROR_RUNTIME;
    }
    return api_.detect(handle_, frame.bgra.data(), frameLength(frame), frame.width, frame.height, frame.stride);
}

void InferenceHost::recordLocked(FailureReason reason)
{
    // Markers are written only for an automatic candidate; an explicit choice is never blocked by one.
    if (choice_.viaAuto && acceleration_.failures != nullptr) {
        acceleration_.failures->record(activeKey_, reason);
    }
}

void InferenceHost::logLocked(const std::string& text) const
{
    if (acceleration_.diagnostics != nullptr) {
        acceleration_.diagnostics->log(LogLevel::Info, text);
    }
}

}  // namespace telaviva
