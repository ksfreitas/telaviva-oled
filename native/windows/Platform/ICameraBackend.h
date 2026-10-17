#pragma once
// Camera seam for the presence pipeline. The MediaCapture adapter and FakeCamera
// implement it, so the lifecycle driver never touches WinRT.
#include "CameraSelection.h"
#include "CameraStatus.h"

#include <cstdint>
#include <string>
#include <vector>

namespace telaviva {

// Top-down BGRA8, as tv_inference_detect_bgra expects. steadySeconds comes from
// the same steady clock as the camera lifecycle.
struct CameraFrame {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    int stride = 0;
    double steadySeconds = 0;
};

struct ICameraBackendCallbacks {
    virtual ~ICameraBackendCallbacks() = default;
    virtual void onFrame(const CameraFrame& frame) = 0;
    virtual void onFailure(const CameraFailure& failure) = 0;
    virtual void onDevicesChanged() = 0;
};

struct ICameraBackend {
    virtual ~ICameraBackend() = default;
    virtual std::vector<CameraInfo> enumerate() = 0;
    // Returns false on immediate failure. The same failure is also reported
    // through onFailure, so callers can handle it on one path.
    virtual bool open(const std::string& cameraId, double targetFps, ICameraBackendCallbacks& callbacks) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
};

}  // namespace telaviva
