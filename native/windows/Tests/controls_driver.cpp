#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/ControlsDriver.h"
#include "Platform/IAttemptRunner.h"
#include "Platform/JsonCodec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using telaviva::AttemptOutcome;
using telaviva::AttemptRequest;
using telaviva::ControlsDriver;
using telaviva::DraftView;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::IAttemptRunner;
using telaviva::ICoreSubmitter;
using telaviva::IControlsApi;
using telaviva::IDdcExecutor;
using telaviva::Snapshot;

namespace {

// Steady-clock seconds at the start of each test. Deadlines are chosen as exact binary
// fractions of it, so the millisecond delays are exact.
constexpr double kStartSteady = 100.0;
constexpr uint64_t kEpochMs = 1781000000000ULL;

const char* const kEmptyReply = R"json({"value":null,"effects":[],"next_deadline":null})json";

// One submit_command effect; command_json is a JSON string inside the reply.
const char* const kCommandReply =
    R"json({"value":null,"effects":[{"type":"submit_command","token":7,"command_json":"{\"type\":\"set_brightness\",\"monitor_id\":\"mon-1\"}"}],"next_deadline":null})json";

// One DDC read attempt for mon-1 with token 3.
const char* const kRunAttemptReply =
    R"json({"value":null,"effects":[{"type":"run_attempt","token":3,"monitor_id":"mon-1","op":"read","code":16}],"next_deadline":null})json";

// A draft_changed effect for mon-1 brightness.
const char* const kDraftChangedReply =
    R"json({"value":null,"effects":[{"type":"draft_changed","monitor_id":"mon-1","feature":"brightness"}],"next_deadline":null})json";

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

// The "type" member of an event, or an empty string.
std::string eventType(const std::string& event)
{
    const std::string key = "\"type\":\"";
    const std::size_t at = event.find(key);
    if (at == std::string::npos) {
        return std::string();
    }
    const std::size_t start = at + key.size();
    const std::size_t end = event.find('"', start);
    return end == std::string::npos ? std::string() : event.substr(start, end - start);
}

// A reply with no effects and the given absolute deadline.
std::string deadlineReply(double deadline)
{
    return R"json({"value":null,"effects":[],"next_deadline":)json" + std::to_string(deadline) + "}";
}

class FakeControlsApi final : public IControlsApi {
public:
    std::string step(const std::string& eventJson) override
    {
        events.push_back(eventJson);
        if (!queued.empty()) {
            const std::string reply = queued.front();
            queued.erase(queued.begin());
            return reply;
        }
        const auto found = byType.find(eventType(eventJson));
        return found != byType.end() ? found->second : std::string(kEmptyReply);
    }

    // Replies returned to the next calls, in order, before byType is consulted.
    std::vector<std::string> queued;
    // Persistent reply per event type.
    std::map<std::string, std::string> byType;
    std::vector<std::string> events;
};

// Records every submitted command, in order. reentrant runs inside submit, before it returns.
class FakeSubmitter final : public ICoreSubmitter {
public:
    int submit(const std::string& json) override
    {
        submitted.push_back(json);
        if (reentrant) {
            reentrant();
        }
        return status;
    }

    std::vector<std::string> submitted;
    int status = 0;
    std::function<void()> reentrant;
};

// Runs posted work inline, or keeps it until runPending.
class FakeExecutor final : public IDdcExecutor {
public:
    void post(std::function<void()> work) override
    {
        if (inlineRun) {
            work();
        } else {
            tasks.push_back(std::move(work));
        }
    }

    // Runs the tasks queued so far. Tasks posted while running wait for the next call.
    void runPending()
    {
        std::vector<std::function<void()>> pending = std::move(tasks);
        tasks.clear();
        for (const std::function<void()>& task : pending) {
            task();
        }
    }

    bool inlineRun = true;
    std::vector<std::function<void()>> tasks;
};

class FakeRunner final : public IAttemptRunner {
public:
    AttemptOutcome run(const AttemptRequest& request) override
    {
        requests.push_back(request);
        return outcome;
    }

