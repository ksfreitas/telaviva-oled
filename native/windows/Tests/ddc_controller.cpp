#include "TestHarness.h"

#include "Platform/DdcController.h"
#include "Platform/DisplayCatalog.h"
#include "Platform/IAttemptRunner.h"
#include "Platform/IDdcPort.h"
#include "Platform/ISleeper.h"
#include "Platform/JsonCodec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using telaviva::AttemptOutcome;
using telaviva::AttemptRequest;
using telaviva::CatalogEntry;
using telaviva::DdcController;
using telaviva::DdcError;
using telaviva::DdcReading;
using telaviva::ICoreSubmitter;
using telaviva::IDdcExecutor;
using telaviva::IDdcListener;
using telaviva::IDdcPort;
using telaviva::IDdcPortProvider;
using telaviva::IDisplayCatalog;
using telaviva::ISleeper;
using telaviva::MonitorInfo;
using telaviva::MonitorSnapshot;
using telaviva::Snapshot;

namespace {

constexpr uint8_t kBrightness = 0x10;
constexpr uint8_t kVolume = 0x62;
const char* const kIdA = "win-DEL-1234-99-abcdef12";
const char* const kIdB = "win-DEL-5678-42-0badc0de";
const char* const kInternalId = "win-BOE-0-0-00000000";
const char* const kPath = "\\\\?\\DISPLAY#DEL0001#5&1a2b3c&0&UID1#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
const char* const kName = "DELL U2720Q";

// Catalog returns whatever the test sets; enumeration has no side effects.
class FakeCatalog final : public IDisplayCatalog {
public:
    std::vector<CatalogEntry> entries;

    std::vector<CatalogEntry> enumerate() override { return entries; }
};

// Per-VCP-code device model shared between the test and the port handed to the controller.
struct Channel {
    bool readable = true;     // false: reads fail with Unsupported (no retry)
    bool acceptWrites = true; // false: writes are dropped, so a readback would mismatch
    std::optional<DdcError> writeError;  // set: every write fails with this error
    uint32_t value = 0;
    uint32_t maximum = 100;
};

struct PortState {
    std::map<uint8_t, Channel> channels;
    std::vector<std::string> calls;  // "read 16", "write 16=58"
};

class FakePort final : public IDdcPort {
public:
    explicit FakePort(std::shared_ptr<PortState> state) : state_(std::move(state)) {}

    bool readVcp(uint8_t code, DdcReading& out, DdcError& error) override
    {
        state_->calls.push_back("read " + std::to_string(code));
        const Channel& channel = state_->channels[code];
        if (!channel.readable) {
            error = DdcError::Unsupported;
            return false;
        }
        out.current = channel.value;
        out.maximum = channel.maximum;
        error = DdcError::None;
        return true;
    }

    bool writeVcp(uint8_t code, uint32_t value, DdcError& error) override
    {
        state_->calls.push_back("write " + std::to_string(code) + "=" + std::to_string(value));
        Channel& channel = state_->channels[code];
        if (channel.writeError) {
            error = *channel.writeError;
            return false;
        }
        if (channel.acceptWrites) {
            channel.value = value;
        }
        error = DdcError::None;
        return true;
    }

private:
    std::shared_ptr<PortState> state_;
};

class FakePortProvider final : public IDdcPortProvider {
public:
    std::map<std::string, std::shared_ptr<PortState>> states;
    std::set<std::string> withoutHandle;  // ids for which open() returns nullptr
    std::vector<std::string> opened;

    std::unique_ptr<IDdcPort> open(const CatalogEntry& entry) override
    {
        opened.push_back(entry.identity.id);
        if (withoutHandle.count(entry.identity.id) != 0) {
            return nullptr;
        }
        return std::make_unique<FakePort>(state(entry.identity.id));
    }

    std::shared_ptr<PortState> state(const std::string& id)
    {
        std::shared_ptr<PortState>& slot = states[id];
        if (!slot) {
            slot = std::make_shared<PortState>();
        }
        return slot;
    }

    std::vector<std::string> callsFor(const std::string& id) { return state(id)->calls; }
};

class FakeSubmitter final : public ICoreSubmitter {
public:
    std::vector<std::string> sent;

    int submit(const std::string& json) override
    {
        sent.push_back(json);
        return 0;
    }
};

class RecordingListener final : public IDdcListener {
public:
    std::vector<std::pair<std::string, std::string>> profiles;  // (monitorId, manufacturer)
    std::vector<std::string> removed;

    void onMonitorProfile(const std::string& monitorId, const std::string& manufacturer) override
    {
        profiles.emplace_back(monitorId, manufacturer);
    }

