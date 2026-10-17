#include "TestHarness.h"

#include "../Platform/EfficiencyController.h"
#include "../Platform/IEfficiency.h"

#include <string>
#include <vector>

using telaviva::EfficiencyController;
using telaviva::EfficiencyThreadId;
using telaviva::IEfficiency;
using telaviva::ProcessEfficiencyReading;

namespace {

constexpr EfficiencyThreadId kUiThread = 100;
constexpr EfficiencyThreadId kHookThread = 200;

struct ThreadCall {
    EfficiencyThreadId thread;
    bool exempt;
};

class FakeEfficiency final : public IEfficiency {
public:
    ProcessEfficiencyReading currentProcessState() override
    {
        ++queries;
        return reading;
    }

    bool setProcessEcoQoS(bool enabled) override
    {
        processCalls.push_back(enabled);
        return processOk;
    }

    bool setThreadEcoQoS(EfficiencyThreadId thread, bool exempt) override
    {
        threadCalls.push_back(ThreadCall{thread, exempt});
        return threadOk;
    }

    // Default: readable, not idle, nothing forced by the user.
    ProcessEfficiencyReading reading{true, false, false, false};
    bool processOk = true;
    bool threadOk = true;
    int queries = 0;
    std::vector<bool> processCalls;
    std::vector<ThreadCall> threadCalls;
};

}  // namespace

TEST(efficiency_startup_requests_process_ecoqos_once)
{
    FakeEfficiency fake;
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    CHECK_EQ(fake.processCalls.size(), std::size_t{1});
    if (fake.processCalls.size() == 1) {
        CHECK(fake.processCalls[0]);
    }
    CHECK(controller.state() == telaviva::EfficiencyState::EcoQoSRequested);
    CHECK_EQ(controller.stateString(), std::string("efficiency: ecoqos-requested"));
}

TEST(efficiency_startup_twice_never_reasserts_process_level)
{
    FakeEfficiency fake;
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    controller.onWindowVisibilityChanged(0);
    controller.registerExemptThread(kHookThread);
    controller.onWindowVisibilityChanged(2);
    controller.onStartup();
    CHECK_EQ(fake.processCalls.size(), std::size_t{1});
    CHECK_EQ(fake.queries, 1);
}

TEST(efficiency_forced_efficiency_mode_sets_nothing)
{
    FakeEfficiency fake;
    fake.reading = ProcessEfficiencyReading{true, false, true, true};
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    controller.onWindowVisibilityChanged(0);
    controller.registerExemptThread(kHookThread);
    CHECK(fake.processCalls.empty());
    CHECK(fake.threadCalls.empty());
    CHECK(controller.state() == telaviva::EfficiencyState::UserManaged);
    CHECK_EQ(controller.stateString(), std::string("efficiency: user-managed"));
}

TEST(efficiency_idle_priority_class_is_user_managed)
{
    FakeEfficiency fake;
    fake.reading = ProcessEfficiencyReading{true, true, false, false};
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    CHECK(fake.processCalls.empty());
    CHECK_EQ(controller.stateString(), std::string("efficiency: user-managed"));
}

TEST(efficiency_unreadable_state_is_unsupported_and_silent)
{
    FakeEfficiency fake;
    fake.reading = ProcessEfficiencyReading{false, false, false, false};
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    CHECK(fake.processCalls.empty());
    CHECK(fake.threadCalls.empty());
    CHECK_EQ(controller.stateString(), std::string("efficiency: unsupported"));
}

TEST(efficiency_failed_process_request_is_unsupported)
{
    FakeEfficiency fake;
    fake.processOk = false;
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    controller.registerExemptThread(kHookThread);
    CHECK_EQ(fake.processCalls.size(), std::size_t{1});
    CHECK(fake.threadCalls.empty());
    CHECK_EQ(controller.stateString(), std::string("efficiency: unsupported"));
}

TEST(efficiency_ui_exemption_follows_window_visibility)
{
    FakeEfficiency fake;
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    controller.onWindowVisibilityChanged(2);
    controller.onWindowVisibilityChanged(0);
    controller.onWindowVisibilityChanged(0);
    CHECK_EQ(fake.threadCalls.size(), std::size_t{2});
    if (fake.threadCalls.size() == 2) {
        CHECK_EQ(fake.threadCalls[0].thread, kUiThread);
        CHECK(fake.threadCalls[0].exempt);
        CHECK_EQ(fake.threadCalls[1].thread, kUiThread);
        CHECK(!fake.threadCalls[1].exempt);
    }
    CHECK_EQ(controller.stateString(), std::string("efficiency: ecoqos-requested"));
}

TEST(efficiency_ui_exemption_never_applies_in_user_managed_state)
{
    FakeEfficiency fake;
    fake.reading = ProcessEfficiencyReading{true, false, true, false};
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    controller.onWindowVisibilityChanged(0);
    CHECK(fake.threadCalls.empty());
}

TEST(efficiency_ui_exemption_never_applies_when_unsupported)
{
    FakeEfficiency fake;
    fake.processOk = false;
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.onWindowVisibilityChanged(1);
    CHECK(fake.threadCalls.empty());
}

TEST(efficiency_hook_thread_exempt_when_it_registers)
{
    FakeEfficiency fake;
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.registerExemptThread(kHookThread);
    CHECK_EQ(fake.threadCalls.size(), std::size_t{1});
    if (fake.threadCalls.size() == 1) {
        CHECK_EQ(fake.threadCalls[0].thread, kHookThread);
        CHECK(fake.threadCalls[0].exempt);
    }
    CHECK_EQ(fake.processCalls.size(), std::size_t{1});
}

TEST(efficiency_hook_thread_not_exempt_in_user_managed_state)
{
    FakeEfficiency fake;
    fake.reading = ProcessEfficiencyReading{false, true, false, false};
    EfficiencyController controller(fake, kUiThread);
    controller.onStartup();
    controller.registerExemptThread(kHookThread);
    CHECK(fake.threadCalls.empty());
}

TEST(efficiency_events_before_startup_do_nothing_until_started)
{
    FakeEfficiency fake;
    EfficiencyController controller(fake, kUiThread);
    controller.onWindowVisibilityChanged(1);
    controller.registerExemptThread(kHookThread);
    CHECK(fake.threadCalls.empty());
    CHECK(fake.processCalls.empty());
    CHECK_EQ(controller.stateString(), std::string("efficiency: not-started"));

    // A window already visible at startup gets its exemption right away.
    controller.onStartup();
    CHECK_EQ(fake.processCalls.size(), std::size_t{1});
    CHECK_EQ(fake.threadCalls.size(), std::size_t{1});
    if (fake.threadCalls.size() == 1) {
        CHECK_EQ(fake.threadCalls[0].thread, kUiThread);
        CHECK(fake.threadCalls[0].exempt);
    }
}
