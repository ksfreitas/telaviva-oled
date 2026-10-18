// Update checker state machine with a fake HTTP client, fake evaluator, fake clock and
// controlled job runners, plus the release evaluation through CoreBridge and the real core DLL.
#include "TestHarness.h"
#include "Fakes.h"
#include "Platform/CoreBridge.h"
#include "Platform/UpdateChecker.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using telaviva::HttpResult;
using telaviva::JobRunner;
using telaviva::UpdateChecker;
using telaviva::UpdateEvaluator;
using telaviva::UpdateKind;
using telaviva::UpdateOutcome;
using telaviva::UpdateState;

struct FakeHttp final : telaviva::IHttpGet {
    HttpResult next{true, 200, "{}"};
    int calls = 0;
    std::string lastUrl;
    std::string lastUserAgent;

    HttpResult get(const std::string& url, const std::string& userAgent) override
    {
        ++calls;
        lastUrl = url;
        lastUserAgent = userAgent;
        return next;
    }
};

struct FakeEvaluator {
    std::optional<UpdateOutcome> next;
    int calls = 0;
};

// Owns every fake a test needs; the checker is built on top of them.
struct Harness {
    std::shared_ptr<telaviva::FakeClock> clock = std::make_shared<telaviva::FakeClock>();
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::shared_ptr<FakeEvaluator> evaluator = std::make_shared<FakeEvaluator>();
};

// Jobs queued by the deferred runner; the test decides when each one runs.
struct DeferredJobs {
    std::vector<std::function<void()>> jobs;
};

UpdateOutcome makeOutcome(const std::string& status, const std::string& remoteVersion = "",
                          const std::string& releaseUrl = "")
{
    UpdateOutcome outcome;
    outcome.status = status;
    outcome.remoteVersion = remoteVersion;
    outcome.releaseUrl = releaseUrl;
    return outcome;
}

UpdateEvaluator evaluatorOver(std::shared_ptr<FakeEvaluator> fake)
{
    return [fake](const std::string&, int, const std::string&) {
        ++fake->calls;
        return fake->next;
    };
}

// Runs each job before start() returns.
JobRunner inlineRunner()
{
    return [](std::function<void()> job) { job(); };
}

JobRunner deferredRunner(std::shared_ptr<DeferredJobs> queue)
{
    return [queue](std::function<void()> job) { queue->jobs.push_back(std::move(job)); };
}

// Runs the oldest queued job; false when none is queued.
bool runNext(DeferredJobs& queue)
{
    if (queue.jobs.empty()) {
        return false;
    }
    std::function<void()> job = std::move(queue.jobs.front());
    queue.jobs.erase(queue.jobs.begin());
    job();
    return true;
}

std::unique_ptr<UpdateChecker> makeChecker(const Harness& harness, JobRunner runner)
{
    return std::make_unique<UpdateChecker>("0.1.0", harness.http, evaluatorOver(harness.evaluator), harness.clock,
                                           std::move(runner));
}

// Records the kind the observer sees on every change.
std::shared_ptr<std::vector<UpdateKind>> recordKinds(UpdateChecker& checker)
{
    auto kinds = std::make_shared<std::vector<UpdateKind>>();
    UpdateChecker* self = &checker;
    checker.setOnChanged([kinds, self] { kinds->push_back(self->state().kind); });
    return kinds;
}

UpdateKind kindAt(const std::vector<UpdateKind>& kinds, std::size_t index)
{
    return index < kinds.size() ? kinds[index] : UpdateKind::Idle;
}

// A settled result is reused for 6 hours: not before 6 h minus 1 s, and again after 6 h plus 1 s.
void expectSixHourCache(const UpdateOutcome& result)
{
    Harness h;
    h.evaluator->next = result;
    auto checker = makeChecker(h, inlineRunner());
    checker->checkIfNeeded();
    CHECK_EQ(h.http->calls, 1);

    h.clock->advance(6.0 * 3600.0 - 1.0);
    checker->checkIfNeeded();
    CHECK_EQ(h.http->calls, 1);

    h.clock->advance(2.0);
    checker->checkIfNeeded();
    CHECK_EQ(h.http->calls, 2);
}