    std::vector<AttemptRequest> requests;
    AttemptOutcome outcome{};
};

int countEvents(const FakeControlsApi& api, const std::string& type)
{
    int count = 0;
    for (const std::string& event : api.events) {
        if (eventType(event) == type) {
            ++count;
        }
    }
    return count;
}

// The most recent event of the given type, or an empty string.
std::string lastEventOfType(const FakeControlsApi& api, const std::string& type)
{
    for (auto it = api.events.rbegin(); it != api.events.rend(); ++it) {
        if (eventType(*it) == type) {
            return *it;
        }
    }
    return std::string();
}

// The driver under test, wired to the fakes.
struct Rig {
    FakeControlsApi api;
    FakeSubmitter submitter;
    FakeRunner runner;
    FakeExecutor executor;
    FakeScheduler scheduler;
    FakeClock clock;
    ControlsDriver driver;

    Rig() : driver(api, submitter, runner, executor, scheduler, clock)
    {
        clock.setSteadySeconds(kStartSteady);
        clock.setEpochMillis(kEpochMs);
    }
};

// Moves the clock and the scheduler together, so each timer runs at its due clock time.
void advanceBoth(Rig& rig, int ms)
{
    rig.clock.advance(static_cast<double>(ms) / 1000.0);
    rig.scheduler.advance(std::chrono::milliseconds(ms));
}

Snapshot emptySnapshot()
{
    Snapshot snapshot;
    return snapshot;
}

}  // namespace

TEST(controls_driver_slider_returns_the_reply_draft)
{
    Rig rig;
    rig.api.queued.push_back(
        R"json({"value":{"draft":42.5,"status":"pending"},"effects":[],"next_deadline":null})json");
    const std::optional<DraftView> draft = rig.driver.slider("mon-1", "brightness", 42.5);

    CHECK(draft.has_value());
    if (draft) {
        CHECK_EQ(draft->draft, 42.5);
        CHECK_EQ(draft->status, std::string("pending"));
    }
    CHECK_EQ(countEvents(rig.api, "slider"), 1);
}

TEST(controls_driver_observe_sends_an_observe_event)
{
    Rig rig;
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(rig.api.events.size(), std::size_t{1});
    CHECK_EQ(countEvents(rig.api, "observe"), 1);
}

TEST(controls_driver_submit_command_is_passed_verbatim_and_reports_submitted)
{
    Rig rig;
    rig.api.queued.push_back(kCommandReply);
    rig.driver.slider("mon-1", "brightness", 40.0);

    CHECK_EQ(rig.submitter.submitted.size(), std::size_t{1});
    CHECK(!rig.submitter.submitted.empty() &&
          rig.submitter.submitted[0] == R"json({"type":"set_brightness","monitor_id":"mon-1"})json");
    CHECK_EQ(countEvents(rig.api, "submitted"), 1);
    CHECK(contains(lastEventOfType(rig.api, "submitted"), "\"accepted\":true"));
}

TEST(controls_driver_rejected_submission_reports_accepted_false)
{
    Rig rig;
    rig.submitter.status = 1;
    rig.api.queued.push_back(kCommandReply);
    rig.driver.slider("mon-1", "brightness", 40.0);

    CHECK_EQ(rig.submitter.submitted.size(), std::size_t{1});
    CHECK_EQ(countEvents(rig.api, "submitted"), 1);
    CHECK(contains(lastEventOfType(rig.api, "submitted"), "\"accepted\":false"));
}

TEST(controls_driver_run_attempt_runs_the_runner_and_reports_done)
{
    Rig rig;
    rig.runner.outcome.ok = true;
    rig.runner.outcome.value = 40.0;
    rig.api.queued.push_back(kRunAttemptReply);
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(rig.runner.requests.size(), std::size_t{1});
    if (!rig.runner.requests.empty()) {
        CHECK_EQ(rig.runner.requests[0].monitorId, std::string("mon-1"));
        CHECK(!rig.runner.requests[0].write);
        CHECK_EQ(static_cast<int>(rig.runner.requests[0].code), 16);
    }
    CHECK_EQ(countEvents(rig.api, "attempt_done"), 1);
    CHECK(contains(lastEventOfType(rig.api, "attempt_done"), "\"ok\":true"));
}

