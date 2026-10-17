#pragma once
// Owns the ONNX detector behind IFrameAnalyzer. The detector is created on the
// first detect() and dropped after idleRelease without use, so an idle app holds
// no inference session. Frames are never retained or logged.
//
// Provider selection runs once per process, at the first detect(): a matching
// DirectML profile leads to an accelerated session after a self-test against a CPU
// session; anything else leads to CPU. ONNX Runtime binds one runtime per process,
// so the runtime path is chosen then and never changes.
#include "AccelerationProfiles.h"
#include "AdapterCatalog.h"
#include "Diagnostics.h"
#include "FrameAnalyzer.h"
#include "ICameraBackend.h"
#include "IClock.h"
#include "InferenceFailureStore.h"
#include "ProviderSelection.h"
#include "Scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

// Seam over the C ABI in telaviva_inference.h, so tests run without the runtime.
// Status values follow TV_INFERENCE_*: detect returns 0 absent, 1 present,
// 2 insufficient light, or a negative error; assessLight returns 0, 2 or negative.
// provider is TV_INFERENCE_PROVIDER_CPU or TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE.
struct IDetectorApi {
    virtual ~IDetectorApi() = default;
    // nullptr on failure. deviceIndex is the DXGI adapter index for DirectML and is ignored otherwise.
    virtual void* create(const std::wstring& runtimePath, const std::wstring& modelPath, int provider,
                         uint32_t deviceIndex) = 0;
    virtual int detect(void* handle, const uint8_t* bgra, size_t len, int width, int height, int stride) = 0;
    virtual int assessLight(const uint8_t* bgra, size_t len, int width, int height, int stride) = 0;
    virtual int selectedProvider(void* handle) = 0;
    // 1 when the loaded runtime can register DirectML, 0 otherwise.
    virtual int directmlAvailable() = 0;
    virtual void destroy(void* handle) = 0;
};

// Production adapter over the exported tv_inference_* functions.
class TvInferenceApi : public IDetectorApi {
public:
    void* create(const std::wstring& runtimePath, const std::wstring& modelPath, int provider,
                 uint32_t deviceIndex) override;
    int detect(void* handle, const uint8_t* bgra, size_t len, int width, int height, int stride) override;
    int assessLight(const uint8_t* bgra, size_t len, int width, int height, int stride) override;
    int selectedProvider(void* handle) override;
    int directmlAvailable() override;
    void destroy(void* handle) override;
};

// Everything the DirectML decision needs. The default value means CPU only, so the
// CPU path is unchanged when no profile is configured.
struct AccelerationOptions {
    // Runtime bound only when a profile matches. Empty disables acceleration.
    std::wstring directmlRuntimePath;
    // Parsed acceleration-profiles.toml. Empty means CPU only.
    std::vector<AccelerationProfile> profiles;
    IAdapterCatalog* adapters = nullptr;
    InferenceFailureStore* failures = nullptr;
    IClock* clock = nullptr;
    std::string appVersion;
    // Frame the self-test runs on, on both sessions. Must be a valid frame; an
    // empty frame disables acceleration.
    CameraFrame selfTestFrame;
    // Optional. Receives the active engine line and fallback reasons at Info level.
    Diagnostics* diagnostics = nullptr;
};

class InferenceHost : public IFrameAnalyzer {
public:
    // Paths must be absolute and are converted to UTF-8 for the C ABI.
    // runtimePath is the CPU runtime. The DirectML runtime is in acceleration.
    InferenceHost(IDetectorApi& api, IScheduler& scheduler, std::wstring runtimePath, std::wstring modelPath,
                  AccelerationOptions acceleration = {},
                  std::chrono::milliseconds idleRelease = std::chrono::seconds(30));
    ~InferenceHost() override;

    InferenceHost(const InferenceHost&) = delete;
    InferenceHost& operator=(const InferenceHost&) = delete;

    FrameAnalysis detect(const CameraFrame& frame) override;
    FrameAnalysis assessLight(const CameraFrame& frame) override;
    void releaseIdle() override;

    // TV_INFERENCE_PROVIDER_CPU (0) or TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE (2)
    // for the loaded detector, -1 when no detector exists.
    int activeProvider() const;

private:
    // Undecided until the first detect(); Accelerated and Cpu are final for the process.
    // A runtime error or a failed accelerated session moves Accelerated to Cpu.
    enum class Mode { Undecided, Accelerated, Cpu };

    void onIdle(uint64_t generation);
    // Requires mutex_. Stale timers are cancelled by the caller after unlocking.
    void destroyLocked();
    // Requires mutex_. Makes handle_ usable, deciding the provider on first use.
    // False when no session could be created.
    bool ensureSessionLocked();
    // Requires mutex_. Chooses the engine for the process and sets handle_ when accelerated.
    void decideLocked();
    // Requires mutex_. nullptr on failure, with the reason in failure.
    void* openAcceleratedLocked(FailureReason& failure);
    // Requires mutex_. nullopt when the accelerated session passed.
    std::optional<FailureReason> selfTestLocked(void* accelerated);
    // Requires mutex_. Runs the self-test frame through one session and times the call.
    SelfTestSample timedSelfTestLocked(void* handle);
    // Requires mutex_. Called when an accelerated session errors on a frame: records the
    // marker, moves to CPU for the process and retries the frame there.
    int fallBackToCpuLocked(const CameraFrame& frame);
    // Requires mutex_. Records a failure marker for the active profile key.
    void recordLocked(FailureReason reason);
    // Requires mutex_. Writes one Info line when diagnostics are configured.
    void logLocked(const std::string& text) const;

    IDetectorApi& api_;
    IScheduler& scheduler_;
    std::wstring runtimePath_;
    std::wstring modelPath_;
    std::chrono::milliseconds idleRelease_;
    AccelerationOptions acceleration_;

    mutable std::mutex mutex_;
    void* handle_ = nullptr;
    Mode mode_ = Mode::Undecided;
    // Runtime bound for the process. Starts as the CPU runtime and changes only
    // when an accelerated attempt starts.
    std::wstring boundRuntime_;
    // Set when a profile matches. Key for markers, and the adapter for logs.
    FailureKey activeKey_;
    AdapterInfo activeAdapter_;
    TimerId idleTimer_ = kInvalidTimerId;
    // Bumped on every re-arm and release, so a timer that already fired but
    // waits on mutex_ sees it is stale and does nothing.
    uint64_t armGeneration_ = 0;
};

}  // namespace telaviva
