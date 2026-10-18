#pragma once
// Wraps the Rust core C ABI. Snapshot changes reach sinks on a worker thread,
// never from inside tv_submit and never while the core mutex is held.
#include "JsonCodec.h"
#include "UpdateChecker.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace telaviva {

struct CoreBridgeOptions {
    std::wstring configPath;  // settings.json; sent to the core as UTF-8
    std::string osLocale;     // passed for compatibility only
};

using SnapshotSink = std::function<void(const Snapshot&)>;

class CoreBridge final {
public:
    // Never throws for core failures: a mismatched ABI or a failed tv_create
    // leaves isOpen() false.
    explicit CoreBridge(const CoreBridgeOptions& options);
    ~CoreBridge();

    CoreBridge(const CoreBridge&) = delete;
    CoreBridge& operator=(const CoreBridge&) = delete;

    static uint32_t abiVersion();
    // Stateless. Returns an empty string when the core call fails.
    static std::string coreVersion();
    // Stateless. Returns nullopt for any non-TV_OK status or an unreadable outcome.
    static std::optional<UpdateOutcome> evaluateUpdate(const std::string& installed, int status,
                                                       const std::string& body);
    bool isOpen() const;

    // Returns the raw TV_* status. TV_CLOSED when the core is not open.
    int32_t submit(const std::string& json);

    // Synchronous read. Throws std::runtime_error when the core is not open or the read fails.
    Snapshot snapshot();

    // Sinks are called on the worker thread after each accepted revision.
    void addSink(SnapshotSink sink);

    // Calls tv_shutdown, then polls until pending_actions is empty or drainDeadline passes.
    void shutdown(std::chrono::milliseconds drainDeadline);

private:
    struct Impl;
    void runWorker();

    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
