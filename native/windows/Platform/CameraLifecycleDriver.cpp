#include "CameraLifecycleDriver.h"

#include "CameraSelection.h"
#include "CameraStatus.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

namespace {

// Deadlines from the macOS shell: 5 s for the first frame and for the post-settle frame,
// a 5 s exposure settle, and 30 s before an unused detector is released.
constexpr std::chrono::milliseconds kWatchdog{5000};
constexpr std::chrono::milliseconds kSettle{5000};
constexpr std::chrono::milliseconds kDetectorIdle{30000};
// Retained capture rate from contracts.md. Single-frame sessions pass 0 and keep the device rate.
constexpr double kRetainedFps = 2.0;

// Work decided under the mutex and carried out after it is released. Cancel, close and open
// can re-enter the driver (a failed open reports through onFailure), so they never run locked.
struct Effects {
    std::vector<TimerId> cancel;
    bool close = false;
    bool releaseDetector = false;
    std::optional<std::string> openId;
    double openFps = 0.0;
};

void retire(TimerId& id, Effects& fx)
{
    if (id != kInvalidTimerId) {
        fx.cancel.push_back(id);
        id = kInvalidTimerId;
    }
}

std::string numberText(double value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

// Raw text of one member of a core reply: the unquoted text of a string, or the literal token
// for true, false, null and numbers. Empty when the key is absent. Core replies are compact
// and fixed, so a plain search is enough.
std::string rawField(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\":";
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) {
        return std::string();
    }
    const std::size_t start = at + needle.size();
    if (start < text.size() && text[start] == '"') {
        const std::size_t end = text.find('"', start + 1);
        return end == std::string::npos ? std::string() : text.substr(start + 1, end - start - 1);
    }
    const std::size_t end = text.find_first_of(",}", start);
    return end == std::string::npos ? std::string() : text.substr(start, end - start);
}

uint64_t uintField(const std::string& text, const std::string& key)
{
    return std::strtoull(rawField(text, key).c_str(), nullptr, 10);
}

double numberField(const std::string& text, const std::string& key)
{
    return std::strtod(rawField(text, key).c_str(), nullptr);
}

std::chrono::milliseconds millisFrom(double ms)
{
    return std::chrono::milliseconds(static_cast<long long>(std::llround(std::max(0.0, ms))));
}

std::string plainEvent(const char* type)
{
    return std::string("{\"type\":\"") + type + "\"}";
}

std::string timedEvent(const char* type, double now)
{
    return std::string("{\"type\":\"") + type + "\",\"now\":" + numberText(now) + "}";
}

std::string tokenEvent(uint64_t token)
{
    return "{\"type\":\"is_current_assessment\",\"token\":" + std::to_string(token) + "}";
}

std::string generationEvent(uint64_t generation)
{
    return "{\"type\":\"accepts_callback\",\"generation\":" + std::to_string(generation) + "}";
}

std::string reconcileEvent(const std::string& mode, const std::string& captureMode, uint64_t generation,
                           const std::string& cameraIds, bool alwaysOn)
{
    const std::string cameraField = cameraIds.empty() ? std::string("null") : jsonQuote(cameraIds);
    return "{\"type\":\"reconcile\",\"mode\":" + jsonQuote(mode) + ",\"capture_mode\":" + jsonQuote(captureMode) +
           ",\"generation\":" + std::to_string(generation) + ",\"camera_id\":" + cameraField +
           ",\"always_on\":" + (alwaysOn ? "true" : "false") + "}";
}

const char* statusName(AnalysisStatus status)
{
    switch (status) {
    case AnalysisStatus::Absent:
        return "absent";
    case AnalysisStatus::Present:
        return "present";
    case AnalysisStatus::InsufficientLight:
        return "insufficient_light";
    case AnalysisStatus::UsableLight:
        return "usable_light";
    case AnalysisStatus::Error:
        break;
    }
    return "unavailable";
}

}  // namespace

