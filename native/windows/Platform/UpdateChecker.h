#pragma once
// Update check for the About page. The HTTP GET and the core evaluation run on a job
// runner's thread; the observer is called from that thread, never under the lock.
#include "IClock.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace telaviva {

enum class UpdateKind { Idle, Checking, UpToDate, UpdateAvailable, NoRelease, RateLimited, Failed };

struct UpdateState {
    UpdateKind kind = UpdateKind::Idle;
    std::string remoteVersion;
    std::string releaseUrl;
};

struct HttpResult {
    bool transportOk = false;
    int status = 0;
    std::string body;
};

struct IHttpGet {
    virtual ~IHttpGet() = default;
    virtual HttpResult get(const std::string& url, const std::string& userAgent) = 0;
};

struct UpdateOutcome {
    std::string status;
    std::string remoteVersion;
    std::string releaseUrl;
};

using UpdateEvaluator =
    std::function<std::optional<UpdateOutcome>(const std::string& installed, int status, const std::string& body)>;
using JobRunner = std::function<void(std::function<void()>)>;

class UpdateChecker {  // shared internal state; safe if destroyed while a job runs
public:
    UpdateChecker(std::string installedVersion, std::shared_ptr<IHttpGet> http, UpdateEvaluator evaluate,
                  std::shared_ptr<IClock> clock, JobRunner runner);
    ~UpdateChecker() = default;

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    UpdateState state() const;
    void checkIfNeeded();
    void checkNow();
    void setOnChanged(std::function<void()> onChanged);  // called from the worker thread

private:
    struct Impl;

    // Expects the lock held; releases it before notifying and running the job.
    void start(std::unique_lock<std::mutex>& lock);
    static void runJob(const std::shared_ptr<Impl>& impl);

    std::shared_ptr<Impl> impl_;
    JobRunner runner_;
};

}  // namespace telaviva
