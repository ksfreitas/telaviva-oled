#include "TestHarness.h"

#include "Fakes.h"
#include "Platform/DdcController.h"
#include "Platform/DisplayCatalog.h"
#include "Platform/IDdcPort.h"
#include "Platform/ISleeper.h"
#include "Platform/JsonCodec.h"
#include "Platform/Scheduler.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using telaviva::CatalogEntry;
using telaviva::DdcController;
using telaviva::DdcError;
using telaviva::DdcReading;
using telaviva::FakeScheduler;
using telaviva::HardwareCompletion;
using telaviva::ICoreSubmitter;
using telaviva::IHardwareObserver;
using telaviva::IDdcExecutor;
using telaviva::IDdcPort;
using telaviva::IDdcPortProvider;
using telaviva::IDisplayCatalog;
using telaviva::ISleeper;
using telaviva::MonitorInfo;
using telaviva::MonitorSnapshot;
using telaviva::PendingAction;
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
    bool acceptWrites = true; // false: writes are dropped, so the readback mismatches
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
    int status = 0;  // non-zero rejects every submission

    int submit(const std::string& json) override
    {
        sent.push_back(json);
        return status;
    }
};

class RecordingObserver final : public IHardwareObserver {
public:
    std::vector<HardwareCompletion> completions;

    void onHardwareCompleted(const HardwareCompletion& completion) override { completions.push_back(completion); }
};

// Runs each job inline, so the whole flow is deterministic on the test thread.
class InlineExecutor final : public IDdcExecutor {
public:
    void post(std::function<void()> work) override { work(); }
};

class FakeSleeper final : public ISleeper {
public:
    void sleepFor(std::chrono::milliseconds) override {}
};

// Declaration order matters: the controller keeps references to every member above it.
struct Rig {
    FakeCatalog catalog;
    FakePortProvider ports;
    FakeSubmitter submitter;
    InlineExecutor executor;
    FakeSleeper sleeper;
    FakeScheduler scheduler;
    RecordingObserver observer;
    DdcController controller{catalog, ports, submitter, executor, sleeper, scheduler, &observer};
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

PendingAction action(uint64_t id, const std::string& monitorId, uint64_t generation, const std::string& kind,
                     std::optional<double> value)
{
    PendingAction pending;
    pending.id = id;
    pending.monitorId = monitorId;
    pending.generation = generation;
    pending.kind = kind;
    pending.value = value;
    return pending;
}

Snapshot snapshotWith(std::vector<MonitorSnapshot> monitors, std::vector<PendingAction> actions)
{
    Snapshot snapshot;
    snapshot.revision = 1;
    snapshot.monitors = std::move(monitors);
    snapshot.pendingActions = std::move(actions);
    return snapshot;
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

TEST(ddc_controller_set_brightness_writes_reads_back_and_reports_ok)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(1));
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "read 16"), std::size_t(2));
    CHECK_EQ(rig.submitter.sent.back(), telaviva::buildHardwareResult(7, 4, true, 58.0, std::nullopt));
}

TEST(ddc_controller_failing_readback_reports_io_error)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.ports.state(kIdA)->channels[kBrightness].acceptWrites = false;
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(1));
    CHECK_EQ(rig.submitter.sent.back(),
             telaviva::buildHardwareResult(7, 4, false, std::nullopt, std::string("io")));
}

TEST(ddc_controller_read_brightness_reports_raw_value)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 47;
    rig.controller.refresh();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(8, kIdA, 4, "read_brightness", std::nullopt)}));
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=47"), std::size_t(0));
    CHECK_EQ(rig.submitter.sent.back(), telaviva::buildHardwareResult(8, 4, true, 47.0, std::nullopt));
}

TEST(ddc_controller_same_action_id_in_two_snapshots_runs_once)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    const Snapshot snapshot =
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)});
    rig.controller.onSnapshot(snapshot);
    rig.controller.onSnapshot(snapshot);
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(1));
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildHardwareResult(7, 4, true, 58.0, std::nullopt)),
             std::size_t(1));
}

TEST(ddc_controller_asleep_monitor_gets_no_hardware_call_and_no_result)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    const std::size_t sentBefore = rig.submitter.sent.size();
    const std::size_t callsBefore = rig.ports.callsFor(kIdA).size();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "asleep")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(rig.ports.callsFor(kIdA).size(), callsBefore);
    CHECK_EQ(rig.submitter.sent.size(), sentBefore);
}

