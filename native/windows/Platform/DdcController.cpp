#include "DdcController.h"

#include "DdcTimingProfile.h"
#include "DdcTransaction.h"
#include "Diagnostics.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {

namespace {

constexpr uint8_t kBrightnessCode = 0x10;
constexpr uint8_t kVolumeCode = 0x62;

// DdcResult carries only the current value, but discovery also reports the maximum.
// This keeps the last successful reading so DdcTransaction::readVcp can still do the retries.
class CapturingPort final : public IDdcPort {
public:
    explicit CapturingPort(IDdcPort& inner) : inner_(inner) {}

    bool readVcp(uint8_t code, DdcReading& out, DdcError& error) override
    {
        if (!inner_.readVcp(code, out, error)) {
            return false;
        }
        reading_ = out;
        return true;
    }

    bool writeVcp(uint8_t code, uint32_t value, DdcError& error) override
    {
        return inner_.writeVcp(code, value, error);
    }

    const std::optional<DdcReading>& reading() const { return reading_; }

private:
    IDdcPort& inner_;
    std::optional<DdcReading> reading_;
};

struct MonitorRecord {
    bool ambiguous = false;
    std::shared_ptr<IDdcPort> port;  // null when unstable or without a DDC handle; used only on the executor
    DdcTimingProfile profile;
    bool hasGeneration = false;
    uint64_t generation = 0;
    std::string displayState;
};

// Maps a transport error to the error name sent in attempt_done. None is treated as "io".
std::string errorName(DdcError error)
{
    if (error == DdcError::Unsupported) {
        return "unsupported";
    }
    if (error == DdcError::Disconnected) {
        return "disconnected";
    }
    return "io";
}

// Runs exactly one primitive on the port: a write of request.value, or a read of request.code.
AttemptOutcome runPrimitive(IDdcPort& port, const AttemptRequest& request)
{
    AttemptOutcome outcome;
    DdcError error = DdcError::None;
    if (request.write) {
        if (port.writeVcp(request.code, request.value, error)) {
            outcome.ok = true;
        } else {
            outcome.error = errorName(error);
        }
        return outcome;
    }

    DdcReading reading;
    if (port.readVcp(request.code, reading, error)) {
        outcome.ok = true;
        outcome.value = static_cast<double>(reading.current);
    } else {
        outcome.error = errorName(error);
    }
    return outcome;
}

// VCP code as two hex digits with a 0x prefix, for example 0x10.
std::string hexCode(uint8_t code)
{
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "0x%02x", static_cast<unsigned>(code));
    return buffer;
}

// Brightness as a whole number, for the discovery line.
std::string brightnessSummary(const MonitorInfo::Channel& channel)
{
    if (!channel.supported || !channel.raw || !channel.max) {
        return "brightness=read_failed";
    }
    return "brightness=" + std::to_string(static_cast<long long>(*channel.raw)) + "/" +
           std::to_string(static_cast<long long>(*channel.max));
}

}  // namespace

// Every member is guarded by mutex_. Port calls, catalog enumeration and submissions happen
// with the lock released, so a re-entrant or inline executor cannot deadlock.
struct DdcController::Impl {
    Impl(IDisplayCatalog& catalog, IDdcPortProvider& ports, ICoreSubmitter& submitter, IDdcExecutor& executor,
         ISleeper& sleeper, IDdcListener* listener)
        : catalog_(catalog), ports_(ports), submitter_(submitter), executor_(executor), sleeper_(sleeper),
          listener_(listener)
    {
    }

    ~Impl() { stop(); }

    IDisplayCatalog& catalog_;
    IDdcPortProvider& ports_;
    ICoreSubmitter& submitter_;
    IDdcExecutor& executor_;
    ISleeper& sleeper_;
    IDdcListener* listener_;  // guarded by mutex_; may be null

    std::recursive_mutex mutex_;
    bool stopped_ = false;
    std::vector<CatalogEntry> knownCatalog_;
    std::map<std::string, MonitorRecord> monitors_;
    TraceSink trace_;  // stored by setTrace; discovery does not trace
    NoticeSink notice_;  // stored by setNotice; read without a lock, like trace_
    // Consecutive failed primitives per monitor id, cleared by the next success or by removal.
    std::map<std::string, uint32_t> failureStreaks_;  // guarded by mutex_

