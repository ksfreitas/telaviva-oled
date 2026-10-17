#pragma once
#include "ISleeper.h"

#include <chrono>
#include <thread>

namespace telaviva {

class SystemSleeper final : public ISleeper {
public:
    void sleepFor(std::chrono::milliseconds duration) override
    {
        std::this_thread::sleep_for(duration);
    }
};

}  // namespace telaviva
