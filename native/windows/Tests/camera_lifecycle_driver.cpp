#include "TestHarness.h"

#include "FakeCamera.h"
#include "Fakes.h"
#include "Platform/CameraLifecycleDriver.h"
#include "Platform/CameraStatus.h"
#include "Platform/CoreLifecycleApi.h"
#include "Platform/DdcController.h"
#include "Platform/FrameAnalyzer.h"
#include "Platform/ICameraBackend.h"
#include "Platform/JsonCodec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using telaviva::AnalysisStatus;
using telaviva::CameraFailure;
using telaviva::CameraFrame;
using telaviva::CameraInfo;
using telaviva::CameraLifecycleDriver;
using telaviva::CoreLifecycleApi;
using telaviva::FakeCamera;
using telaviva::FakeClock;
using telaviva::FakeScheduler;
using telaviva::FrameAnalysis;
using telaviva::ICoreSubmitter;
using telaviva::IFrameAnalyzer;
using telaviva::Snapshot;

namespace {

// Epoch milliseconds at the start of each test. The clock moves it with every advance.
constexpr uint64_t kEpochMs = 1781000000000ULL;
// Steady-clock seconds at the start of each test; keeps deadlines away from zero.
constexpr double kStartSteady = 100.0;

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

class FakeAnalyzer final : public IFrameAnalyzer {
public:
    FrameAnalysis detect(const CameraFrame&) override
    {
        ++detectCalls;
        return detectResult;
    }

    FrameAnalysis assessLight(const CameraFrame&) override
    {
        ++assessCalls;
        return assessResult;
    }

    void releaseIdle() override { ++releaseCalls; }

    FrameAnalysis detectResult{AnalysisStatus::Absent};
    FrameAnalysis assessResult{AnalysisStatus::UsableLight};
    int detectCalls = 0;
    int assessCalls = 0;
    int releaseCalls = 0;
};

// Records every submitted command, in order.
class FakeSubmitter final : public ICoreSubmitter {
public:
    int submit(const std::string& json) override
    {
        messages.push_back(json);
        return 0;
    }

    std::vector<std::string> messages;
};

// Number of submitted messages with the given "type" that also contain needle.
int countCommands(const FakeSubmitter& submitter, const std::string& type, const std::string& needle = "")
{
    const std::string typeText = "\"type\":\"" + type + "\"";
    int count = 0;
    for (const std::string& message : submitter.messages) {
        if (contains(message, typeText) && contains(message, needle)) {
            ++count;
        }
    }
    return count;
}

int presenceCount(const FakeSubmitter& submitter)
{
    return countCommands(submitter, "presence_observation");
}

// The most recent presence_observation text, or an empty string when there is none.
std::string lastPresence(const FakeSubmitter& submitter)
{
    for (auto it = submitter.messages.rbegin(); it != submitter.messages.rend(); ++it) {
        if (contains(*it, "\"type\":\"presence_observation\"")) {
            return *it;
        }
    }
    return std::string();
}

// True when some presence_observation has this status. An empty cameraStatus means the
// camera_status key must be absent.
bool hasPresence(const FakeSubmitter& submitter, const std::string& status, const std::string& cameraStatus)
{
    for (const std::string& message : submitter.messages) {
        if (!contains(message, "\"type\":\"presence_observation\"") ||
            !contains(message, "\"status\":\"" + status + "\"")) {
            continue;
        }
        const bool hasCameraStatus = contains(message, "\"camera_status\":");
        if (cameraStatus.empty()) {
            if (!hasCameraStatus) {
                return true;
            }
        } else if (contains(message, "\"camera_status\":\"" + cameraStatus + "\"")) {
            return true;
        }
    }
    return false;
}

// The wire form of the captured_at_ms member, for the given epoch value.
std::string capturedAtField(uint64_t epochMs)
{
    return "\"captured_at_ms\":" + std::to_string(epochMs);
}

CameraInfo builtInCamera(const std::string& id)
{
    CameraInfo camera;
    camera.id = id;
    camera.name = L"Built-in camera";
    camera.builtIn = true;
    return camera;
}

// A snapshot whose settings select cam-a. Only the camera fields the driver reads are set.
Snapshot snapshotFor(const std::string& mode, const std::string& captureMode, uint64_t generation,
                     std::optional<double> nextSampleMs, bool alwaysOn)
{
    Snapshot snapshot;
    snapshot.settings.selected_camera_id = std::string("cam-a");
    snapshot.settings.always_on_camera = alwaysOn;
    snapshot.camera.mode = mode;
    snapshot.camera.captureMode = captureMode;
    snapshot.camera.generation = generation;
    snapshot.camera.nextSampleInMs = nextSampleMs;
    return snapshot;
}

// The driver under test, wired to the fakes and to the real core lifecycle.
struct Rig {
    FakeCamera camera;
    FakeAnalyzer analyzer;
    FakeSubmitter submitter;
    CoreLifecycleApi lifecycle;
    FakeClock clock;
    FakeScheduler scheduler;
    CameraLifecycleDriver driver;