    void onMonitorRemoved(const std::string& monitorId) override { removed.push_back(monitorId); }
};

// Runs each job inline, so the whole flow is deterministic on the test thread.
class InlineExecutor final : public IDdcExecutor {
public:
    void post(std::function<void()> work) override { work(); }
};

class FakeSleeper final : public ISleeper {
public:
    int calls = 0;

    void sleepFor(std::chrono::milliseconds) override { ++calls; }
};

// Declaration order matters: the controller keeps references to every member above it.
struct Rig {
    FakeCatalog catalog;
    FakePortProvider ports;
    FakeSubmitter submitter;
    InlineExecutor executor;
    FakeSleeper sleeper;
    RecordingListener listener;
    DdcController controller{catalog, ports, submitter, executor, sleeper, &listener};
};

CatalogEntry makeEntry(const std::string& id, bool stable, const std::string& name, bool internal = false)
{
    CatalogEntry entry;
    entry.identity.id = id;
    entry.identity.stable = stable;
    entry.identity.name = name;
    entry.manufacturer = "DEL";
    entry.connectionPath = kPath;
    entry.isInternalPanel = internal;
    return entry;
}

MonitorSnapshot monitorSnapshot(const std::string& id, uint64_t generation, const std::string& displayState)
{
    MonitorSnapshot monitor;
    monitor.id = id;
    monitor.name = kName;
    monitor.identityStatus = "stable";
    monitor.generation = generation;
    monitor.displayState = displayState;
    return monitor;
}

Snapshot snapshotWith(std::vector<MonitorSnapshot> monitors)
{
    Snapshot snapshot;
    snapshot.revision = 1;
    snapshot.monitors = std::move(monitors);
    return snapshot;
}

AttemptRequest writeRequest(const std::string& monitorId, uint8_t code, uint32_t value)
{
    AttemptRequest request;
    request.monitorId = monitorId;
    request.write = true;
    request.code = code;
    request.value = value;
    return request;
}

AttemptRequest readRequest(const std::string& monitorId, uint8_t code)
{
    AttemptRequest request;
    request.monitorId = monitorId;
    request.write = false;
    request.code = code;
    return request;
}

MonitorInfo::Channel supportedChannel(double raw, double max)
{
    MonitorInfo::Channel channel;
    channel.supported = true;
    channel.raw = raw;
    channel.max = max;
    return channel;
}

MonitorInfo::Channel unsupportedChannel()
{
    return MonitorInfo::Channel{};
}

// Expected monitor_discovered, built by the real builder so only field values are compared.
std::string discoveredJson(const std::string& id, bool stable, const std::string& name,
                           MonitorInfo::Channel brightness, MonitorInfo::Channel volume)
{
    MonitorInfo info;
    info.id = id;
    info.name = name;
    info.identityStable = stable;
    info.connectionPath = kPath;
    info.brightness = brightness;
    info.volume = volume;
    return telaviva::buildMonitorDiscovered(info);
}

std::size_t countOf(const std::vector<std::string>& items, const std::string& expected)
{
    std::size_t count = 0;
    for (const std::string& item : items) {
        if (item == expected) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(ddc_controller_discovers_stable_monitor_once_with_features)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.ports.state(kIdA)->channels[kVolume].value = 30;
    rig.controller.refresh();
    CHECK_EQ(rig.submitter.sent.size(), std::size_t(1));
    CHECK_EQ(rig.submitter.sent.front(),
             discoveredJson(kIdA, true, kName, supportedChannel(41, 100), supportedChannel(30, 100)));
    CHECK(rig.submitter.sent.front().find("\"identity_status\":\"stable\"") != std::string::npos);
}

TEST(ddc_controller_failed_volume_read_reports_volume_unsupported)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.ports.state(kIdA)->channels[kVolume].readable = false;
    rig.controller.refresh();
    CHECK_EQ(rig.submitter.sent.size(), std::size_t(1));
    CHECK_EQ(rig.submitter.sent.front(),
             discoveredJson(kIdA, true, kName, supportedChannel(41, 100), unsupportedChannel()));
    CHECK(rig.submitter.sent.front().find("\"volume\":{\"supported\":false}") != std::string::npos);
}

TEST(ddc_controller_second_refresh_with_same_catalog_sends_nothing)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    CHECK_EQ(rig.submitter.sent.size(), std::size_t(1));
    rig.controller.refresh();
    CHECK_EQ(rig.submitter.sent.size(), std::size_t(1));
}

TEST(ddc_controller_removed_monitor_sends_disconnected)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    rig.catalog.entries.clear();
    rig.controller.refresh();
    CHECK_EQ(rig.submitter.sent.size(), std::size_t(2));
    CHECK_EQ(rig.submitter.sent.back(), telaviva::buildMonitorDisconnected(kIdA));
}

