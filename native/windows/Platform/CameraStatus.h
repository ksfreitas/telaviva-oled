#pragma once
// Maps camera backend failures onto the core's camera_status values. The
// precedence below keeps a removed device from being reported as a busy one.
#include <cstdint>

namespace telaviva {

// Facts the backend gathered about a failure. Defaults describe a healthy device.
struct CameraFailure {
    uint32_t hresult = 0;
    bool devicePresent = true;
    bool deadlineExceeded = false;
    bool startFailed = false;
    bool conversionFailed = false;
    bool inferenceFailed = false;
};

// Values match the camera_status strings in contracts.md.
enum class CameraStatusCode { Permission, Unavailable, Busy, Error };

CameraStatusCode classifyCameraFailure(const CameraFailure& failure);

const char* cameraStatusString(CameraStatusCode code);

}  // namespace telaviva