    // The listener in effect now. Read under mutex_, and the hook is called with the lock released.
    IDdcListener* currentListener()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return listener_;
    }

    // Runs on the executor.
    void refreshOnExecutor()
    {
        std::vector<CatalogEntry> previous;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            previous = knownCatalog_;
        }

        const std::vector<CatalogEntry> current = catalog_.enumerate();
        const CatalogDiff diff = diffCatalogs(previous, current);
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            knownCatalog_ = current;
        }

        for (const std::string& removedId : diff.removedIds) {
            bool wasKnown = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                wasKnown = monitors_.erase(removedId) != 0;
                failureStreaks_.erase(removedId);
            }
            // Only monitors the core knows get a disconnect; internal panels never do.
            if (wasKnown) {
                submit(buildMonitorDisconnected(removedId));
                notify("discovery_removed monitor=" + shortId(removedId));
                if (IDdcListener* target = currentListener(); target != nullptr) {
                    target->onMonitorRemoved(removedId);
                }
            }
        }

        // Entries just added are not in monitors_ yet, so they are skipped here.
        reopenKnownPorts(current);

        for (const CatalogEntry& entry : diff.added) {
            if (entry.isInternalPanel) {
                continue;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
            }

            MonitorRecord record;
            record.ambiguous = !entry.identity.stable;
            record.profile = DdcTimingProfile::forManufacturer(entry.manufacturer);

            MonitorInfo info;
            info.id = entry.identity.id;
            info.name = entry.identity.name;
            info.identityStable = entry.identity.stable;
            info.connectionPath = entry.connectionPath;
            info.primary = entry.primary;

            // An ambiguous identity may address the wrong panel, so it is never read or opened.
            if (!record.ambiguous) {
                record.port = ports_.open(entry);
                if (record.port) {
                    info.brightness = readFeature(*record.port, record.profile, kBrightnessCode);
                    info.volume = readFeature(*record.port, record.profile, kVolumeCode);
                }
            }

            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                // A re-discovered id replaces the old record; the next snapshot supplies the new generation.
                monitors_[info.id] = std::move(record);
            }
            notify("discovery_added monitor=" + shortId(info.id) + " " + brightnessSummary(info.brightness));
            submit(buildMonitorDiscovered(info));
            if (IDdcListener* target = currentListener(); target != nullptr) {
                target->onMonitorProfile(info.id, entry.manufacturer);
            }
        }
    }

    // Runs on the executor. Replaces the port of every known external monitor that is still listed, so a
    // handle that a topology change invalidated is renewed. The core is not told: the monitor is the same,
    // and a new monitor_discovered would reset its generation, pending actions and baseline. A failed open
    // keeps the old port.
    void reopenKnownPorts(const std::vector<CatalogEntry>& current)
    {
        for (const CatalogEntry& entry : current) {
            if (entry.isInternalPanel || !entry.active || !entry.identity.stable || entry.identity.id.empty()) {
                continue;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                const auto found = monitors_.find(entry.identity.id);
                if (found == monitors_.end() || found->second.ambiguous || !found->second.port) {
                    continue;
                }
            }

            std::shared_ptr<IDdcPort> reopened = ports_.open(entry);
            if (!reopened) {
                continue;
            }
            bool replaced = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return;
                }
                const auto found = monitors_.find(entry.identity.id);
                if (found != monitors_.end()) {
                    found->second.port = std::move(reopened);
                    replaced = true;
                }
            }
            if (replaced) {
                notify("port_reopened monitor=" + shortId(entry.identity.id));
            }
        }
    }

    // Runs on the executor. Opens a fresh port for one known monitor that is still in the catalog and
    // stores it. Returns null when the monitor is unknown, gone or cannot be opened.
    std::shared_ptr<IDdcPort> reopenPort(const std::string& monitorId)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return nullptr;
            }
            const auto found = monitors_.find(monitorId);
            if (found == monitors_.end() || found->second.ambiguous || !found->second.port) {
                return nullptr;
            }
        }

        for (const CatalogEntry& entry : catalog_.enumerate()) {
            if (entry.identity.id != monitorId || entry.isInternalPanel || !entry.active) {
                continue;
            }
            std::shared_ptr<IDdcPort> reopened = ports_.open(entry);
            if (!reopened) {
                return nullptr;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (stopped_) {
                    return nullptr;
                }
                const auto found = monitors_.find(monitorId);
                if (found == monitors_.end()) {
                    return nullptr;
                }
                found->second.port = reopened;
            }
            if (trace_) {
                trace_("ddc reopened port for " + shortId(monitorId));
            }
            notify("port_reopened monitor=" + shortId(monitorId));
            return reopened;
        }
        return nullptr;
    }

    MonitorInfo::Channel readFeature(IDdcPort& port, const DdcTimingProfile& profile, uint8_t code)
    {
        CapturingPort capture(port);
        DdcTransaction transaction(capture, sleeper_, profile);
        const DdcResult result = transaction.readVcp(code);
        MonitorInfo::Channel channel;
        if (!result.ok || !capture.reading()) {
            return channel;
        }
        channel.supported = true;
        channel.raw = static_cast<double>(capture.reading()->current);
        channel.max = static_cast<double>(capture.reading()->maximum);
        return channel;
    }

    // Records the generation and display state of each known monitor. Nothing else.
    void recordSnapshot(const Snapshot& snapshot)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        for (const MonitorSnapshot& monitor : snapshot.monitors) {
            auto found = monitors_.find(monitor.id);
            if (found == monitors_.end()) {
                continue;
            }
            found->second.hasGeneration = true;
            found->second.generation = monitor.generation;
            found->second.displayState = monitor.displayState;
        }
    }

    void handleDisplayState(bool displayOn)
    {
        const char* state = displayOn ? "awake" : "asleep";
        std::vector<std::string> reports;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            for (auto& entry : monitors_) {
                entry.second.displayState = state;
                if (entry.second.hasGeneration) {
                    reports.push_back(buildMonitorDisplayState(entry.first, entry.second.generation, state));
                }
            }
        }
        for (const std::string& report : reports) {
            submit(report);
        }
    }

    void stop()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        stopped_ = true;
    }

    // Sends one line to the notice sink, when one is set. Called with no lock held.
    void notify(const std::string& line)
    {
        if (notice_) {
            notice_(line);
        }
    }

    // Runs on the executor. Runs one primitive and records the outcome: a failure gets an attempt_failed line
    // and extends the monitor's streak, and the first success after failures gets attempt_recovered. Only the
    // short id is written, never the raw monitor id.
    AttemptOutcome runNoticed(IDdcPort& port, const AttemptRequest& request)
    {
        const auto started = std::chrono::steady_clock::now();
        AttemptOutcome outcome = runPrimitive(port, request);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        if (!outcome.ok) {
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                ++failureStreaks_[request.monitorId];
            }
            notify("attempt_failed monitor=" + shortId(request.monitorId) + " op=" +
                   (request.write ? "write" : "read") + " code=" + hexCode(request.code) +
                   " error=" + outcome.error + " elapsed_ms=" + std::to_string(elapsed.count()));
            return outcome;
        }

        uint32_t failures = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto found = failureStreaks_.find(request.monitorId);
            if (found != failureStreaks_.end()) {
                failures = found->second;
                failureStreaks_.erase(found);
            }
        }
        if (failures > 0) {
            notify("attempt_recovered monitor=" + shortId(request.monitorId) +
                   " after_failures=" + std::to_string(failures));
        }
        return outcome;
    }

    // Returns 0 when the core accepted the command and -1 when the controller is stopped.
    int submit(const std::string& json)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (stopped_) {
                return -1;
            }
        }
        return submitter_.submit(json);
    }
};