TEST(controls_driver_failed_attempt_with_empty_error_reports_io)
{
    Rig rig;
    rig.runner.outcome.ok = false;
    rig.runner.outcome.error = "";
    rig.api.queued.push_back(kRunAttemptReply);
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(rig.runner.requests.size(), std::size_t{1});
    CHECK_EQ(countEvents(rig.api, "attempt_done"), 1);
    CHECK(contains(lastEventOfType(rig.api, "attempt_done"), "\"ok\":false"));
    CHECK(contains(lastEventOfType(rig.api, "attempt_done"), "\"error\":\"io\""));
}

TEST(controls_driver_run_attempt_waits_for_the_executor)
{
    Rig rig;
    rig.executor.inlineRun = false;
    rig.runner.outcome.ok = true;
    rig.api.queued.push_back(kRunAttemptReply);
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(rig.runner.requests.size(), std::size_t{0});
    CHECK_EQ(rig.executor.tasks.size(), std::size_t{1});

    rig.executor.runPending();
    CHECK_EQ(rig.runner.requests.size(), std::size_t{1});
    CHECK_EQ(countEvents(rig.api, "attempt_done"), 1);
}

TEST(controls_driver_next_deadline_arms_one_timer_and_fires_a_tick)
{
    Rig rig;
    rig.api.queued.push_back(deadlineReply(100.5));
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});
    advanceBoth(rig, 499);
    CHECK_EQ(countEvents(rig.api, "tick"), 0);

    advanceBoth(rig, 1);
    CHECK_EQ(countEvents(rig.api, "tick"), 1);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
}

TEST(controls_driver_changed_deadline_replaces_the_timer_and_equal_deadline_does_not_duplicate)
{
    Rig rig;
    rig.api.queued.push_back(deadlineReply(100.5));
    rig.driver.observe(emptySnapshot());
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});

    // The same deadline arms nothing new.
    rig.api.queued.push_back(deadlineReply(100.5));
    rig.driver.observe(emptySnapshot());
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});

    // A later deadline cancels the old timer and arms one for 1000 ms.
    rig.api.queued.push_back(deadlineReply(101.0));
    rig.driver.observe(emptySnapshot());
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});

    // The cancelled 500 ms timer must not fire.
    advanceBoth(rig, 500);
    CHECK_EQ(countEvents(rig.api, "tick"), 0);

    advanceBoth(rig, 500);
    CHECK_EQ(countEvents(rig.api, "tick"), 1);
}

TEST(controls_driver_reply_without_deadline_cancels_the_armed_timer)
{
    Rig rig;
    rig.api.queued.push_back(deadlineReply(100.5));
    rig.driver.observe(emptySnapshot());
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});

    rig.driver.observe(emptySnapshot());
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});

    advanceBoth(rig, 1000);
    CHECK_EQ(countEvents(rig.api, "tick"), 0);
}

TEST(controls_driver_tick_reply_can_arm_the_next_deadline)
{
    Rig rig;
    rig.api.queued.push_back(deadlineReply(100.5));
    rig.driver.observe(emptySnapshot());

    // The first tick answers with a later deadline, which the driver arms from inside the timer.
    rig.api.queued.push_back(deadlineReply(101.0));
    advanceBoth(rig, 500);
    CHECK_EQ(countEvents(rig.api, "tick"), 1);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});

    advanceBoth(rig, 500);
    CHECK_EQ(countEvents(rig.api, "tick"), 2);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
}