struct CameraLifecycleDriver::Impl final : ICameraBackendCallbacks {
    Impl(ICameraBackend& camera, IFrameAnalyzer& analyzer, ICoreSubmitter& submitter, ILifecycleApi& lifecycle,
         IClock& clock, IScheduler& scheduler)
        : camera_(camera), analyzer_(analyzer), submitter_(submitter), lifecycle_(lifecycle), clock_(clock),
          scheduler_(scheduler)
    {
    }

    void onSnapshot(const Snapshot& snapshot)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        Effects fx;
        const bool policyChanged = snapshot.camera.generation != generation_ || snapshot.camera.mode != mode_ ||
                                   snapshot.camera.captureMode != captureMode_;
        selectedCameraId_ = snapshot.settings.selected_camera_id;
        alwaysOn_ = snapshot.settings.always_on_camera;
        const std::string cameraIds = lifecycleCameraIdList({}, selectedCameraId_);
        const std::string reply = lifecycle_.step(reconcileEvent(snapshot.camera.mode, snapshot.camera.captureMode,
                                                                 snapshot.camera.generation, cameraIds, alwaysOn_));
        // A rejected reconcile leaves the lifecycle unchanged, so stopping is the safe answer.
        const bool stop = reply.empty() || rawField(reply, "value") == "true";

        // The lifecycle decides whether the open camera stops. Without a stop, a policy change
        // still drops the pending timers.
        if (stop) {
            stopCapture(fx);
        } else if (policyChanged) {
            cancelPolicyTimers(fx);
        }
        mode_ = snapshot.camera.mode;
        captureMode_ = snapshot.camera.captureMode;
        generation_ = snapshot.camera.generation;
        armDetectorRelease(fx);
        if (!eligible()) {
            stopCapture(fx);
        } else if (sampleTimer_ == kInvalidTimerId && (continuous() ? !inferenceRequested_ : !sampling_)) {
            scheduleSample(millisFrom(snapshot.camera.nextSampleInMs.value_or(0.0)));
        }
        lock.unlock();
        flush(fx);
    }

    void onDevicesChanged() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        Effects fx;
        if (sampling_) {
            // Any change of the chosen camera restarts the session, which also covers a higher-ranked join.
            const CameraChoice choice = chooseCamera(camera_.enumerate(), historyOf());
            if (!choice.effective || choice.effective->id != activeCameraId_) {
                stopCapture(fx);
                if (eligible()) {
                    scheduleSample(std::chrono::milliseconds(0));
                }
            }
        }
        // A busy or unavailable camera is re-sent as unknown, so a re-enumerated camera is not held back.
        if (lastCameraStatus_ == "busy" || lastCameraStatus_ == "unavailable") {
            lastCameraStatus_ = "unknown";
            submitter_.submit(buildCameraStatus("unknown", generation_));
        }
        lock.unlock();
        flush(fx);
    }

    void shutdown()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        Effects fx;
        stopCapture(fx);
        retire(releaseTimer_, fx);
        ++releaseEpoch_;
        fx.releaseDetector = true;
        lock.unlock();
        flush(fx);
    }

    void onFrame(const CameraFrame& frame) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || !sampling_ || (continuous() && !inferenceRequested_) ||
            frame.steadySeconds < sampleStartedAt_) {
            return;
        }
        const uint64_t session = session_;
        if (!receivedFirstFrame_) {
            receivedFirstFrame_ = true;
            lifecycle_.step(plainEvent("acquired_frame"));
        }
        const std::string decision = rawField(lifecycle_.step(timedEvent("frame", frame.steadySeconds)), "value");
        Effects fx;
        if (decision == "discard_warmup") {
            // The watchdog without a token only guards the first frame, so warmup ends it.
            if (!timeoutToken_) {
                retire(timeoutTimer_, fx);
                ++timeoutEpoch_;
            }
            lock.unlock();
            flush(fx);
            return;
        }
        if (decision != "assess_light" && decision != "assess_presence") {
            // assessment_expired: the post-settle deadline passed, so the frame is not analysed.
            stopCapture(fx);
            if (decision == "assessment_expired") {
                reportLocked("unavailable", "busy");
            } else {
                reportLocked("unavailable", "error");
            }
            lock.unlock();
            flush(fx);
            return;
        }

        const bool light = decision == "assess_light";
        retire(timeoutTimer_, fx);
        timeoutToken_.reset();
        ++timeoutEpoch_;
        inferenceRequested_ = false;
        const bool keepOpen = keepSessionOpen();
        lock.unlock();
        flush(fx);

        // Analysis runs without the mutex. The result is dropped if the session ended meanwhile.
        const FrameAnalysis analysis = light ? analyzer_.assessLight(frame) : analyzer_.detect(frame);

        lock.lock();
        if (stopped_ || !sampling_ || session != session_) {
            return;
        }
        Effects end;
        bool keep = keepOpen;
        if (analysis.status == AnalysisStatus::Error) {
            reportLocked("unavailable", "error");
        } else if (!light && analysis.status == AnalysisStatus::InsufficientLight &&
                   captureMode_ == "single_frame" && startDarkSettle(end)) {
            // The settle reports the outcome later, so nothing is sent now.
            keep = true;
        } else {
            reportLocked(statusName(analysis.status), nullptr);
        }
        if (!keep) {
            stopCapture(end);
        }
        lock.unlock();
        flush(end);
    }

    void onFailure(const CameraFailure& failure) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || !sampling_) {
            return;
        }
        Effects fx;
        stopCapture(fx);
        reportLocked("unavailable", cameraStatusString(classifyCameraFailure(failure)));
        lock.unlock();
        flush(fx);
    }

