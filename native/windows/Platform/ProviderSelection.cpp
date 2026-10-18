#include "ProviderSelection.h"

namespace telaviva {

namespace {

// TV_INFERENCE_PRESENT. Kept as a local constant so this file needs no C header.
constexpr int kPresent = 1;

}  // namespace

std::optional<FailureReason> judgeSelfTest(const SelfTestSample& accelerated, const SelfTestSample& cpu)
{
    if (accelerated.status != kPresent || cpu.status != kPresent) {
        return FailureReason::SelfTestMismatch;
    }
    if (accelerated.seconds > kMaxAcceleratedLatencySeconds) {
        return FailureReason::SelfTestSlow;
    }
    return std::nullopt;
}

}  // namespace telaviva