    Rig() : driver(camera, analyzer, submitter, lifecycle, clock, scheduler)
    {
        clock.setSteadySeconds(kStartSteady);
        clock.setEpochMillis(kEpochMs);
        camera.setDevices({builtInCamera("cam-a")});
    }
};

// Moves the clock and the scheduler together, so each timer runs at its due clock time.
// Whole seconds keep the double clock exact at the 5 s boundaries.
void advanceSeconds(Rig& rig, int seconds)
{
    rig.clock.advance(static_cast<double>(seconds));
    rig.scheduler.advance(std::chrono::milliseconds(seconds * 1000));
}

// Delivers one frame stamped with the current steady clock, as a backend would.
void deliverFrame(Rig& rig)
{
    rig.camera.emitFrame(640, 480, 0x80, rig.clock.steadySeconds());
}

int openCount(const Rig& rig)
{
    return static_cast<int>(rig.camera.openCalls().size());
}

// True when the first open asked for about 2 fps, the retained-capture rate in contracts.md.
bool openedAtAboutTwoFps(const Rig& rig)
{
    if (rig.camera.openCalls().empty()) {
        return false;
    }
    const double fps = rig.camera.openCalls()[0].targetFps;
    return fps > 1.9 && fps < 2.1;
}

// Starts a single-frame face sample whose only frame is dark. The camera stays open in its settle window.
void startDarkFaceSample(Rig& rig)
{
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::InsufficientLight};
    deliverFrame(rig);
}

// A suspended snapshot that carries the core's retry delay, as the core reports after busy.
Snapshot suspendedWithRetry(uint64_t generation, double retryMs)
{
    Snapshot snapshot = snapshotFor("suspended", "stopped", generation, std::nullopt, false);
    snapshot.camera.retryInMs = retryMs;
    return snapshot;
}

// Makes the driver report busy for generation 5, the way a camera that fails to start does.
void reportBusy(Rig& rig)
{
    CameraFailure startFailure;
    startFailure.startFailed = true;
    rig.camera.setOpenFailure(startFailure);
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
}

}  // namespace

TEST(lifecycle_driver_single_frame_face_reports_present_and_closes)
{
    Rig rig;
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Present};
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    CHECK_EQ(openCount(rig), 0);

    advanceSeconds(rig, 1);
    CHECK_EQ(openCount(rig), 1);
    if (openCount(rig) == 1) {
        CHECK_EQ(rig.camera.openCalls()[0].id, std::string("cam-a"));
    }

    const uint64_t reportedAt = rig.clock.epochMillis();
    deliverFrame(rig);
    CHECK_EQ(rig.analyzer.detectCalls, 1);
    CHECK_EQ(presenceCount(rig.submitter), 1);
    CHECK(hasPresence(rig.submitter, "present", ""));
    CHECK(contains(lastPresence(rig.submitter), "\"camera_generation\":5"));
    CHECK(contains(lastPresence(rig.submitter), capturedAtField(reportedAt)));
    CHECK(!rig.camera.isOpen());
    CHECK_EQ(rig.camera.counters().close, 1);
}