// A failure is reused for 30 s, then checked again.
void expectRetryAfterThirtySeconds(Harness& h, UpdateChecker& checker, UpdateKind kind)
{
    CHECK(checker.state().kind == kind);
    const int before = h.http->calls;

    h.clock->advance(29.0);
    checker.checkIfNeeded();
    CHECK_EQ(h.http->calls, before);

    h.clock->advance(2.0);
    checker.checkIfNeeded();
    CHECK_EQ(h.http->calls, before + 1);
}

}  // namespace

TEST(update_checker_idle_at_start)
{
    Harness h;
    auto checker = makeChecker(h, inlineRunner());
    CHECK(checker->state().kind == UpdateKind::Idle);
    CHECK_EQ(h.http->calls, 0);
}

TEST(update_checker_notifies_checking_then_settles)
{
    Harness h;
    h.evaluator->next = makeOutcome("up_to_date", "0.1.0");
    auto checker = makeChecker(h, inlineRunner());
    auto kinds = recordKinds(*checker);

    checker->checkIfNeeded();

    CHECK_EQ(kinds->size(), std::size_t{2});
    CHECK(kindAt(*kinds, 0) == UpdateKind::Checking);
    CHECK(kindAt(*kinds, 1) == UpdateKind::UpToDate);
    CHECK(checker->state().kind == UpdateKind::UpToDate);
    CHECK_EQ(checker->state().remoteVersion, "0.1.0");
    CHECK_EQ(h.http->calls, 1);
}

TEST(update_checker_request_carries_user_agent_and_url)
{
    Harness h;
    auto checker = makeChecker(h, inlineRunner());

    checker->checkIfNeeded();

    CHECK_EQ(h.http->lastUrl, "https://api.github.com/repos/ksfreitas/telaviva-oled/releases/latest");
    CHECK_EQ(h.http->lastUserAgent, "TelaVivaOLED/0.1.0");
}

TEST(update_checker_up_to_date_is_cached_for_six_hours)
{
    expectSixHourCache(makeOutcome("up_to_date", "0.1.0"));
}

TEST(update_checker_update_available_is_cached_for_six_hours)
{
    expectSixHourCache(makeOutcome("update_available", "0.2.0",
                                   "https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0"));
}

TEST(update_checker_no_release_is_cached_for_six_hours)
{
    expectSixHourCache(makeOutcome("no_release"));
}

TEST(update_checker_update_available_keeps_remote_and_url)
{
    Harness h;
    h.evaluator->next = makeOutcome("update_available", "0.2.0",
                                    "https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0");
    auto checker = makeChecker(h, inlineRunner());

    checker->checkIfNeeded();

    const UpdateState state = checker->state();
    CHECK(state.kind == UpdateKind::UpdateAvailable);
    CHECK_EQ(state.remoteVersion, "0.2.0");
    CHECK_EQ(state.releaseUrl, "https://github.com/ksfreitas/telaviva-oled/releases/tag/v0.2.0");
}

TEST(update_checker_failed_is_retried_after_thirty_seconds)
{
    Harness h;
    h.http->next = HttpResult{false, 0, ""};
    auto checker = makeChecker(h, inlineRunner());
    checker->checkIfNeeded();
    expectRetryAfterThirtySeconds(h, *checker, UpdateKind::Failed);
}

TEST(update_checker_rate_limited_is_retried_after_thirty_seconds)
{
    Harness h;
    h.evaluator->next = makeOutcome("rate_limited");
    auto checker = makeChecker(h, inlineRunner());
    checker->checkIfNeeded();
    expectRetryAfterThirtySeconds(h, *checker, UpdateKind::RateLimited);
}

TEST(update_checker_check_now_is_throttled_for_thirty_seconds)
{
    Harness h;
    h.evaluator->next = makeOutcome("up_to_date", "0.1.0");
    auto checker = makeChecker(h, inlineRunner());
    checker->checkIfNeeded();
    CHECK_EQ(h.http->calls, 1);

    checker->checkNow();
    CHECK_EQ(h.http->calls, 1);

    h.clock->advance(29.0);
    checker->checkNow();
    CHECK_EQ(h.http->calls, 1);

    // Works even though the up-to-date result is still fresh.
    h.clock->advance(1.0);
    checker->checkNow();
    CHECK_EQ(h.http->calls, 2);
}

