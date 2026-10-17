#include "ProviderSelection.h"

namespace telaviva {

namespace {

// TV_INFERENCE_PRESENT. Kept as a local constant so this file needs no C header.
constexpr int kPresent = 1;

}  // namespace

std::optional<AccelerationMatch> findAccelerationMatch(const std::vector<AccelerationProfile>& profiles,
                                                       const std::vector<AdapterInfo>& adapters)
{
    for (const AdapterInfo& adapter : adapters) {
        if (adapter.software) {
            continue;
        }
        const AccelerationProfile* profile = matchProfile(profiles, adapter);
        if (profile != nullptr) {
            return AccelerationMatch{*profile, adapter};
        }
    }
    return std::nullopt;
}

std::optional<FailureReason> judgeSelfTest(const SelfTestSample& accelerated, const SelfTestSample& cpu)
{
    if (accelerated.status != kPresent || cpu.status != kPresent) {
        return FailureReason::SelfTestMismatch;
    }
    if (!(accelerated.seconds < cpu.seconds)) {
        return FailureReason::SelfTestSlow;
    }
    return std::nullopt;
}

}  // namespace telaviva
