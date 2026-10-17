#pragma once
// Injected so DDC pacing can be tested without real delays.
#include <chrono>

namespace telaviva {

struct ISleeper {
    virtual ~ISleeper() = default;
    virtual void sleepFor(std::chrono::milliseconds) = 0;
};

}  // namespace telaviva
