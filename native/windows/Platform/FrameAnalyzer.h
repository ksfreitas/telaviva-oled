#pragma once
// Frame analysis seam between the camera lifecycle and the inference host.
// Implementations must not keep frame bytes after a call returns.
#include "ICameraBackend.h"

namespace telaviva {

enum class AnalysisStatus { Absent, Present, InsufficientLight, UsableLight, Error };

struct FrameAnalysis {
    AnalysisStatus status = AnalysisStatus::Error;
};

struct IFrameAnalyzer {
    virtual ~IFrameAnalyzer() = default;
    // Present, Absent, InsufficientLight or Error.
    virtual FrameAnalysis detect(const CameraFrame& frame) = 0;
    // UsableLight, InsufficientLight or Error.
    virtual FrameAnalysis assessLight(const CameraFrame& frame) = 0;
    // Drops the detector now instead of waiting for the idle timer.
    virtual void releaseIdle() = 0;
};

}  // namespace telaviva
