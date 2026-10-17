#include "CameraStatus.h"

namespace telaviva {

namespace {

// E_ACCESSDENIED, returned by MediaCapture when camera privacy blocks the app.
constexpr uint32_t kAccessDenied = 0x80070005u;

}  // namespace

CameraStatusCode classifyCameraFailure(const CameraFailure& failure)
{
    // Order matters: a denied or missing device is not a busy camera even if a deadline also fired.
    if (failure.hresult == kAccessDenied) {
        return CameraStatusCode::Permission;
    }
    if (!failure.devicePresent) {
        return CameraStatusCode::Unavailable;
    }
    if (failure.deadlineExceeded || failure.startFailed) {
        return CameraStatusCode::Busy;
    }
    // Conversion and inference errors, and anything unrecognized, count as a camera error.
    return CameraStatusCode::Error;
}

const char* cameraStatusString(CameraStatusCode code)
{
    switch (code) {
    case CameraStatusCode::Permission:
        return "permission";
    case CameraStatusCode::Unavailable:
        return "unavailable";
    case CameraStatusCode::Busy:
        return "busy";
    case CameraStatusCode::Error:
        return "error";
    }
    return "error";
}

}  // namespace telaviva
