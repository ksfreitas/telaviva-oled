#pragma once
// Deterministic doubles for the Platform services. Not thread-safe; tests drive them from one thread.
#include "Platform/IClock.h"
#include "Platform/Scheduler.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace telaviva {

class FakeClock final : public IClock {
public:
    double steadySeconds() const override { return steady_; }
    uint64_t epochMillis() const override { return epoch_; }

    void setSteadySeconds(double seconds) { steady_ = seconds; }
    void setEpochMillis(uint64_t millis) { epoch_ = millis; }

    // Moves both readings together. Forward only.
    void advance(double seconds)
    {
        steady_ += seconds;
        epoch_ += static_cast<uint64_t>(std::llround(seconds * 1000.0));
    }

private:
    double steady_ = 0.0;
    uint64_t epoch_ = 0;
};

// Uses its own virtual time, independent of FakeClock. Timers fire in due
// order (ties by id) during advance(), and each tolerance is recorded.
class FakeScheduler final : public IScheduler {
public:
    TimerId scheduleOnce(std::chrono::milliseconds delay, std::chrono::milliseconds tolerance,
                         std::function<void()> fn) override
    {
        const TimerId id = nextId_++;
        toleranceLog_.push_back(tolerance);
        timers_.push_back(Timer{id, now_ + delay, std::move(fn)});
        return id;
    }

    void cancel(TimerId id) override
    {
        for (auto it = timers_.begin(); it != timers_.end(); ++it) {
            if (it->id == id) {
                timers_.erase(it);
                return;
            }
        }
    }

    void advance(std::chrono::milliseconds span)
    {
        const std::chrono::milliseconds target = now_ + span;
        for (;;) {
            auto next = timers_.end();
            for (auto it = timers_.begin(); it != timers_.end(); ++it) {
                if (it->due > target) {
                    continue;
                }
                if (next == timers_.end() || it->due < next->due ||
                    (it->due == next->due && it->id < next->id)) {
                    next = it;
                }
            }
            if (next == timers_.end()) {
                break;
            }
            Timer due = std::move(*next);
            timers_.erase(next);
            now_ = due.due;
            // The callback may schedule or cancel, so no iterator survives this call.
            due.fn();
        }
        now_ = target;
    }

    const std::vector<std::chrono::milliseconds>& toleranceLog() const { return toleranceLog_; }
    std::size_t pendingCount() const { return timers_.size(); }

private:
    struct Timer {
        TimerId id;
        std::chrono::milliseconds due;
        std::function<void()> fn;
    };

    std::vector<Timer> timers_;
    std::vector<std::chrono::milliseconds> toleranceLog_;
    std::chrono::milliseconds now_{0};
    TimerId nextId_ = 1;
};

}  // namespace telaviva
