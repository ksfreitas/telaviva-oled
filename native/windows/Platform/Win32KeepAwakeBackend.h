#pragma once
// Real OS calls behind IKeepAwakeBackend: the execution-state request, zero-displacement
// mouse input, and the session and guard reads. The Win32 headers stay in the .cpp.
#include "KeepAwakeController.h"
#include "ScreensaverMonitor.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace telaviva {

class Win32KeepAwakeBackend final : public IKeepAwakeBackend {
public:
    explicit Win32KeepAwakeBackend(IScreensaverProbe& probe);
    // Releases the request on the worker, then stops and joins the worker.
    ~Win32KeepAwakeBackend() override;

    Win32KeepAwakeBackend(const Win32KeepAwakeBackend&) = delete;
    Win32KeepAwakeBackend& operator=(const Win32KeepAwakeBackend&) = delete;

    // Notifications from the shell. Thread-safe. nullopt means no notification.
    void setScreensaverActive(std::optional<bool> notified);
    void setDisplayAsleep(bool asleep);
    void setSystemSleeping(bool sleeping);
    // Diagnostic only. A closed lid does not veto the session.
    void setLidClosed(bool closed);
    void setSessionLocked(std::optional<bool> notified);

    KeepAwakeSafety safetyState() override;
    bool postingAllowed() override;
    bool acquireRequest() override;
    void releaseRequest() override;
    bool postActivity() override;

    // Read-only "key=value ..." line of every guard input, for diagnostics.
    std::string diagnosticState();

private:
    // Copies the notification and power inputs under the mutex.
    void readInputs(std::optional<bool>& saverNotified, std::optional<bool>& lockNotified,
                    bool& displayAsleep, bool& systemSleeping, bool& lidClosed);
    // Starts the worker if it is not running. False if the worker is stopping or cannot start.
    bool ensureWorker();
    // Runs job on the worker and blocks for its result. False if no worker is running.
    bool runOnWorker(std::function<bool()> job);
    void workerLoop();

    IScreensaverProbe& probe_;

    // Guards the inputs below.
    std::mutex mutex_;
    std::optional<bool> saverNotified_;
    std::optional<bool> lockNotified_;
    bool displayAsleep_ = false;
    bool systemSleeping_ = false;
    bool lidClosed_ = false;

    // Guards the worker thread and queue. The worker is the only thread that calls the
    // execution-state APIs, because they apply per thread.
    std::mutex workerMutex_;
    std::condition_variable workerWake_;
    std::deque<std::function<void()>> jobs_;
    bool workerStopping_ = false;
    std::thread worker_;
    // Touched only on the worker thread.
    bool held_ = false;
};

}  // namespace telaviva
