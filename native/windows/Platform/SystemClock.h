#pragma once
#include "IClock.h"

#include <chrono>

namespace telaviva {

class SystemClock final : public IClock {
public:
    double steadySeconds() const override
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // system_clock is Unix time in C++20, so this is the epoch the core expects.
    uint64_t epochMillis() const override
    {
        const auto since = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
    }
};

}  // namespace telaviva
