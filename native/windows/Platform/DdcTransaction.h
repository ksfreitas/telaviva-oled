#pragma once
// Retry, pacing and readback rules for one monitor's DDC/CI traffic. A write is
// only a submission, so setAndVerify succeeds only when a readback matches.
#include "DdcTimingProfile.h"
#include "IDdcPort.h"
#include "ISleeper.h"

#include <cstdint>
#include <optional>
#include <string>

namespace telaviva {

struct DdcResult {
    bool ok = false;
    std::optional<uint32_t> value;
    std::string error;  // "", "unsupported", "io", "disconnected"
};

class DdcTransaction {
public:
    DdcTransaction(IDdcPort& port, ISleeper& sleeper, DdcTimingProfile profile);

    // Retries io failures with profile.readRetryDelays between attempts.
    DdcResult readVcp(uint8_t code);

    // Writes, optionally repeats the write after duplicateGap, waits
    // interTransactionDelay, then reads back. ok only when the readback equals target.
    DdcResult setAndVerify(uint8_t code, uint32_t target);

private:
    IDdcPort& port_;
    ISleeper& sleeper_;
    DdcTimingProfile profile_;
};

}  // namespace telaviva