TEST(lifecycle_driver_face_absent_reports_absent_without_camera_status)
{
    Rig rig;
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Absent};
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
    deliverFrame(rig);

    CHECK(hasPresence(rig.submitter, "absent", ""));
    CHECK_EQ(presenceCount(rig.submitter), 1);
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_dark_single_frame_waits_five_seconds_then_reports_settled_frame)
{
    Rig rig;
    startDarkFaceSample(rig);
    // The first dark result starts a 5 s settle, so nothing is reported and the camera stays open.
    CHECK_EQ(presenceCount(rig.submitter), 0);
    CHECK(rig.camera.isOpen());

    advanceSeconds(rig, 5);
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Present};
    deliverFrame(rig);
    CHECK(hasPresence(rig.submitter, "present", ""));
    CHECK_EQ(presenceCount(rig.submitter), 1);
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_dark_single_frame_still_dark_after_settle_reports_insufficient_light)
{
    Rig rig;
    startDarkFaceSample(rig);
    advanceSeconds(rig, 5);
    // The analyzer still reports insufficient light, and the settle allowance is spent.
    deliverFrame(rig);

    CHECK(hasPresence(rig.submitter, "insufficient_light", ""));
    CHECK_EQ(presenceCount(rig.submitter), 1);
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_continuous_return_reports_insufficient_light_at_once)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("face", "continuous_return", 9, 1000.0, false));
    advanceSeconds(rig, 1);
    CHECK_EQ(openCount(rig), 1);
    CHECK(openedAtAboutTwoFps(rig));

    // Continuous capture skips the dark settle, so the dark result is reported immediately.
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::InsufficientLight};
    deliverFrame(rig);
    CHECK(hasPresence(rig.submitter, "insufficient_light", ""));
    CHECK(rig.camera.isOpen());
}

TEST(lifecycle_driver_light_only_discards_warmup_then_reports_usable_light)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("light_only", "single_frame", 2, 1000.0, false));
    advanceSeconds(rig, 1);
    CHECK_EQ(openCount(rig), 1);

    // A frame inside the five-second exposure settle is discarded before any assessment.
    deliverFrame(rig);
    CHECK_EQ(rig.analyzer.assessCalls, 0);
    CHECK_EQ(presenceCount(rig.submitter), 0);
    CHECK(rig.camera.isOpen());

    advanceSeconds(rig, 5);
    deliverFrame(rig);
    CHECK_EQ(rig.analyzer.assessCalls, 1);
    CHECK_EQ(rig.analyzer.detectCalls, 0);
    CHECK(hasPresence(rig.submitter, "usable_light", ""));
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_access_denied_reports_unavailable_with_permission)
{
    Rig rig;
    CameraFailure denied;
    denied.hresult = 0x80070005u;
    rig.camera.setOpenFailure(denied);
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);

    CHECK(hasPresence(rig.submitter, "unavailable", "permission"));
    CHECK_EQ(presenceCount(rig.submitter), 1);
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_missing_device_reports_unavailable_without_opening)
{
    Rig rig;
    rig.camera.setDevices({});
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);

    CHECK_EQ(openCount(rig), 0);
    CHECK(hasPresence(rig.submitter, "unavailable", "unavailable"));
}

TEST(lifecycle_driver_acquisition_deadline_reports_busy_and_closes)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);   // opened at t=101
    advanceSeconds(rig, 4);   // t=105, inside the 5 s watchdog
    CHECK_EQ(presenceCount(rig.submitter), 0);
    CHECK(rig.camera.isOpen());

    advanceSeconds(rig, 1);   // t=106, the watchdog fires
    CHECK(hasPresence(rig.submitter, "unavailable", "busy"));
    CHECK(!rig.camera.isOpen());

    // The backend has stopped delivering, so a late frame is never assessed.
    deliverFrame(rig);
    CHECK_EQ(rig.analyzer.detectCalls, 0);
}

TEST(lifecycle_driver_analyzer_error_reports_unavailable_with_error)
{
    Rig rig;
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Error};
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
    deliverFrame(rig);

    CHECK(hasPresence(rig.submitter, "unavailable", "error"));
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_late_frame_after_post_settle_deadline_is_not_assessed)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("light_only", "single_frame", 2, 1000.0, false));
    advanceSeconds(rig, 1);   // t=101, opened
    deliverFrame(rig);        // warmup, inside the settle window
    advanceSeconds(rig, 5);   // t=106, settle ends; the post-settle deadline is t=111

    // The clock passes the deadline before the scheduler runs the watchdog, so the late
    // frame is what observes the expiry.
    rig.clock.advance(6.0);   // t=112
    deliverFrame(rig);

    CHECK_EQ(rig.analyzer.assessCalls, 0);
    CHECK(!hasPresence(rig.submitter, "usable_light", ""));
    CHECK(hasPresence(rig.submitter, "unavailable", "busy"));
    CHECK_EQ(presenceCount(rig.submitter), 1);
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_generation_change_restarts_and_reports_the_new_generation)
{
    Rig rig;
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Present};
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
    CHECK_EQ(openCount(rig), 1);

    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 6, 1000.0, false));
    CHECK(!rig.camera.isOpen());

    advanceSeconds(rig, 1);
    CHECK_EQ(openCount(rig), 2);
    deliverFrame(rig);
    CHECK(contains(lastPresence(rig.submitter), "\"camera_generation\":6"));
    CHECK_EQ(countCommands(rig.submitter, "presence_observation", "\"camera_generation\":5"), 0);
}

