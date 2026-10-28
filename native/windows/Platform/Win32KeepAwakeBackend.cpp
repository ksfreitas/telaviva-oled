#include "Win32KeepAwakeBackend.h"

#include <windows.h>
#include <wtsapi32.h>

#include <cwchar>
#include <future>
#include <utility>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")

namespace telaviva {

namespace {

// true: locked or not interactive. false: unlocked. nullopt: unknown.
// A failed query never reports unlocked.
std::optional<bool> liveSessionLockState()
{
    DWORD processSession = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &processSession) == FALSE) {
        return std::nullopt;
    }
    // Only the console session can show the user's desktop.
    if (processSession != WTSGetActiveConsoleSessionId()) {
        return true;
    }

    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSConnectState,
                                    &buffer, &bytes) == FALSE) {
        return std::nullopt;
    }
    if (buffer == nullptr || bytes < static_cast<DWORD>(sizeof(WTS_CONNECTSTATE_CLASS))) {
        if (buffer != nullptr) {
            WTSFreeMemory(buffer);
        }
        return std::nullopt;
    }
    const WTS_CONNECTSTATE_CLASS connectState = *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(buffer);
    WTSFreeMemory(buffer);
    if (connectState != WTSActive) {
        return true;
    }

    // The input desktop alone is not enough: the secure desktop must not count as unlocked.
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) {
        return std::nullopt;
    }
    wchar_t name[256] = {};
    const BOOL named = GetUserObjectInformationW(desktop, UOI_NAME, name, static_cast<DWORD>(sizeof(name)), nullptr);
    CloseDesktop(desktop);
    if (named == FALSE) {
        return std::nullopt;
    }
    return std::wcscmp(name, L"Default") != 0;
}

std::string boolText(std::optional<bool> value)
{
    if (!value) {
        return "unknown";
    }
    return *value ? "true" : "false";
}

const char* safetyText(KeepAwakeSafety safety)
{
    switch (safety) {
    case KeepAwakeSafety::Eligible:
        return "eligible";
    case KeepAwakeSafety::SessionInactive:
        return "session_inactive";
    case KeepAwakeSafety::SessionStateUnknown:
        return "session_state_unknown";
    case KeepAwakeSafety::ScreensaverActive:
        return "screensaver_active";
    case KeepAwakeSafety::ScreensaverStateUnknown:
        return "screensaver_state_unknown";
    case KeepAwakeSafety::DisplayAsleep:
        return "display_asleep";
    case KeepAwakeSafety::SystemSleeping:
        return "system_sleeping";
    case KeepAwakeSafety::LidClosed:
        return "lid_closed";
    }
    return "unknown";
}

}  // namespace

Win32KeepAwakeBackend::Win32KeepAwakeBackend(IScreensaverProbe& probe) : probe_(probe)
{
}

Win32KeepAwakeBackend::~Win32KeepAwakeBackend()
{
    // Clear the request while the worker still runs, then stop it.
    releaseRequest();
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        workerStopping_ = true;
        worker.swap(worker_);
    }
    workerWake_.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void Win32KeepAwakeBackend::setScreensaverActive(std::optional<bool> notified)
{
    std::lock_guard<std::mutex> lock(mutex_);
    saverNotified_ = notified;
}

void Win32KeepAwakeBackend::setDisplayAsleep(bool asleep)
{
    std::lock_guard<std::mutex> lock(mutex_);
    displayAsleep_ = asleep;
}

void Win32KeepAwakeBackend::setSystemSleeping(bool sleeping)
{
    std::lock_guard<std::mutex> lock(mutex_);
    systemSleeping_ = sleeping;
}

void Win32KeepAwakeBackend::setLidClosed(bool closed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lidClosed_ = closed;
}

void Win32KeepAwakeBackend::setSessionLocked(std::optional<bool> notified)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lockNotified_ = notified;
}

void Win32KeepAwakeBackend::readInputs(std::optional<bool>& saverNotified, std::optional<bool>& lockNotified,
                                       bool& displayAsleep, bool& systemSleeping, bool& lidClosed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    saverNotified = saverNotified_;
    lockNotified = lockNotified_;
    displayAsleep = displayAsleep_;
    systemSleeping = systemSleeping_;
    lidClosed = lidClosed_;
}

KeepAwakeSafety Win32KeepAwakeBackend::safetyState()
{
    std::optional<bool> saverNotified;
    std::optional<bool> lockNotified;
    bool displayAsleep = false;
    bool systemSleeping = false;
    bool lidClosed = false;
    readInputs(saverNotified, lockNotified, displayAsleep, systemSleeping, lidClosed);

    // The session is always active at this layer, so only the lock state is read.
    // The lid is diagnostic and does not enter the safety decision.
    const std::optional<bool> locked =
        resolveKeepAwakeGuard(lockNotified, [] { return liveSessionLockState(); });
    const std::optional<bool> saver =
        resolveKeepAwakeGuard(saverNotified, [this] { return probe_.isRunning(); });
    return resolveKeepAwakeSafety(true, locked, saver, displayAsleep, systemSleeping);
}

