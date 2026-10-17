#pragma once
// WinRT MediaCapture adapter for ICameraBackend. WinRT and windows.h types stay in the .cpp.
#include "ICameraBackend.h"
#include "IClock.h"

#include <memory>
#include <string>
#include <vector>

namespace telaviva {

class MediaCaptureBackend final : public ICameraBackend {
public:
    // The clock must outlive the backend.
    explicit MediaCaptureBackend(const IClock& clock);
    ~MediaCaptureBackend() override;

    MediaCaptureBackend(const MediaCaptureBackend&) = delete;
    MediaCaptureBackend& operator=(const MediaCaptureBackend&) = delete;

    // Blocks on WinRT async calls, so it must run off the UI thread.
    std::vector<CameraInfo> enumerate() override;
    // Blocks on WinRT async calls. A second open replaces the open session.
    bool open(const std::string& cameraId, double targetFps, ICameraBackendCallbacks& callbacks) override;
    void close() override;
    bool isOpen() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