private:
    ICameraBackend& camera_;
    IFrameAnalyzer& analyzer_;
    ICoreSubmitter& submitter_;
    ILifecycleApi& lifecycle_;
    IClock& clock_;
    IScheduler& scheduler_;

    // Every field below is guarded by mutex_. Timer and camera callbacks compare the session,
    // generation and epoch they captured, so a callback that outlived its session does nothing.
    std::mutex mutex_;
    bool stopped_ = false;
    std::string mode_ = "disabled";
    std::string captureMode_ = "single_frame";
    uint64_t generation_ = 0;
    bool alwaysOn_ = false;
    std::optional<std::string> selectedCameraId_;
    bool sampling_ = false;  // The camera is open for the current session.
    uint64_t session_ = 0;
    std::string activeCameraId_;
    bool inferenceRequested_ = false;
    bool receivedFirstFrame_ = false;
    uint64_t sampleGeneration_ = 0;
    double sampleStartedAt_ = 0.0;
    std::optional<uint64_t> timeoutToken_;
    TimerId sampleTimer_ = kInvalidTimerId;
    TimerId timeoutTimer_ = kInvalidTimerId;
    TimerId settleTimer_ = kInvalidTimerId;
    TimerId releaseTimer_ = kInvalidTimerId;
    uint64_t sampleEpoch_ = 0;
    uint64_t timeoutEpoch_ = 0;
    uint64_t releaseEpoch_ = 0;
    std::string lastCameraStatus_ = "ready";

    bool eligible() const { return mode_ == "face" || mode_ == "light_only"; }

    bool continuous() const
    {
        return captureMode_ == "continuous_return" || captureMode_ == "continuous_monitoring";
    }

    bool keepSessionOpen() const
    {
        return captureMode_ == "continuous_return" || (alwaysOn_ && captureMode_ == "continuous_monitoring");
    }

    std::vector<std::string> historyOf() const
    {
        std::vector<std::string> history;
        if (selectedCameraId_) {
            history.push_back(*selectedCameraId_);
        }
        return history;
    }

    // Runs with the mutex released.
    void flush(Effects& fx)
    {
        for (const TimerId id : fx.cancel) {
            scheduler_.cancel(id);
        }
        if (fx.close) {
            camera_.close();
        }
        if (fx.releaseDetector) {
            analyzer_.releaseIdle();
        }
        if (fx.openId) {
            camera_.open(*fx.openId, fx.openFps, *this);
        }
    }

    // Under the mutex. Sends the result only when the lifecycle accepts the sample's generation.
    void reportLocked(const char* status, const char* cameraStatus)
    {
        const std::string reply = lifecycle_.step(generationEvent(sampleGeneration_));
        if (rawField(reply, "value") != "true") {
            return;
        }
        lastCameraStatus_ = cameraStatus != nullptr ? cameraStatus : "ready";
        std::optional<std::string> field;
        if (cameraStatus != nullptr) {
            field = cameraStatus;
        }
        submitter_.submit(buildPresenceObservation(status, clock_.epochMillis(), sampleGeneration_, field));
    }

    void cancelPolicyTimers(Effects& fx)
    {
        retire(sampleTimer_, fx);
        ++sampleEpoch_;
        retire(settleTimer_, fx);
        retire(timeoutTimer_, fx);
        ++timeoutEpoch_;
        timeoutToken_.reset();
        inferenceRequested_ = false;
        lifecycle_.step(plainEvent("cancel_deadlines"));
    }

    void stopCapture(Effects& fx)
    {
        cancelPolicyTimers(fx);
        if (sampling_) {
            fx.close = true;
        }
        sampling_ = false;
        ++session_;
        activeCameraId_.clear();
        receivedFirstFrame_ = false;
    }

    // Under the mutex. Restarts the detector release after every non-face snapshot.
    void armDetectorRelease(Effects& fx)
    {
        retire(releaseTimer_, fx);
        ++releaseEpoch_;
        if (mode_ == "face") {
            return;
        }
        const uint64_t epoch = releaseEpoch_;
        releaseTimer_ = scheduler_.scheduleOnce(kDetectorIdle, toleranceFor(kDetectorIdle),
                                                [this, epoch] { onDetectorIdle(epoch); });
    }

    void onDetectorIdle(uint64_t epoch)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || epoch != releaseEpoch_ || mode_ == "face") {
            return;
        }
        releaseTimer_ = kInvalidTimerId;
        Effects fx;
        fx.releaseDetector = true;
        lock.unlock();
        flush(fx);
    }

    void scheduleSample(std::chrono::milliseconds delay)
    {
        ++sampleEpoch_;
        const uint64_t epoch = sampleEpoch_;
        const uint64_t generation = generation_;
        sampleTimer_ = scheduler_.scheduleOnce(delay, toleranceFor(delay),
                                               [this, epoch, generation] { onSampleTimer(epoch, generation); });
    }

    void onSampleTimer(uint64_t epoch, uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || epoch != sampleEpoch_ || generation != generation_) {
            return;
        }
        sampleTimer_ = kInvalidTimerId;
        Effects fx;
        requestSample(fx);
        lock.unlock();
        flush(fx);
    }

    void requestSample(Effects& fx)
    {
        if (!eligible()) {
            return;
        }
        sampleStartedAt_ = clock_.steadySeconds();
        sampleGeneration_ = generation_;
        if (continuous()) {
            inferenceRequested_ = true;
            if (sampling_ && mode_ != "light_only") {
                armCaptureTimeout(std::nullopt, fx);
            }
        }
        if (mode_ == "light_only") {
            startCapture(fx);
            if (!sampling_) {
                return;
            }
            const std::string reply = lifecycle_.step(timedEvent("begin_light_assessment", clock_.steadySeconds()));
            retire(settleTimer_, fx);
            scheduleSettle(kSettle, uintField(reply, "assessment_token"));
            return;
        }
        startCapture(fx);
    }

    void startCapture(Effects& fx)
    {
        if (sampling_) {
            return;
        }
        // Enumerated on every sample, so a reconnected camera is used without a settings change.
        const CameraChoice choice = chooseCamera(camera_.enumerate(), historyOf());
        if (!choice.effective) {
            reportLocked("unavailable", "unavailable");
            return;
        }
        sampling_ = true;
        ++session_;
        activeCameraId_ = choice.effective->id;
        receivedFirstFrame_ = false;
        if (continuous()) {
            inferenceRequested_ = true;
        }
        lifecycle_.step(timedEvent("begin_acquisition", clock_.steadySeconds()));
        armCaptureTimeout(std::nullopt, fx);
        fx.openId = activeCameraId_;
        fx.openFps = continuous() ? kRetainedFps : 0.0;
    }

    void armCaptureTimeout(std::optional<uint64_t> token, Effects& fx)
    {
        retire(timeoutTimer_, fx);
        timeoutToken_ = token;
        ++timeoutEpoch_;
        const uint64_t epoch = timeoutEpoch_;
        const uint64_t session = session_;
        const uint64_t generation = generation_;
        timeoutTimer_ = scheduler_.scheduleOnce(kWatchdog, toleranceFor(kWatchdog),
                                                [this, epoch, session, generation, token] {
                                                    onCaptureTimeout(epoch, session, generation, token);
                                                });
    }

    void onCaptureTimeout(uint64_t epoch, uint64_t session, uint64_t generation, std::optional<uint64_t> token)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || !sampling_ || session != session_ || generation != generation_ || epoch != timeoutEpoch_) {
            return;
        }
        timeoutTimer_ = kInvalidTimerId;
        Effects fx;
        if (mode_ == "light_only" || token) {
            if (token && rawField(lifecycle_.step(tokenEvent(*token)), "value") != "true") {
                return;
            }
            if (rawField(lifecycle_.step(timedEvent("timed_out", clock_.steadySeconds())), "value") != "true") {
                return;
            }
        } else {
            const bool stillWaiting = continuous() ? inferenceRequested_ : !receivedFirstFrame_;
            if (!stillWaiting) {
                return;
            }
        }
        stopCapture(fx);
        reportLocked("unavailable", "busy");
        lock.unlock();
        flush(fx);
    }

    // Under the mutex. Starts the dark settle of a single-frame face sample, once per sample.
    bool startDarkSettle(Effects& fx)
    {
        const double now = clock_.steadySeconds();
        const std::string reply = lifecycle_.step(timedEvent("begin_dark_settle", now));
        if (rawField(reply, "value") != "true") {
            return false;
        }
        retire(settleTimer_, fx);
        const double untilSeconds = numberField(reply, "settling_until");
        scheduleSettle(millisFrom((untilSeconds - now) * 1000.0), uintField(reply, "assessment_token"));
        return true;
    }

    void scheduleSettle(std::chrono::milliseconds delay, uint64_t token)
    {
        const uint64_t session = session_;
        const uint64_t generation = generation_;
        settleTimer_ = scheduler_.scheduleOnce(delay, toleranceFor(delay), [this, session, generation, token] {
            onSettle(session, generation, token);
        });
    }

    // Ends a light settle or a dark settle, then arms the post-settle watchdog for the token.
    void onSettle(uint64_t session, uint64_t generation, uint64_t token)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || !sampling_ || session != session_ || generation != generation_ || !eligible()) {
            return;
        }
        const std::string reply = lifecycle_.step(tokenEvent(token));
        if (rawField(reply, "value") != "true") {
            return;
        }
        if (mode_ == "light_only" && rawField(reply, "acquisition_pending") != "false") {
            return;
        }
        settleTimer_ = kInvalidTimerId;
        Effects fx;
        armCaptureTimeout(token, fx);
        lock.unlock();
        flush(fx);
    }
};

CameraLifecycleDriver::CameraLifecycleDriver(ICameraBackend& camera, IFrameAnalyzer& analyzer,
                                             ICoreSubmitter& submitter, ILifecycleApi& lifecycle, IClock& clock,
                                             IScheduler& scheduler)
    : impl_(std::make_unique<Impl>(camera, analyzer, submitter, lifecycle, clock, scheduler))
{
}

CameraLifecycleDriver::~CameraLifecycleDriver()
{
    impl_->shutdown();
}

void CameraLifecycleDriver::onSnapshot(const Snapshot& snapshot)
{
    impl_->onSnapshot(snapshot);
}

void CameraLifecycleDriver::onDevicesChanged()
{
    impl_->onDevicesChanged();
}

void CameraLifecycleDriver::shutdown()
{
    impl_->shutdown();
}

}  // namespace telaviva