TEST(update_checker_no_second_job_while_pending)
{
    Harness h;
    h.evaluator->next = makeOutcome("up_to_date", "0.1.0");
    auto queue = std::make_shared<DeferredJobs>();
    auto checker = makeChecker(h, deferredRunner(queue));

    checker->checkIfNeeded();
    CHECK(checker->state().kind == UpdateKind::Checking);
    CHECK_EQ(queue->jobs.size(), std::size_t{1});

    checker->checkIfNeeded();
    checker->checkNow();
    CHECK_EQ(queue->jobs.size(), std::size_t{1});

    CHECK(runNext(*queue));
    CHECK_EQ(h.http->calls, 1);
    CHECK(checker->state().kind == UpdateKind::UpToDate);
}

TEST(update_checker_transport_failure_is_failed_without_evaluation)
{
    Harness h;
    h.http->next = HttpResult{false, 200, "{}"};
    h.evaluator->next = makeOutcome("up_to_date", "0.1.0");
    auto checker = makeChecker(h, inlineRunner());

    checker->checkIfNeeded();

    CHECK(checker->state().kind == UpdateKind::Failed);
    CHECK_EQ(h.evaluator->calls, 0);
}

TEST(update_checker_maps_each_outcome_status)
{
    struct StatusCase {
        const char* status;
        UpdateKind kind;
    };
    const StatusCase cases[] = {
        {"update_available", UpdateKind::UpdateAvailable},
        {"up_to_date", UpdateKind::UpToDate},
        {"no_release", UpdateKind::NoRelease},
        {"rate_limited", UpdateKind::RateLimited},
        {"http_error", UpdateKind::Failed},
        {"invalid_response", UpdateKind::Failed},
        {"some_future_status", UpdateKind::Failed},
    };
    for (const StatusCase& testCase : cases) {
        Harness h;
        h.evaluator->next = makeOutcome(testCase.status, "0.2.0", "https://example.invalid/release");
        auto checker = makeChecker(h, inlineRunner());

        checker->checkIfNeeded();

        CHECK(checker->state().kind == testCase.kind);
        CHECK_EQ(h.evaluator->calls, 1);
    }
}

TEST(update_checker_evaluator_nullopt_is_failed)
{
    Harness h;
    auto checker = makeChecker(h, inlineRunner());

    checker->checkIfNeeded();

    CHECK(checker->state().kind == UpdateKind::Failed);
    CHECK_EQ(h.evaluator->calls, 1);
}

TEST(update_checker_destroyed_while_job_pending_is_safe)
{
    Harness h;
    h.evaluator->next = makeOutcome("up_to_date", "0.1.0");
    auto queue = std::make_shared<DeferredJobs>();
    // Captures only a counter, so the observer stays harmless after the checker is gone.
    auto notified = std::make_shared<int>(0);
    auto checker = makeChecker(h, deferredRunner(queue));
    checker->setOnChanged([notified] { ++*notified; });
    checker->checkIfNeeded();

    checker.reset();

    CHECK(runNext(*queue));
    CHECK_EQ(h.http->calls, 1);
    CHECK_EQ(h.evaluator->calls, 1);
}

TEST(core_update_version_is_non_empty_with_dot)
{
    const std::string version = telaviva::CoreBridge::coreVersion();
    CHECK(!version.empty());
    CHECK(version.find('.') != std::string::npos);
}

TEST(core_update_evaluates_404_as_no_release)
{
    const std::optional<UpdateOutcome> outcome = telaviva::CoreBridge::evaluateUpdate("0.1.0", 404, "");
    CHECK(outcome.has_value());
    if (outcome) {
        CHECK_EQ(outcome->status, "no_release");
    }
}

TEST(core_update_evaluates_newer_release)
{
    const std::optional<UpdateOutcome> outcome =
        telaviva::CoreBridge::evaluateUpdate("0.1.0", 200, R"({"tag_name":"v999.0.0"})");
    CHECK(outcome.has_value());
    if (outcome) {
        CHECK_EQ(outcome->status, "update_available");
        CHECK_EQ(outcome->remoteVersion, "999.0.0");
        CHECK_EQ(outcome->releaseUrl, "https://github.com/ksfreitas/telaviva-oled/releases/tag/v999.0.0");
    }
}

TEST(core_update_rejects_malformed_installed_version)
{
    const std::optional<UpdateOutcome> outcome = telaviva::CoreBridge::evaluateUpdate("not-a-version", 200, "{}");
    CHECK(!outcome.has_value());
}