std::string Win32KeepAwakeBackend::diagnosticState()
{
    std::optional<bool> saverNotified;
    std::optional<bool> lockNotified;
    bool displayAsleep = false;
    bool systemSleeping = false;
    bool lidClosed = false;
    readInputs(saverNotified, lockNotified, displayAsleep, systemSleeping, lidClosed);

    const std::optional<bool> lockQuery = liveSessionLockState();
    const std::optional<bool> saverQuery = probe_.isRunning();
    const std::optional<bool> locked = resolveKeepAwakeGuard(lockNotified, [&lockQuery] { return lockQuery; });
    const std::optional<bool> saver = resolveKeepAwakeGuard(saverNotified, [&saverQuery] { return saverQuery; });
    const KeepAwakeSafety safety = resolveKeepAwakeSafety(true, locked, saver, displayAsleep, systemSleeping);
    const bool cameraAllowed = keepAwakeCameraMonitoringAllowed(safety);
    const bool posting = postingAllowed();

    std::string text = "session_active=true";
    const auto add = [&text](const char* key, const std::string& value) {
        text += ' ';
        text += key;
        text += '=';
        text += value;
    };
    add("session_locked", boolText(locked));
    add("lock_notification", boolText(lockNotified));
    add("lock_query", boolText(lockQuery));
    add("screensaver_active", boolText(saver));
    add("saver_notification", boolText(saverNotified));
    add("saver_query", boolText(saverQuery));
    add("display_asleep", boolText(displayAsleep));
    add("system_sleeping", boolText(systemSleeping));
    add("lid_closed", boolText(lidClosed));
    add("safety", safetyText(safety));
    add("camera_monitoring", boolText(cameraAllowed));
    add("posting_allowed", boolText(posting));
    return text;
}

bool Win32KeepAwakeBackend::postingAllowed()
{
    BOOL blocked = FALSE;
    if (SystemParametersInfoW(SPI_GETBLOCKSENDINPUTRESETS, 0, &blocked, 0) == 0) {
        return false;
    }
    return blocked == FALSE;
}

bool Win32KeepAwakeBackend::acquireRequest()
{
    if (!ensureWorker()) {
        return false;
    }
    return runOnWorker([this] {
        // Away mode is never requested. It would stop the system from sleeping while the
        // user is absent, which is not what this feature promises.
        const EXECUTION_STATE previous =
            SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        held_ = previous != 0;
        return held_;
    });
}

void Win32KeepAwakeBackend::releaseRequest()
{
    // Without a worker nothing was ever requested, so there is nothing to clear.
    runOnWorker([this] {
        if (held_) {
            // Cleared even if the call fails, so a failed release is never retried in a loop.
            held_ = false;
            SetThreadExecutionState(ES_CONTINUOUS);
        }
        return true;
    });
}

bool Win32KeepAwakeBackend::postActivity()
{
    // The controller checks these too, but the pulse denies itself on any unknown state.
    if (safetyState() != KeepAwakeSafety::Eligible || !postingAllowed()) {
        return false;
    }
    return runOnWorker([] {
        // A zero-displacement move: no key, no click and no cursor displacement.
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = 0;
        input.mi.dy = 0;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        return SendInput(1, &input, static_cast<int>(sizeof(INPUT))) == 1;
    });
}

bool Win32KeepAwakeBackend::ensureWorker()
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    if (workerStopping_) {
        return false;
    }
    if (worker_.joinable()) {
        return true;
    }
    try {
        worker_ = std::thread([this] { workerLoop(); });
    } catch (...) {
        return false;
    }
    return true;
}

bool Win32KeepAwakeBackend::runOnWorker(std::function<bool()> job)
{
    std::promise<bool> result;
    std::future<bool> outcome = result.get_future();
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (workerStopping_ || !worker_.joinable()) {
            return false;
        }
        // The caller waits for the result below, so the promise outlives the job.
        jobs_.push_back([job = std::move(job), &result] {
            bool ok = false;
            try {
                ok = job();
            } catch (...) {
                ok = false;
            }
            result.set_value(ok);
        });
    }
    workerWake_.notify_one();
    return outcome.get();
}

void Win32KeepAwakeBackend::workerLoop()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerWake_.wait(lock, [this] { return workerStopping_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                // Stopping, and every queued job has already run.
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

}  // namespace telaviva
