#pragma once
// Discovers external monitors over DDC/CI, reports them to the core and runs the
// core's pending hardware actions (architecture Decision 6, contracts "Pending-action
// execution"). No windows.h here; the dxva2 adapter lives behind IDdcPortProvider.
#include "DisplayCatalog.h"
#include "IDdcPort.h"
#include "ISleeper.h"
#include "JsonCodec.h"
#include "Scheduler.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace telaviva {

// One set_brightness or set_volume hardware action, reported after its hardware_result was
// accepted by the core. feature is "brightness" or "volume". observedPercent is the readback
// as a percent of the reported maximum, present only when ok.
struct HardwareCompletion {
    std::string monitorId;
    std::string feature;
    uint64_t generation = 0;  // action generation, which equals the connection generation it was sent for
    bool ok = false;
    std::optional<double> observedPercent;
};

// Receives completions on the executor thread, never under DdcController's mutex. The
// observer must stay valid until DdcController::shutdown has returned and queued work has drained.
struct IHardwareObserver {
    virtual ~IHardwareObserver() = default;
    virtual void onHardwareCompleted(const HardwareCompletion& completion) = 0;
};

// Sends one JSON command to the core (tv_submit). The return value is the TV_ status,
// 0 on success. The controller does not retry failed submissions; the next snapshot
// or display-state change is the recovery path.
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
// Catalog enumeration, port opens, DDC transactions and their hardware_result
// submissions all run through the executor; display-state reports and the grace
// timer run on the caller or the scheduler thread.
class DdcController {
public:
    // observer is optional. It is told about each set action after the core accepted its result,
    // including failed and skipped ones; read_brightness is not reported.
    DdcController(IDisplayCatalog& catalog, IDdcPortProvider& ports, ICoreSubmitter& submitter,
                  IDdcExecutor& executor, ISleeper& sleeper, IScheduler& scheduler,
                  IHardwareObserver* observer = nullptr);
    // Cancels the grace timer. Work already posted to the executor must be drained
    // before destruction, because it captures this controller.
    ~DdcController();

    DdcController(const DdcController&) = delete;
    DdcController& operator=(const DdcController&) = delete;

    // Enumerates the catalog and diffs it against the known set. Each new external
    // monitor is read on the executor (VCP 0x10 and 0x62 through DdcTransaction::readVcp)
    // and reported as monitor_discovered. supported is true only when that read
    // succeeded; raw is the current value, min is 0 and max the reported maximum.
    // Each removed id is reported as monitor_disconnected and forgotten. Internal panels
    // are ignored. An ambiguous (unstable) identity is still discovered with
    // identity_status "ambiguous", but it is not read, so both features are sent as
    // unsupported and no port is used for it.
    void refresh();

    // Stores the connection generations and display states of the snapshot, then for
    // each monitor's first pending action (the core exposes at most one per monitor)
    // posts the work to the executor. Rules per action:
    //  - An id already in flight or already completed is skipped.
    //  - A monitor whose displayState is "asleep", or whose wake grace is running, is
    //    not touched: no port call and no hardware_result. The action is not marked
    //    completed, so it runs once the monitor is awake and the grace has ended.
    //  - A monitor that is not discovered gets hardware_result error "disconnected".
    //  - An ambiguous monitor gets error "ambiguous"; a monitor without a port gets
    //    error "unsupported".
    //  - set_brightness and set_volume: write the raw target (action.value rounded to an
    //    integer) with DdcTransaction::setAndVerify on 0x10 or 0x62. ok is true only when
    //    the readback equals the target, and then value is the readback. Otherwise ok is
    //    false with the DdcResult error ("io", "unsupported" or "disconnected").
    //  - read_brightness: readVcp(0x10); ok with value = raw readback, or ok false.
    //  - Any other kind: ok false with error "unsupported".
    // Every result is sent as buildHardwareResult(actionId, action.generation, ...);
    // action.generation is the action's generation, not the connection generation.
    void onSnapshot(const Snapshot& snapshot);

    // Session display status. Sends monitor_display_state for every known external
    // monitor with its connection generation from the last snapshot (monitors without
    // a generation yet are skipped). displayOn false sends "asleep" and cancels any
    // running grace. displayOn true sends "awake" and arms profile.wakeGrace (2 s) on the
    // scheduler. Actions that arrive or wait during the grace run when it ends.
    void onDisplayState(bool displayOn);

    // Stops execution. Later refresh, onSnapshot and onDisplayState calls do nothing,
    // queued work checks the flag before any port call or submission, and the grace
    // timer is cancelled.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
