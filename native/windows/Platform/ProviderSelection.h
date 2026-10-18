#pragma once
// Pure decision for DirectML self-tests: whether a pair of detect calls on the same
// frame passes. No ABI, clock or file access happens here.
#include "AdapterCatalog.h"
#include "InferenceFailureStore.h"

#include <optional>

namespace telaviva {

// Longest accelerated detect call that still fits the detection cadence (seconds).
inline constexpr double kMaxAcceleratedLatencySeconds = 0.125;   // half of the 0.25 s minimum return interval

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
