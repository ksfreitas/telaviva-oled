#pragma once
// Presence camera driver. It owns at most one capture session and turns the core's
// lifecycle decisions into camera, timer and presence_observation calls. It is a port of
// the macOS PresenceCamera shell (native/macos/TelaViva/PresenceCamera.swift) onto
// ICameraBackend, IFrameAnalyzer, IScheduler and ILifecycleApi. No windows.h here.
//
// Two clocks, kept apart on purpose:
//  - Lifecycle `now` values and frame times use IClock::steadySeconds() and
//    CameraFrame::steadySeconds: steady_clock seconds, as doubles. The core reads no clock.
//  - captured_at_ms in presence_observation is IClock::epochMillis(): Unix epoch
//    milliseconds from the system clock, because the core checks it against wall-clock time.
#include "CoreLifecycleApi.h"
#include "DdcController.h"
#include "ICameraBackend.h"
#include "IClock.h"
#include "FrameAnalyzer.h"
#include "JsonCodec.h"
#include "Scheduler.h"

#include <memory>

namespace telaviva {

class CameraLifecycleDriver {
public:
    // The driver holds references only. Every argument must outlive the driver.
    CameraLifecycleDriver(ICameraBackend& camera, IFrameAnalyzer& analyzer, ICoreSubmitter& submitter,
                          ILifecycleApi& lifecycle, IClock& clock, IScheduler& scheduler);
    // Stops the camera and cancels every timer the driver armed.
    ~CameraLifecycleDriver();

    CameraLifecycleDriver(const CameraLifecycleDriver&) = delete;
    CameraLifecycleDriver& operator=(const CameraLifecycleDriver&) = delete;

    // Reconciles the driver with one core snapshot.
    //  - camera.mode, camera.captureMode and camera.generation go to the lifecycle `reconcile`
    //    event with settings.always_on_camera. The camera id list is lifecycleCameraIdList over
    //    settings.selected_camera_id, because the snapshot does not carry camera_history yet.
    //  - If the lifecycle asks for a stop, or the mode, capture mode or generation changed, the
    //    camera closes and the sample, watchdog and settle timers are cancelled.
    //  - In face or light_only mode, one sample timer is armed after camera.nextSampleInMs
    //    (null counts as 0). Continuous capture arms it only when no check is in flight.
    //  - Outside face mode the analyzer is asked to release its detector after 30 s.
    //  - Once the driver has shut down, later snapshots are ignored.
    void onSnapshot(const Snapshot& snapshot);

    // Called when a camera is added or removed.
    //  - If the active camera left, or a camera ranked above it in the selection joined, the
    //    session restarts and the next sample is taken at once.
    //  - If the last reported camera status was busy or unavailable, camera_status "unknown"
    //    is submitted with the current generation, so a re-enumerated camera is not held back.
    void onDevicesChanged();

    // Closes the camera, cancels all timers and releases the analyzer. Later calls do nothing.
    void shutdown();

    // Presence observations, sent through the submitter as presence_observation:
    //  - Each sample reports with the generation it was started for, so a result never
    //    carries a newer or older generation than the sample that produced it.
    //  - The lifecycle `accepts_callback` check runs before every report. A result for a
    //    stale or disabled session is dropped.
    //  - camera_status is included only for the unavailable outcomes (permission,
    //    unavailable, busy, error). The core sets "ready" itself for the other outcomes.
    //
    // Lifecycle events and what the driver does with each:
    //  - reconcile (returns a stop flag): stop the camera when true, otherwise keep going.
    //  - begin_acquisition: called on each camera open. Arms a 5 s watchdog on the scheduler.
    //  - acquired_frame: called on the first frame of a session. Clears the acquisition deadline.
    //  - frame: the decision for each frame. discard_warmup drops it without analysis;
    //    assess_light calls the analyzer's assessLight; assess_presence calls detect;
    //    assessment_expired stops the camera and reports unavailable with busy.
    //  - begin_light_assessment: called at each light_only sample. Settles for 5 s, then
    //    arms a watchdog for the post-settle frame.
    //  - begin_dark_settle: called when a single-frame face sample is insufficient_light.
    //    Only the first dark result of a sample starts a settle; the camera stays open and
    //    frames are discarded until it ends. The next frame after the settle is final.
    //  - timed_out: checked when the watchdog fires. A passed deadline stops the camera and
    //    reports unavailable with busy.
    //  - cancel_deadlines: called whenever the camera stops or the policy changes.
    //  - is_current_assessment: checked before a settle or watchdog callback acts.
    //  - accepts_callback: checked before every report.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telaviva
