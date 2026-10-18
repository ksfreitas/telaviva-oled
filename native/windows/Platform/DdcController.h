#pragma once
// Discovers external monitors over DDC/CI, reports them to the core and runs one DDC
// primitive on request (IAttemptRunner). Retries, backoff, readback checks and hardware_result
// building live in the core handle (architecture Decision 6). Device discovery reads still go
// through DdcTransaction (with its own retries). No windows.h here; the dxva2 adapter lives
// behind IDdcPortProvider.
#include "DisplayCatalog.h"
#include "IAttemptRunner.h"
#include "IDdcPort.h"
#include "ISleeper.h"
#include "JsonCodec.h"

#include <functional>
#include <memory>
#include <string>

namespace telaviva {

// Sends one JSON command to the core (tv_submit). The return value is the TV_ status,
// 0 on success. The controller does not retry failed submissions.
struct ICoreSubmitter {
    virtual ~ICoreSubmitter() = default;
    virtual int submit(const std::string& json) = 0;
};

// Opens the DDC transport for one catalog entry. Called only on the executor.
struct IDdcPortProvider {
    virtual ~IDdcPortProvider() = default;
    // Returns nullptr when the monitor has no DDC handle; the monitor is then
    // discovered with both features unsupported and gets no hardware calls.
    virtual std::unique_ptr<IDdcPort> open(const CatalogEntry& entry) = 0;
};

// Runs DDC work. The production executor is ONE worker thread, so every port call is
// serialized. Work may run inline inside post(), which tests rely on.
struct IDdcExecutor {
    virtual ~IDdcExecutor() = default;
    virtual void post(std::function<void()> work) = 0;
};

// Owns the per-monitor DDC state. Public methods may be called from any thread.
// Catalog enumeration and port opens run through the executor, and so does each
// one-primitive run(). Display-state reports run on the caller's thread.
class DdcController final : public IAttemptRunner {
public:
    // listener is optional. It is told about each newly discovered monitor and each removed one.
    DdcController(IDisplayCatalog& catalog, IDdcPortProvider& ports, ICoreSubmitter& submitter,
                  IDdcExecutor& executor, ISleeper& sleeper, IDdcListener* listener = nullptr);
    ~DdcController();

    DdcController(const DdcController&) = delete;
    DdcController& operator=(const DdcController&) = delete;

    // Optional decision trace, one line per event. Set once before the first call that can trace;
    // it is read without a lock afterwards. Empty means no line is built at all. The sink must not
    // call back into this controller.
    using TraceSink = std::function<void(const std::string&)>;
    void setTrace(TraceSink trace);

    // Sets or clears the discovery listener (null for none). Guarded by the controller's lock, so it
    // may be called at any time; the listener must stay valid until it is cleared or shutdown has drained.
    void setListener(IDdcListener* listener);

    // Enumerates the catalog and diffs it against the known set. Each new external
    // monitor is read on the executor (VCP 0x10 and 0x62 through DdcTransaction::readVcp)
    // and reported as monitor_discovered. supported is true only when that read
    // succeeded; raw is the current value, min is 0 and max the reported maximum.
    // Each removed id is reported as monitor_disconnected and forgotten. Internal panels
    // are ignored. An ambiguous (unstable) identity is still discovered with
    // identity_status "ambiguous", but it is not read, so both features are sent as
    // unsupported and no port is used for it. For each monitor already known and still
    // listed (external, active, stable identity), the port is reopened without reporting
    // anything to the core, so a handle invalidated by a topology change recovers; a failed
    // reopen keeps the old port.
    void refresh();

    // Records the connection generation and display state of each known monitor in the
    // snapshot. Monitors not yet discovered are skipped. Nothing else is done.
    void onSnapshot(const Snapshot& snapshot);

    // Session display status. Sends monitor_display_state for every known external
    // monitor that has a generation from onSnapshot (monitors without one are skipped).
    // displayOn false sends "asleep"; displayOn true sends "awake".
    void onDisplayState(bool displayOn);

    // Stops execution. Later refresh, onSnapshot and onDisplayState calls do nothing, queued
    // work checks the flag before any port call or submission, and run() returns "disconnected".
    void shutdown();

    // Runs exactly one DDC primitive for the monitor: a write of request.value, or a read of
    // request.code. No sleeps or readback comparison; a failure other than "unsupported" reopens
    // the monitor's port once and runs the same primitive again. Unknown monitor or shutdown gives
    // "disconnected"; an ambiguous monitor or one without a port gives "unsupported". Called on
    // the executor thread; the port lock is released before the port is touched.
    AttemptOutcome run(const AttemptRequest& request) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