DdcController::DdcController(IDisplayCatalog& catalog, IDdcPortProvider& ports, ICoreSubmitter& submitter,
                             IDdcExecutor& executor, ISleeper& sleeper, IDdcListener* listener)
    : impl_(std::make_unique<Impl>(catalog, ports, submitter, executor, sleeper, listener))
{
}

DdcController::~DdcController() = default;

void DdcController::refresh()
{
    Impl* impl = impl_.get();
    impl->executor_.post([impl] { impl->refreshOnExecutor(); });
}

void DdcController::onSnapshot(const Snapshot& snapshot)
{
    impl_->recordSnapshot(snapshot);
}

void DdcController::onDisplayState(bool displayOn)
{
    impl_->handleDisplayState(displayOn);
}

void DdcController::setTrace(TraceSink trace)
{
    impl_->trace_ = std::move(trace);
}

void DdcController::setNotice(NoticeSink notice)
{
    impl_->notice_ = std::move(notice);
}

void DdcController::setListener(IDdcListener* listener)
{
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->listener_ = listener;
}

void DdcController::shutdown()
{
    impl_->stop();
}

AttemptOutcome DdcController::run(const AttemptRequest& request)
{
    AttemptOutcome outcome;
    std::shared_ptr<IDdcPort> port;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (impl_->stopped_) {
            outcome.error = "disconnected";
            return outcome;
        }
        const auto found = impl_->monitors_.find(request.monitorId);
        if (found == impl_->monitors_.end()) {
            outcome.error = "disconnected";
            return outcome;
        }
        if (found->second.ambiguous || !found->second.port) {
            outcome.error = "unsupported";
            return outcome;
        }
        port = found->second.port;
    }

    // The port is touched with the lock released. One primitive; when it fails for any reason other than an
    // unsupported feature, the handle may be stale, so the port is reopened and the same primitive runs once more.
    outcome = impl_->runNoticed(*port, request);
    if (outcome.ok || outcome.error == "unsupported") {
        return outcome;
    }
    const std::shared_ptr<IDdcPort> reopened = impl_->reopenPort(request.monitorId);
    if (!reopened) {
        return outcome;
    }
    return impl_->runNoticed(*reopened, request);
}

}  // namespace telaviva
