#pragma once
// One-shot timers on the Windows thread pool. Callbacks run on pool threads.
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace telaviva {

using TimerId = uint64_t;

// Returned when a timer could not be created; real ids start at 1.
inline constexpr TimerId kInvalidTimerId = 0;

struct IScheduler {
    virtual ~IScheduler() = default;
    // tolerance lets Windows coalesce wake-ups; the callback may run up to
    // tolerance late, never earlier than delay.
    virtual TimerId scheduleOnce(std::chrono::milliseconds delay, std::chrono::milliseconds tolerance,
                                 std::function<void()> fn) = 0;
    virtual void cancel(TimerId id) = 0;
};

// Matches the macOS leeway: 10% of the delay, capped at 1 s, so long timers
// do not drift far and short timers still wake close to their due time.
std::chrono::milliseconds toleranceFor(std::chrono::milliseconds delay);

class ThreadpoolScheduler final : public IScheduler {
public:
    ThreadpoolScheduler();
    ~ThreadpoolScheduler() override;

    ThreadpoolScheduler(const ThreadpoolScheduler&) = delete;
    ThreadpoolScheduler& operator=(const ThreadpoolScheduler&) = delete;

    TimerId scheduleOnce(std::chrono::milliseconds delay, std::chrono::milliseconds tolerance,
                         std::function<void()> fn) override;

    // Safe from any thread. After it returns the callback will not run. Do not
    // call it for a timer from inside that timer's own callback.
    void cancel(TimerId id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
