#include "TestHarness.h"

#include "Platform/LaunchAtLogin.h"

#include <optional>
#include <string>

using telaviva::ILaunchRegistry;
using telaviva::LaunchAtLogin;
using telaviva::LaunchResult;

namespace {

const std::wstring kExePath = L"C:\\TelaViva\\telaviva.exe";
const std::wstring kExpected = L"\"C:\\TelaViva\\telaviva.exe\" --background";
const std::wstring kOldValue = L"\"C:\\Old\\telaviva.exe\" --background";
const std::wstring kOtherValue = L"\"C:\\Other\\app.exe\" --background";

// Scriptable registry. Failures are set per test; corruptNextWrite stores a different
// command once, so the read-back differs while the restore write still succeeds.
class FakeRegistry : public ILaunchRegistry {
public:
    std::optional<std::wstring> stored;
    bool failWrite = false;
    bool failRemove = false;
    bool corruptNextWrite = false;
    int writes = 0;
    int removes = 0;

    std::optional<std::wstring> read() override { return stored; }

    bool write(const std::wstring& value, std::wstring& error) override
    {
        ++writes;
        if (failWrite) {
            error = L"write denied";
            return false;
        }
        stored = corruptNextWrite ? kOtherValue : value;
        corruptNextWrite = false;
        return true;
    }

    bool remove(std::wstring& error) override
    {
        ++removes;
        if (failRemove) {
            error = L"remove denied";
            return false;
        }
        stored.reset();
        return true;
    }
};

bool storedIs(const FakeRegistry& registry, const std::wstring& value)
{
    return registry.stored.has_value() && *registry.stored == value;
}

}  // namespace

TEST(launch_at_login_enable_success)
{
    FakeRegistry registry;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(true);
    CHECK(result.ok);
    CHECK(result.enabled);
    CHECK(result.error.empty());
    CHECK(storedIs(registry, kExpected));
    CHECK(launch.isEnabled());
}

TEST(launch_at_login_enable_write_failure_keeps_previous_state)
{
    FakeRegistry registry;
    registry.stored = kOldValue;
    registry.failWrite = true;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(true);
    CHECK(!result.ok);
    CHECK(!result.enabled);
    CHECK(result.error == L"write denied");
    CHECK(storedIs(registry, kOldValue));
    // Nothing changed, so no restore write is attempted.
    CHECK(registry.writes == 1);
}

TEST(launch_at_login_enable_write_failure_without_previous_stays_absent)
{
    FakeRegistry registry;
    registry.failWrite = true;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(true);
    CHECK(!result.ok);
    CHECK(!result.enabled);
    CHECK(result.error == L"write denied");
    CHECK(!registry.stored.has_value());
}

TEST(launch_at_login_enable_readback_mismatch_restores_previous)
{
    FakeRegistry registry;
    registry.stored = kOldValue;
    registry.corruptNextWrite = true;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(true);
    CHECK(!result.ok);
    CHECK(!result.enabled);
    CHECK(!result.error.empty());
    CHECK(storedIs(registry, kOldValue));
    CHECK(!launch.isEnabled());
}

TEST(launch_at_login_enable_readback_mismatch_without_previous_removes_value)
{
    FakeRegistry registry;
    registry.corruptNextWrite = true;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(true);
    CHECK(!result.ok);
    CHECK(!result.enabled);
    CHECK(!result.error.empty());
    CHECK(!registry.stored.has_value());
    CHECK(registry.removes == 1);
}

TEST(launch_at_login_disable_success)
{
    FakeRegistry registry;
    registry.stored = kExpected;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(false);
    CHECK(result.ok);
    CHECK(!result.enabled);
    CHECK(!registry.stored.has_value());
    CHECK(!launch.isEnabled());
}

TEST(launch_at_login_disable_when_absent_is_ok)
{
    FakeRegistry registry;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(false);
    CHECK(result.ok);
    CHECK(!result.enabled);
    CHECK(result.error.empty());
}

TEST(launch_at_login_disable_failure_reports_verified_state)
{
    FakeRegistry registry;
    registry.stored = kExpected;
    registry.failRemove = true;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult result = launch.setEnabled(false);
    CHECK(!result.ok);
    // The value is still there, so the verified state is still enabled.
    CHECK(result.enabled);
    CHECK(result.error == L"remove denied");
    CHECK(storedIs(registry, kExpected));
}

TEST(launch_at_login_expected_command_quotes_path_with_spaces)
{
    CHECK(LaunchAtLogin::expectedCommand(L"C:\\Program Files\\TelaViva\\telaviva.exe") ==
          L"\"C:\\Program Files\\TelaViva\\telaviva.exe\" --background");
    CHECK(LaunchAtLogin::expectedCommand(kExePath) == kExpected);
}

TEST(launch_at_login_is_enabled_false_for_other_command)
{
    FakeRegistry registry;
    registry.stored = kOtherValue;
    LaunchAtLogin launch(registry, kExePath);

    CHECK(!launch.isEnabled());
    // A stale entry from another install path is not treated as ours.
    registry.stored = kOldValue;
    CHECK(!launch.isEnabled());
}

TEST(launch_at_login_enable_twice_is_idempotent)
{
    FakeRegistry registry;
    LaunchAtLogin launch(registry, kExePath);

    const LaunchResult first = launch.setEnabled(true);
    const LaunchResult second = launch.setEnabled(true);
    CHECK(first.ok);
    CHECK(second.ok);
    CHECK(second.enabled);
    CHECK(storedIs(registry, kExpected));
    CHECK(registry.writes == 2);
}
