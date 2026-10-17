#pragma once
// Runs all DDC work on one thread. dxva2 gives no thread-safety guarantee, so
// serializing every port call here is what keeps the handles safe.
#include "Platform/DdcController.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace telaviva::host {

class ThreadExecutor final : public telaviva::IDdcExecutor {
public:
    ThreadExecutor() : worker_([this] { run(); }) {}

    // Runs the queued work, including work the queue gains while draining, then joins.
    ~ThreadExecutor() override
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        worker_.join();
    }

    ThreadExecutor(const ThreadExecutor&) = delete;
    ThreadExecutor& operator=(const ThreadExecutor&) = delete;

    void post(std::function<void()> work) override
    {
        {
            std::lock_guard lock(mutex_);
            // Work posted after the worker has exited would never run; drop it.
            if (exited_) {
                return;
            }
            tasks_.push_back(std::move(work));
        }
        wake_.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    exited_ = true;
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            // One failing task must not end the only DDC thread.
            try {
                task();
            } catch (...) {
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    bool exited_ = false;
    // Declared last so run() starts after every other member is constructed.
    std::thread worker_;
};

}  // namespace telaviva::host
