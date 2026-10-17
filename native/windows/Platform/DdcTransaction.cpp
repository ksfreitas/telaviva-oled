#include "DdcTransaction.h"

#include <cstddef>
#include <utility>

namespace telaviva {

namespace {

struct ReadOutcome {
    bool ok = false;
    uint32_t current = 0;
    DdcError error = DdcError::None;
};

const char* errorName(DdcError error)
{
    switch (error) {
    case DdcError::Unsupported:
        return "unsupported";
    case DdcError::Disconnected:
        return "disconnected";
    case DdcError::None:
    case DdcError::Io:
        break;
    }
    // A failure without a known cause is reported as io.
    return "io";
}

// Retrying these cannot change the outcome, so they fail fast.
bool isPermanent(DdcError error)
{
    return error == DdcError::Unsupported || error == DdcError::Disconnected;
}

DdcResult failure(DdcError error)
{
    DdcResult result;
    result.ok = false;
    result.error = errorName(error);
    return result;
}

// One initial attempt plus one attempt per readRetryDelays entry. The delay
// before attempt N+1 is readRetryDelays[N]; no delay follows the final attempt.
ReadOutcome readWithRetry(IDdcPort& port, ISleeper& sleeper, const DdcTimingProfile& profile, uint8_t code)
{
    ReadOutcome outcome;
    const std::size_t attempts = profile.readRetryDelays.size() + 1;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        DdcReading reading;
        DdcError error = DdcError::None;
        if (port.readVcp(code, reading, error)) {
            outcome.ok = true;
            outcome.current = reading.current;
            outcome.error = DdcError::None;
            return outcome;
        }
        outcome.error = error;
        if (isPermanent(error) || attempt + 1 == attempts) {
            break;
        }
        sleeper.sleepFor(profile.readRetryDelays[attempt]);
    }
    return outcome;
}

}  // namespace

DdcTransaction::DdcTransaction(IDdcPort& port, ISleeper& sleeper, DdcTimingProfile profile)
    : port_(port), sleeper_(sleeper), profile_(std::move(profile))
{
}

DdcResult DdcTransaction::readVcp(uint8_t code)
{
    const ReadOutcome outcome = readWithRetry(port_, sleeper_, profile_, code);
    if (!outcome.ok) {
        return failure(outcome.error);
    }
    DdcResult result;
    result.ok = true;
    result.value = outcome.current;
    return result;
}

DdcResult DdcTransaction::setAndVerify(uint8_t code, uint32_t target)
{
    // A write only submits the packet, so the readback below decides success.
    // A failed write is not retried here and skips the readback.
    DdcError error = DdcError::None;
    if (!port_.writeVcp(code, target, error)) {
        return failure(error);
    }
    if (profile_.duplicateSet) {
        sleeper_.sleepFor(profile_.duplicateGap);
        if (!port_.writeVcp(code, target, error)) {
            return failure(error);
        }
    }

    sleeper_.sleepFor(profile_.interTransactionDelay);
    const ReadOutcome readback = readWithRetry(port_, sleeper_, profile_, code);
    if (!readback.ok) {
        return failure(readback.error);
    }

    DdcResult result;
    result.value = readback.current;
    result.ok = readback.current == target;
    if (!result.ok) {
        result.error = "io";
    }
    return result;
}

}  // namespace telaviva
