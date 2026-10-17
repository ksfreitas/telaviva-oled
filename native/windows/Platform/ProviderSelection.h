#pragma once
// Pure decisions for DirectML provider selection: which adapter matches a profile,
// and whether a self-test pair passes. No ABI, clock or file access happens here.
#include "AccelerationProfiles.h"
#include "AdapterCatalog.h"
#include "InferenceFailureStore.h"

#include <optional>
#include <vector>

namespace telaviva {

struct AccelerationMatch {
    AccelerationProfile profile;
    AdapterInfo adapter;
};

// First hardware adapter, in catalog order, that matches a profile in file order.
// Software adapters never match.
std::optional<AccelerationMatch> findAccelerationMatch(const std::vector<AccelerationProfile>& profiles,
                                                       const std::vector<AdapterInfo>& adapters);

// One self-test detect call: the status it returned and the seconds it took.
struct SelfTestSample {
    int status = -1;
    double seconds = 0.0;
};

// The accelerated session passes only when both sessions report present for the
// same frame (any error or other result is a mismatch) and the accelerated call is
// strictly faster than the CPU call. A tie or a slower call is a slow result.
// Returns the reason to record, or nullopt when the accelerated session may be used.
std::optional<FailureReason> judgeSelfTest(const SelfTestSample& accelerated, const SelfTestSample& cpu);

}  // namespace telaviva