TEST(ddc_controller_ambiguous_monitor_is_discovered_without_ddc_calls)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, false, kName)};
    rig.controller.refresh();
    CHECK_EQ(rig.submitter.sent.size(), std::size_t(1));
    CHECK_EQ(rig.submitter.sent.front(),
             discoveredJson(kIdA, false, kName, unsupportedChannel(), unsupportedChannel()));
    CHECK(rig.submitter.sent.front().find("\"identity_status\":\"ambiguous\"") != std::string::npos);
    CHECK(rig.ports.callsFor(kIdA).empty());
}

TEST(ddc_controller_internal_panel_is_never_discovered)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kInternalId, true, "Built-in panel", true)};
    rig.controller.refresh();
    CHECK(rig.submitter.sent.empty());
}

TEST(ddc_controller_display_off_reports_asleep_for_every_monitor)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName), makeEntry(kIdB, true, kName)};
    rig.controller.refresh();
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "awake"), monitorSnapshot(kIdB, 9, "awake")}));
    rig.controller.onDisplayState(false);
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildMonitorDisplayState(kIdA, 4, "asleep")), std::size_t(1));
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildMonitorDisplayState(kIdB, 9, "asleep")), std::size_t(1));
}

TEST(ddc_controller_display_on_reports_awake)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "asleep")}));
    rig.controller.onDisplayState(true);
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildMonitorDisplayState(kIdA, 4, "awake")), std::size_t(1));
}

TEST(ddc_controller_run_write_succeeds_and_records_port_write)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    const AttemptOutcome outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(outcome.ok);
    CHECK(outcome.error.empty());
    CHECK(!outcome.value.has_value());
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(1));
}

TEST(ddc_controller_run_read_returns_raw_current_value)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 47;
    rig.controller.refresh();
    const AttemptOutcome outcome = rig.controller.run(readRequest(kIdA, kBrightness));
    CHECK(outcome.ok);
    CHECK(outcome.error.empty());
    CHECK(outcome.value.has_value() && *outcome.value == 47.0);
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=47"), std::size_t(0));
}

TEST(ddc_controller_run_read_error_maps_to_unsupported)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    rig.ports.state(kIdA)->channels[kBrightness].readable = false;
    const AttemptOutcome outcome = rig.controller.run(readRequest(kIdA, kBrightness));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("unsupported"));
    CHECK(!outcome.value.has_value());
}

TEST(ddc_controller_run_write_error_maps_to_error_names)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    Channel& channel = rig.ports.state(kIdA)->channels[kBrightness];

    channel.writeError = DdcError::Unsupported;
    AttemptOutcome outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("unsupported"));

    channel.writeError = DdcError::Disconnected;
    outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("disconnected"));

    channel.writeError = DdcError::Io;
    outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("io"));
}

TEST(ddc_controller_run_unknown_monitor_is_disconnected)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    const AttemptOutcome outcome = rig.controller.run(writeRequest(kIdB, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("disconnected"));
    CHECK(rig.ports.callsFor(kIdB).empty());
}

TEST(ddc_controller_run_ambiguous_monitor_is_unsupported)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, false, kName)};
    rig.controller.refresh();
    const AttemptOutcome outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("unsupported"));
    CHECK(rig.ports.callsFor(kIdA).empty());
}

TEST(ddc_controller_run_monitor_without_port_is_unsupported)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.withoutHandle.insert(kIdA);
    rig.controller.refresh();
    const AttemptOutcome outcome = rig.controller.run(readRequest(kIdA, kBrightness));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("unsupported"));
}

TEST(ddc_controller_run_failing_write_is_attempted_once_without_sleep)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    rig.ports.state(kIdA)->channels[kBrightness].writeError = DdcError::Io;
    const int sleepsBefore = rig.sleeper.calls;
    const AttemptOutcome outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("io"));
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(1));
    CHECK_EQ(rig.sleeper.calls, sleepsBefore);
}

TEST(ddc_controller_run_after_shutdown_is_disconnected)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.controller.shutdown();
    const std::size_t callsBefore = rig.ports.callsFor(kIdA).size();
    const AttemptOutcome outcome = rig.controller.run(writeRequest(kIdA, kBrightness, 58));
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error, std::string("disconnected"));
    CHECK_EQ(rig.ports.callsFor(kIdA).size(), callsBefore);
}

TEST(ddc_controller_listener_receives_profile_on_discovery_and_removal)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.controller.refresh();
    CHECK_EQ(rig.listener.profiles.size(), std::size_t(1));
    CHECK(rig.listener.profiles.front().first == kIdA);
    CHECK(rig.listener.profiles.front().second == "DEL");
    CHECK(rig.listener.removed.empty());

    rig.catalog.entries.clear();
    rig.controller.refresh();
    CHECK_EQ(rig.listener.removed.size(), std::size_t(1));
    CHECK(rig.listener.removed.front() == kIdA);
}
