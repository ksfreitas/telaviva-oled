#include "Scheduler.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace telaviva {
namespace {

constexpr std::chrono::milliseconds kMaxTolerance{1000};

struct Entry {
    std::function<void()> fn;
    PTP_TIMER timer = nullptr;
    // Set after fn returns so finished timers can be pruned without waiting on them.
    std::atomic<bool> done{false};
};

VOID CALLBACK onTimerFired(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER)
{
    Entry* entry = static_cast<Entry*>(context);
    // Exceptions must not cross into the thread pool.
    try {
        entry->fn();
    } catch (...) {
    }
    entry->done.store(true);
}

// Waits for a running callback before closing, so the callback never touches
// a freed Entry. Callers must not hold the scheduler lock here.
void retire(std::unique_ptr<Entry> entry)
{
    if (entry->timer != nullptr) {
        WaitForThreadpoolTimerCallbacks(entry->timer, TRUE);
        CloseThreadpoolTimer(entry->timer);
        entry->timer = nullptr;
    }
}

}  // namespace

std::chrono::milliseconds toleranceFor(std::chrono::milliseconds delay)
{
    if (delay.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    return std::min(delay / 10, kMaxTolerance);
}

struct ThreadpoolScheduler::Impl {
    std::mutex mutex;
    std::unordered_map<TimerId, std::unique_ptr<Entry>> timers;
    TimerId nextId = 1;
};

ThreadpoolScheduler::ThreadpoolScheduler() : impl_(std::make_unique<Impl>()) {}

ThreadpoolScheduler::~ThreadpoolScheduler()
{
    std::unordered_map<TimerId, std::unique_ptr<Entry>> pending;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        pending.swap(impl_->timers);
    }
    for (auto& item : pending) {
        retire(std::move(item.second));
    }
}

TimerId ThreadpoolScheduler::scheduleOnce(std::chrono::milliseconds delay, std::chrono::milliseconds tolerance,
                                          std::function<void()> fn)
{
    auto entry = std::make_unique<Entry>();
    entry->fn = std::move(fn);
    entry->timer = CreateThreadpoolTimer(onTimerFired, entry.get(), nullptr);
    if (entry->timer == nullptr) {
        return kInvalidTimerId;
    }

    // Negative FILETIME means relative time, in 100 ns units.
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-(std::max<LONGLONG>(delay.count(), 0) * 10000));
    FILETIME dueTime{due.LowPart, due.HighPart};
    const DWORD windowMs = static_cast<DWORD>(std::max<LONGLONG>(tolerance.count(), 0));
    PTP_TIMER timer = entry->timer;

    // Track the entry before arming it, so no callback runs untracked.
    std::vector<std::unique_ptr<Entry>> finished;
    TimerId id = kInvalidTimerId;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto it = impl_->timers.begin(); it != impl_->timers.end();) {
            if (it->second->done.load()) {
                finished.push_back(std::move(it->second));
                it = impl_->timers.erase(it);
            } else {
                ++it;
            }
        }
        id = impl_->nextId++;
        impl_->timers.emplace(id, std::move(entry));
    }

    SetThreadpoolTimerEx(timer, &dueTime, 0, windowMs);

    for (auto& old : finished) {
        retire(std::move(old));
    }
    return id;
}

void ThreadpoolScheduler::cancel(TimerId id)
{
    std::unique_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->timers.find(id);
        if (it == impl_->timers.end()) {
            return;
        }
        entry = std::move(it->second);
        impl_->timers.erase(it);
    }
    retire(std::move(entry));
}

}  // namespace telaviva
