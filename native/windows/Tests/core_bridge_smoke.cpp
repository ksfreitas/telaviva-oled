// Smoke tests that drive the real Rust core DLL through CoreBridge with a temp config.
#include "TestHarness.h"
#include "Platform/CoreBridge.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

namespace {

// Values from include/telaviva_core.h, repeated here to keep the test free of C headers.
constexpr int kOk = 0;
constexpr int kClosed = 3;  // TV_CLOSED

// Each test gets its own config directory so persisted state never leaks between tests.
// Declared before the bridge in each test so the bridge is destroyed first.
struct TempConfigDir {
    std::filesystem::path root;

    TempConfigDir()
        : root(std::filesystem::temp_directory_path() /
               ("telaviva-core-smoke-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(root);
    }

    ~TempConfigDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    telaviva::CoreBridgeOptions bridgeOptions() const
    {
        telaviva::CoreBridgeOptions options;
        options.configPath = (root / L"config.json").wstring();
        options.osLocale = "en-US";
        return options;
    }
};

}  // namespace

TEST(core_bridge_abi_version_is_1)
{
    CHECK_EQ(telaviva::CoreBridge::abiVersion(), uint32_t{1});
}

TEST(core_bridge_opens_and_reads_snapshot)
{
    TempConfigDir dir;
    telaviva::CoreBridge bridge(dir.bridgeOptions());
    CHECK(bridge.isOpen());

    const telaviva::Snapshot snapshot = bridge.snapshot();
    CHECK(snapshot.revision >= 1);
    CHECK(snapshot.monitors.empty());
}

TEST(core_bridge_power_command_advances_revision)
{
    TempConfigDir dir;
    telaviva::CoreBridge bridge(dir.bridgeOptions());
    const uint64_t before = bridge.snapshot().revision;

    CHECK_EQ(bridge.submit(R"({"type":"power","on_battery":false})"), kOk);

    const telaviva::Snapshot after = bridge.snapshot();
    CHECK(after.power.onBattery.has_value() && !*after.power.onBattery);
    CHECK(after.revision > before);
}

TEST(core_bridge_sink_receives_snapshot_after_submit)
{
    // Shared with the sink so a late callback cannot touch a destroyed test frame.
    struct SinkState {
        std::mutex mutex;
        std::condition_variable changed;
        int received = 0;
    };

    TempConfigDir dir;
    auto state = std::make_shared<SinkState>();
    telaviva::CoreBridge bridge(dir.bridgeOptions());
    // Runs on the bridge worker thread after each accepted revision.
    bridge.addSink([state](const telaviva::Snapshot&) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->received;
        }
        state->changed.notify_all();
    });

    CHECK_EQ(bridge.submit(R"({"type":"power","on_battery":true})"), kOk);

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool received = state->changed.wait_for(lock, std::chrono::seconds(2), [&state] {
        return state->received > 0;
    });
    CHECK(received);
}

TEST(core_bridge_invalid_json_returns_error_without_crash)
{
    TempConfigDir dir;
    telaviva::CoreBridge bridge(dir.bridgeOptions());

    CHECK(bridge.submit("{not json") != kOk);
    // The bridge must stay usable after a rejected command.
    CHECK(bridge.isOpen());
    CHECK(bridge.snapshot().revision >= 1);
}

TEST(core_bridge_submit_after_shutdown_returns_closed)
{
    TempConfigDir dir;
    telaviva::CoreBridge bridge(dir.bridgeOptions());

    bridge.shutdown(std::chrono::milliseconds(2000));

    CHECK_EQ(bridge.submit(R"({"type":"power","on_battery":false})"), kClosed);
}