TEST(ddc_controller_display_off_reports_asleep_for_every_monitor)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName), makeEntry(kIdB, true, kName)};
    rig.controller.refresh();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake"), monitorSnapshot(kIdB, 9, "awake")}, {}));
    rig.controller.onDisplayState(false);
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildMonitorDisplayState(kIdA, 4, "asleep")), std::size_t(1));
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildMonitorDisplayState(kIdB, 9, "asleep")), std::size_t(1));
}

TEST(ddc_controller_display_on_reports_awake_and_holds_actions_for_grace)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {}));
    rig.controller.onDisplayState(true);
    CHECK_EQ(countOf(rig.submitter.sent, telaviva::buildMonitorDisplayState(kIdA, 4, "awake")), std::size_t(1));

    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(0));

    rig.scheduler.advance(1999ms);
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(0));

    rig.scheduler.advance(1ms);
    CHECK_EQ(countOf(rig.ports.callsFor(kIdA), "write 16=58"), std::size_t(1));
    CHECK_EQ(rig.submitter.sent.back(), telaviva::buildHardwareResult(7, 4, true, 58.0, std::nullopt));
}

TEST(ddc_controller_reports_successful_brightness_set_to_observer)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(rig.observer.completions.size(), std::size_t(1));
    const telaviva::HardwareCompletion& done = rig.observer.completions.front();
    CHECK(done.monitorId == kIdA);
    CHECK(done.feature == "brightness");
    CHECK_EQ(done.generation, uint64_t(4));
    CHECK(done.ok);
    CHECK(done.observedPercent.has_value() && std::fabs(*done.observedPercent - 58.0) < 1e-9);
}

TEST(ddc_controller_reports_successful_volume_set_with_volume_feature)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kVolume].value = 30;
    rig.controller.refresh();
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(9, kIdA, 4, "set_volume", 60.0)}));
    CHECK_EQ(rig.observer.completions.size(), std::size_t(1));
    const telaviva::HardwareCompletion& done = rig.observer.completions.front();
    CHECK(done.feature == "volume");
    CHECK(done.ok);
    CHECK(done.observedPercent.has_value() && std::fabs(*done.observedPercent - 60.0) < 1e-9);
}

TEST(ddc_controller_reports_failed_set_without_percent)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.ports.state(kIdA)->channels[kBrightness].acceptWrites = false;
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(rig.observer.completions.size(), std::size_t(1));
    CHECK(!rig.observer.completions.front().ok);
    CHECK(!rig.observer.completions.front().observedPercent.has_value());
}

TEST(ddc_controller_skipped_set_is_reported_as_failed)
{
    // An ambiguous monitor is answered "ambiguous", so its draft must still settle.
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, false, kName)};
    rig.controller.refresh();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(rig.observer.completions.size(), std::size_t(1));
    CHECK(!rig.observer.completions.front().ok);
    CHECK(rig.observer.completions.front().feature == "brightness");
}

TEST(ddc_controller_read_and_held_actions_are_not_reported_to_observer)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 47;
    rig.controller.refresh();
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(8, kIdA, 4, "read_brightness", std::nullopt)}));
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "asleep")}, {action(9, kIdA, 4, "set_brightness", 58.0)}));
    CHECK(rig.observer.completions.empty());
}

TEST(ddc_controller_rejected_result_is_not_reported_to_observer)
{
    // The core did not take the result, so the action is offered again and no slot may be released.
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.submitter.status = -1;
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK(rig.observer.completions.empty());
}

TEST(ddc_controller_shutdown_stops_further_actions)
{
    Rig rig;
    rig.catalog.entries = {makeEntry(kIdA, true, kName)};
    rig.ports.state(kIdA)->channels[kBrightness].value = 41;
    rig.controller.refresh();
    rig.controller.onSnapshot(snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {}));
    rig.controller.shutdown();
    const std::size_t sentBefore = rig.submitter.sent.size();
    const std::size_t callsBefore = rig.ports.callsFor(kIdA).size();
    rig.controller.onSnapshot(
        snapshotWith({monitorSnapshot(kIdA, 4, "awake")}, {action(7, kIdA, 4, "set_brightness", 58.0)}));
    CHECK_EQ(rig.ports.callsFor(kIdA).size(), callsBefore);
    CHECK_EQ(rig.submitter.sent.size(), sentBefore);
}