TEST(lifecycle_driver_always_on_keeps_camera_open_and_discards_frames_between_checks)
{
    Rig rig;
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Present};
    rig.driver.onSnapshot(snapshotFor("face", "continuous_monitoring", 3, 1000.0, true));
    advanceSeconds(rig, 1);
    CHECK_EQ(openCount(rig), 1);
    CHECK(openedAtAboutTwoFps(rig));

    deliverFrame(rig);
    CHECK(hasPresence(rig.submitter, "present", ""));
    CHECK(rig.camera.isOpen());

    // Frames between checks reach the backend but are not analysed.
    deliverFrame(rig);
    CHECK_EQ(rig.analyzer.detectCalls, 1);

    // The next snapshot arms the next check; the same session serves it.
    rig.driver.onSnapshot(snapshotFor("face", "continuous_monitoring", 3, 1000.0, true));
    advanceSeconds(rig, 1);
    deliverFrame(rig);
    CHECK_EQ(rig.analyzer.detectCalls, 2);
    CHECK_EQ(presenceCount(rig.submitter), 2);
    CHECK_EQ(openCount(rig), 1);
    CHECK_EQ(rig.camera.counters().close, 0);
    CHECK(rig.camera.isOpen());
}

TEST(lifecycle_driver_disabled_mode_cancels_the_pending_sample)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    rig.driver.onSnapshot(snapshotFor("disabled", "stopped", 6, std::nullopt, false));
    advanceSeconds(rig, 3);

    CHECK_EQ(openCount(rig), 0);
    CHECK(!rig.camera.isOpen());
}

TEST(lifecycle_driver_suspended_closes_an_open_session_and_stops_its_timers)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("face", "continuous_monitoring", 3, 1000.0, true));
    advanceSeconds(rig, 1);
    CHECK(rig.camera.isOpen());

    rig.driver.onSnapshot(snapshotFor("suspended", "stopped", 4, std::nullopt, true));
    CHECK(!rig.camera.isOpen());
    CHECK_EQ(rig.camera.counters().close, 1);

    // The acquisition watchdog armed at open would report busy here, so no report proves it is cancelled.
    advanceSeconds(rig, 10);
    CHECK_EQ(openCount(rig), 1);
    deliverFrame(rig);
    CHECK_EQ(presenceCount(rig.submitter), 0);
}

TEST(lifecycle_driver_devices_changed_after_busy_sends_camera_status_unknown_once)
{
    Rig rig;
    CameraFailure startFailure;
    startFailure.startFailed = true;
    rig.camera.setOpenFailure(startFailure);
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
    CHECK(hasPresence(rig.submitter, "unavailable", "busy"));

    rig.driver.onDevicesChanged();
    CHECK_EQ(countCommands(rig.submitter, "camera_status", "\"status\":\"unknown\",\"generation\":5"), 1);

    // The status is now unknown, so a second change sends nothing new.
    rig.driver.onDevicesChanged();
    CHECK_EQ(countCommands(rig.submitter, "camera_status", "\"status\":\"unknown\",\"generation\":5"), 1);
}

TEST(lifecycle_driver_devices_changed_after_ready_sends_nothing)
{
    Rig rig;
    rig.analyzer.detectResult = FrameAnalysis{AnalysisStatus::Present};
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 5, 1000.0, false));
    advanceSeconds(rig, 1);
    deliverFrame(rig);

    rig.driver.onDevicesChanged();
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 0);
}

