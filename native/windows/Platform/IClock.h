#pragma once
// Two clocks because timers need a monotonic reading while logs and settings
// need wall-clock time. Tests inject a fake through this interface.
#include <cstdint>

namespace telaviva {

struct IClock {
    virtual ~IClock() = default;
    virtual double steadySeconds() const = 0;
    virtual uint64_t epochMillis() const = 0;
};

}  // namespace telaviva
