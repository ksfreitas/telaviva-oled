#pragma once
// Owns the ONNX detector behind IFrameAnalyzer. The detector is created on the
// first detect() and dropped after idleRelease without use, so an idle app holds
// no inference session. Frames are never retained or logged.
//
// The device is chosen per preference (the stored inference_device setting), not
// per process: setPreference() releases the session and the next detect() decides
// again. Automatic picks the first integrated GPU without a failure marker after a
// self-test against a CPU session; any other choice runs on CPU. ONNX Runtime binds
// one runtime per process, so every session uses the single runtime path given to
// the constructor.
#include "AdapterCatalog.h"
#include "DeviceSelection.h"
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

// Everything the device decision needs. The default value means CPU only, so the
// CPU path is unchanged when no catalog is configured.
struct AccelerationOptions {
    // Live adapter list. Null means no GPU candidate exists.
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
    // runtimePath is the one runtime for every session (CPU, DirectML and self-test reference).
    InferenceHost(IDetectorApi& api, IScheduler& scheduler, std::wstring runtimePath, std::wstring modelPath,
                  AccelerationOptions acceleration = {},
                  std::chrono::milliseconds idleRelease = std::chrono::seconds(30));
    ~InferenceHost() override;

    InferenceHost(const InferenceHost&) = delete;
    InferenceHost& operator=(const InferenceHost&) = delete;

    FrameAnalysis detect(const CameraFrame& frame) override;
    FrameAnalysis assessLight(const CameraFrame& frame) override;
    void releaseIdle() override;

    // Sets the requested device id ("auto", "cpu" or a gpu id). A change releases the
    // session and the next detect() decides again. The same value is a no-op.
    void setPreference(const std::string& id);

    // What the Advanced page reads. Does not create or destroy a session.
    InferenceReport report() const;

    // TV_INFERENCE_PROVIDER_CPU (0) or TV_INFERENCE_PROVIDER_DIRECTML_CANDIDATE (2)
    // for the loaded detector, -1 when no detector exists.
    int activeProvider() const;

private:
    // Undecided until a session is needed for the current preference. Accelerated and
    // Cpu are final only for that preference. A runtime error or a rejected accelerated
    // session moves Accelerated to Cpu.
    enum class Mode { Undecided, Accelerated, Cpu };

    void onIdle(uint64_t generation);
    // Requires mutex_. Stale timers are cancelled by the caller after unlocking.
    void destroyLocked();
    // Requires mutex_. Makes handle_ usable, deciding the device on first use.
    // False when no session could be created.
    bool ensureSessionLocked();
    // Requires mutex_. Chooses the device for preference_ and sets handle_ when accelerated.
    void decideLocked();
    // Requires mutex_. Sets the CPU mode and the report for a CPU session.
    void reportCpuLocked(DeviceState state, std::optional<FailureReason> reason);
    // Requires mutex_. The accelerated session was not usable (already destroyed by the caller):
    // records a marker when the candidate was automatic and reports the CPU fallback.
    // `when` is appended to the log line.
    void rejectAcceleratedLocked(FailureReason reason, const char* when);
    // Requires mutex_. nullptr on failure, with the reason in failure.
    void* openAcceleratedLocked(FailureReason& failure);
    // Requires mutex_. nullopt when the accelerated session passed.
    std::optional<FailureReason> selfTestLocked(void* accelerated);
    // Requires mutex_. Runs the self-test frame through one session and times the call.
    SelfTestSample timedSelfTestLocked(void* handle);
    // Requires mutex_. Called when an accelerated session errors on a frame: records the
    // marker (automatic only), moves to CPU for the process and retries the frame there.
    int fallBackToCpuLocked(const CameraFrame& frame);
    // Requires mutex_. Records a failure marker for activeKey_, only for an automatic candidate.
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
    // The stored setting value. Changed only by setPreference.
    std::string preference_ = "auto";
    // Device chosen for the current preference. Its adapter and viaAuto drive logs and markers.
    DeviceChoice choice_;
    // Marker key of the chosen GPU candidate, set before the GPU is tried.
    FailureKey activeKey_;
    InferenceReport report_;
    TimerId idleTimer_ = kInvalidTimerId;
    // Bumped on every re-arm and release, so a timer that already fired but
    // waits on mutex_ sees it is stale and does nothing.
    uint64_t armGeneration_ = 0;
};

}  // namespace telaviva