TEST(lifecycle_driver_shutdown_closes_the_camera_and_stops_timers)
{
    Rig rig;
    rig.driver.onSnapshot(snapshotFor("face", "continuous_monitoring", 3, 1000.0, true));
    advanceSeconds(rig, 1);
    CHECK(rig.camera.isOpen());

    rig.driver.shutdown();
    CHECK(!rig.camera.isOpen());
    CHECK_EQ(rig.camera.counters().close, 1);

    // The watchdog and the sample timer must not fire after shutdown.
    advanceSeconds(rig, 10);
    CHECK_EQ(presenceCount(rig.submitter), 0);

    // Later snapshots are ignored, so the camera is not reopened.
    rig.driver.onSnapshot(snapshotFor("face", "single_frame", 4, 1000.0, false));
    advanceSeconds(rig, 2);
    CHECK_EQ(openCount(rig), 1);
}

TEST(lifecycle_driver_busy_retry_sends_camera_status_unknown_when_the_core_delay_elapses)
{
    Rig rig;
    reportBusy(rig);
    CHECK(hasPresence(rig.submitter, "unavailable", "busy"));

    rig.driver.onSnapshot(suspendedWithRetry(5, 30000.0));
    advanceSeconds(rig, 29);
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 0);

    advanceSeconds(rig, 1);   // 30 s after the snapshot
    CHECK_EQ(countCommands(rig.submitter, "camera_status", "\"status\":\"unknown\",\"generation\":5"), 1);

    // The retry is one-shot.
    advanceSeconds(rig, 60);
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 1);
}

TEST(lifecycle_driver_repeated_snapshot_does_not_postpone_the_busy_retry)
{
    Rig rig;
    reportBusy(rig);
    rig.driver.onSnapshot(suspendedWithRetry(5, 30000.0));
    advanceSeconds(rig, 20);
    rig.driver.onSnapshot(suspendedWithRetry(5, 30000.0));   // the same generation, 20 s later

    advanceSeconds(rig, 9);   // 29 s after the first snapshot
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 0);
    advanceSeconds(rig, 1);   // 30 s after the first snapshot
    CHECK_EQ(countCommands(rig.submitter, "camera_status", "\"status\":\"unknown\",\"generation\":5"), 1);
}

TEST(lifecycle_driver_busy_retry_is_cancelled_when_the_core_reports_no_delay)
{
    Rig rig;
    reportBusy(rig);
    rig.driver.onSnapshot(suspendedWithRetry(5, 30000.0));
    advanceSeconds(rig, 10);
    rig.driver.onSnapshot(snapshotFor("suspended", "stopped", 5, std::nullopt, false));

    advanceSeconds(rig, 30);
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 0);
}

TEST(lifecycle_driver_shutdown_cancels_the_busy_retry)
{
    Rig rig;
    reportBusy(rig);
    rig.driver.onSnapshot(suspendedWithRetry(5, 30000.0));
    advanceSeconds(rig, 10);
    rig.driver.shutdown();

    advanceSeconds(rig, 30);
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 0);
}

TEST(lifecycle_driver_new_generation_rearms_the_busy_retry_from_that_snapshot)
{
    Rig rig;
    reportBusy(rig);
    rig.driver.onSnapshot(suspendedWithRetry(5, 30000.0));
    advanceSeconds(rig, 20);
    rig.driver.onSnapshot(suspendedWithRetry(6, 30000.0));   // new generation at t+20

    advanceSeconds(rig, 10);   // t+30, when the old generation's retry would have fired
    CHECK_EQ(countCommands(rig.submitter, "camera_status"), 0);
    advanceSeconds(rig, 20);   // t+50, 30 s after the new snapshot
    CHECK_EQ(countCommands(rig.submitter, "camera_status", "\"status\":\"unknown\",\"generation\":6"), 1);
    CHECK_EQ(countCommands(rig.submitter, "camera_status", "\"generation\":5"), 0);
}

TEST(lifecycle_core_api_steps_the_real_core)
{
    CoreLifecycleApi api;
    const std::string reconciled = api.step(
        R"({"type":"reconcile","mode":"face","capture_mode":"single_frame","generation":1,"camera_id":"cam-a","always_on":false})");
    CHECK(contains(reconciled, "\"value\":false"));
    CHECK(contains(reconciled, "\"generation\":1"));
    CHECK(contains(reconciled, "\"camera_id\":\"cam-a\""));

    const std::string callback = api.step(R"({"type":"accepts_callback","generation":1})");
    CHECK(contains(callback, "\"value\":true"));
}

TEST(lifecycle_core_api_returns_empty_text_for_a_rejected_event)
{
    CoreLifecycleApi api;
    CHECK(api.step("{\"type\":\"explode\"}").empty());
    CHECK(api.step("not json").empty());
}
