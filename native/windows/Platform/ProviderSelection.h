#pragma once
// Pure decision for DirectML self-tests: whether a pair of detect calls on the same
// frame passes. No ABI, clock or file access happens here.
#include "AdapterCatalog.h"
#include "InferenceFailureStore.h"

#include <optional>

namespace telaviva {

// Longest accelerated detect call accepted (seconds). It applies once per preference session,
// to the first detection, and is 500 ms on both macOS and Windows: the first accelerated
// inference can compile lazily (CoreML measured 242 ms on macOS).
inline constexpr double kMaxAcceleratedLatencySeconds = 0.500;

// One self-test detect call: the status it returned and the seconds it took.
struct SelfTestSample {
    int status = -1;
    double seconds = 0.0;
};

// The accelerated session passes only when both sessions report present for the
// same frame (any error or other result is a mismatch) and the accelerated call
// takes at most kMaxAcceleratedLatencySeconds. Speed relative to the CPU is not
// checked. Returns the reason to record, or nullopt when the accelerated session may be used.
std::optional<FailureReason> judgeSelfTest(const SelfTestSample& accelerated, const SelfTestSample& cpu);

}  // namespace telaviva