TEST(controls_driver_draft_changed_calls_the_observer)
{
    Rig rig;
    std::vector<std::pair<std::string, std::string>> calls;
    rig.driver.setDraftObserver([&calls](const std::string& monitorId, const std::string& feature) {
        calls.emplace_back(monitorId, feature);
    });
    rig.api.queued.push_back(kDraftChangedReply);
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(calls.size(), std::size_t{1});
    if (!calls.empty()) {
        CHECK_EQ(calls[0].first, std::string("mon-1"));
        CHECK_EQ(calls[0].second, std::string("brightness"));
    }
}

TEST(controls_driver_key_returns_the_core_consume)
{
    Rig rig;
    rig.api.queued.push_back(R"json({"value":{"consume":true},"effects":[],"next_deadline":null})json");
    CHECK(rig.driver.key("volume_up", true, false, false, std::nullopt));

    // The default reply carries no consume decision, which reads as false.
    CHECK(!rig.driver.key("volume_up", false, false, false, std::nullopt));
}

TEST(controls_driver_shutdown_stops_everything)
{
    Rig rig;
    rig.executor.inlineRun = false;
    rig.api.queued.push_back(
        R"json({"value":null,"effects":[{"type":"run_attempt","token":4,"monitor_id":"mon-1","op":"read","code":16}],"next_deadline":100.5})json");
    rig.driver.observe(emptySnapshot());
    CHECK_EQ(rig.executor.tasks.size(), std::size_t{1});
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{1});

    rig.driver.shutdown();
    CHECK_EQ(countEvents(rig.api, "shutdown"), 1);
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});

    // A late executor task does nothing: no runner call and no attempt_done.
    rig.executor.runPending();
    CHECK_EQ(rig.runner.requests.size(), std::size_t{0});
    CHECK_EQ(countEvents(rig.api, "attempt_done"), 0);

    // No tick after shutdown.
    advanceBoth(rig, 1000);
    CHECK_EQ(countEvents(rig.api, "tick"), 0);

    // Later calls return defaults and send nothing; a second shutdown is a no-op.
    const std::size_t before = rig.api.events.size();
    CHECK(!rig.driver.slider("mon-1", "brightness", 10.0).has_value());
    CHECK(!rig.driver.key("mute", true, false, false, std::nullopt));
    rig.driver.shutdown();
    CHECK_EQ(rig.api.events.size(), before);
    CHECK_EQ(countEvents(rig.api, "shutdown"), 1);
}

TEST(controls_driver_invalid_or_empty_reply_is_ignored)
{
    Rig rig;
    rig.api.queued.push_back("");
    rig.api.queued.push_back("not json");
    rig.api.queued.push_back(R"json({"value":null,"effects":[],"next_deadline":)json");
    rig.driver.observe(emptySnapshot());
    rig.driver.observe(emptySnapshot());
    rig.driver.observe(emptySnapshot());

    CHECK_EQ(rig.submitter.submitted.size(), std::size_t{0});
    CHECK_EQ(rig.scheduler.pendingCount(), std::size_t{0});
    CHECK_EQ(countEvents(rig.api, "observe"), 3);
}

TEST(controls_driver_queries_draft_without_deadlock_when_the_submitter_reenters)
{
    Rig rig;
    bool reentered = false;
    rig.submitter.reentrant = [&rig, &reentered] {
        rig.driver.queryDraft("mon-1", "brightness");
        reentered = true;
    };
    rig.api.queued.push_back(kCommandReply);
    rig.driver.slider("mon-1", "brightness", 40.0);

    CHECK(reentered);
    CHECK_EQ(countEvents(rig.api, "query_draft"), 1);
    CHECK_EQ(countEvents(rig.api, "submitted"), 1);
}

TEST(controls_driver_effect_chain_is_bounded)
{
    Rig rig;
    // Every submitted answer asks for another submit, which would never end without the bound.
    rig.api.byType["submitted"] = kCommandReply;
    rig.api.queued.push_back(kCommandReply);
    rig.driver.slider("mon-1", "brightness", 40.0);

    CHECK(rig.submitter.submitted.size() >= std::size_t{1});
    CHECK(rig.submitter.submitted.size() <= std::size_t{65});
}
