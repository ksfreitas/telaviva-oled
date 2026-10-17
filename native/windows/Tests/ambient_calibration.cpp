#include "TestHarness.h"

#include "../Platform/AdaptiveBrightness.h"
#include "../Platform/AmbientCalibrationController.h"
#include "../Platform/CalibrationSession.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using telaviva::ActiveCalibration;
using telaviva::AdaptiveBrightness;
using telaviva::AmbientCurve;
using telaviva::AmbientCurveMap;
using telaviva::AmbientCurvePoint;
using telaviva::AmbientCalibrationController;
using telaviva::CalibrationEndReason;
using telaviva::CalibrationNotice;
using telaviva::CalibrationSession;
using telaviva::IPowerSettings;
using telaviva::PowerSource;
using telaviva::PowerStatus;
using telaviva::SavedAdaptiveState;
using telaviva::SliderRange;

namespace {

// Unique per call; the destructor removes the directory and everything in it.
class TempDir {
public:
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("telaviva-ambient-" + std::to_string(stamp) + "-" + std::to_string(std::random_device{}()));
        std::error_code ec;
        static_cast<void>(fs::create_directories(path_, ec));
    }
    ~TempDir()
    {
        std::error_code ec;
        static_cast<void>(fs::remove_all(path_, ec));
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// Every call the controller makes into a fake is appended here, in order.
using EventLog = std::vector<std::string>;

const std::string kScheme = "381b4222-f694-41f0-9685-ff5bb260df2e";

std::string levelText(double value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string joined(const EventLog& events)
{
    std::string text;
    for (const std::string& event : events) {
        if (!text.empty()) {
            text += "; ";
        }
        text += event;
    }
    return text;
}

bool contains(const std::string& text, const char* fragment)
{
    return text.find(fragment) != std::string::npos;
}

std::string describe(const std::optional<ActiveCalibration>& active)
{
    if (!active) {
        return "none";
    }
    return active->monitorId + " " + std::to_string(active->point) + " " + levelText(active->sliderValue);
}

std::string describeRange(const SliderRange& range)
{
    return levelText(range.lower) + ".." + levelText(range.upper);
}

std::string noticeName(const std::optional<CalibrationNotice>& notice)
{
    if (!notice) {
        return "none";
    }
    switch (*notice) {
    case CalibrationNotice::BuiltInUnreadable:
        return "builtInUnreadable";
    case CalibrationNotice::BuiltInWriteFailed:
        return "builtInWriteFailed";
    case CalibrationNotice::BuiltInUnavailable:
        return "builtInUnavailable";
    case CalibrationNotice::AutoBrightnessUnknown:
        return "autoBrightnessUnknown";
    case CalibrationNotice::AutoBrightnessNotChanged:
        return "autoBrightnessNotChanged";
    case CalibrationNotice::RecoveryFailed:
        return "recoveryFailed";
    }
    return "unknown";
}

// Curve points as "monitor:builtin" in stored order, for comparing curves without operator==.
std::string curveText(const AmbientCurve& curve)
{
    std::string text;
    for (const AmbientCurvePoint& point : curve.points) {
        if (!text.empty()) {
            text += " ";
        }
        text += levelText(point.monitor) + ":" + levelText(point.builtin);
    }
    return text;
}

// Builds a curve from (monitor percent, MacBook level) pairs, in the order given, like the Swift tests.
AmbientCurve makeCurve(const std::vector<std::pair<int, double>>& pairs)
{
    AmbientCurve curve;
    for (const auto& pair : pairs) {
        curve.points.push_back(AmbientCurvePoint{static_cast<double>(pair.first), pair.second});
    }
    return curve;
}

// The Laptop state the fixture captures: scheme with AC and DC both 1.
SavedAdaptiveState capturedState()
{
    SavedAdaptiveState state;
    state.schemeGuid = kScheme;
    state.acValue = 1;
    state.dcValue = 1;
    return state;
}

// Minimal in-memory power API. Writes are logged before they can fail, as on Windows.
class FakePower final : public IPowerSettings {
public:
    FakePower(EventLog& log, uint32_t ac, uint32_t dc)
        : log_(log), ac_(ac), dc_(dc), active_(kScheme)
    {
    }

    PowerStatus activeScheme(std::string& schemeGuid) override
    {
        schemeGuid = active_;
        return PowerStatus::Ok;
    }

    PowerStatus readAdaptive(const std::string&, PowerSource source, uint32_t& value) override
    {
        if (failReads) {
            return PowerStatus::Failed;
        }
        value = source == PowerSource::Ac ? ac_ : dc_;
        return PowerStatus::Ok;
    }

    PowerStatus writeAdaptive(const std::string&, PowerSource source, uint32_t value) override
    {
        log_.push_back(std::string("adaptive ") + (source == PowerSource::Ac ? "ac " : "dc ") +
                       levelText(static_cast<double>(value)));
        if (failWrites) {
            return PowerStatus::Failed;
        }
        if (source == PowerSource::Ac) {
            ac_ = value;
        } else {
            dc_ = value;
        }
        return PowerStatus::Ok;
    }

    PowerStatus setActiveScheme(const std::string& schemeGuid) override
    {
        active_ = schemeGuid;
        return PowerStatus::Ok;
    }

    uint32_t ac() const { return ac_; }
    uint32_t dc() const { return dc_; }

    bool failReads = false;
    bool failWrites = false;

private:
    EventLog& log_;
    uint32_t ac_;
    uint32_t dc_;
    std::string active_;
};

class FakePanel final : public telaviva::ICalibrationPanel {
public:
    FakePanel(EventLog& log, std::optional<double> level)
        : log_(log), level_(level)
    {
    }

    std::optional<double> panelPercent() override { return level_; }

    bool setPanelPercent(double percent) override
    {
        log_.push_back("writeLevel " + levelText(percent));
        // A refused write is still logged and leaves the level unchanged.
        if (failedLevels.count(percent) != 0) {
            return false;
        }
        level_ = percent;
        return true;
    }

    std::set<double> failedLevels;

private:
    EventLog& log_;
    std::optional<double> level_;
};

class FakeFollow final : public telaviva::IFollowPause {
public:
    explicit FakeFollow(EventLog& log) : log_(log) {}

    void setCalibrating(bool calibrating) override
    {
        log_.push_back(calibrating ? "setCalibrating true" : "setCalibrating false");
        calibrating_ = calibrating;
    }

    bool paused() override { return paused_; }

    bool paused_ = false;
    bool calibrating_ = false;

private:
    EventLog& log_;
};

class FakeMonitors final : public telaviva::ICalibrationMonitors {
public:
    explicit FakeMonitors(EventLog& log) : log_(log) {}

    std::optional<double> observedPercent(const std::string& monitorId) override
    {
        const auto it = observed.find(monitorId);
        if (it == observed.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void setPercent(const std::string& monitorId, double percent) override
    {
        log_.push_back("setMonitor " + monitorId + " " + levelText(percent));
    }

    // Readouts stand in for the monitor; writes are logged and do not change them. A missing key is unknown.
    std::map<std::string, double> observed;

private:
    EventLog& log_;
};

class FakeSettings final : public telaviva::ICalibrationSettings {
public:
    explicit FakeSettings(EventLog& log) : log_(log) {}

    telaviva::SettingsDraft currentDraft() override { return draft; }

    AmbientCurveMap curves() override { return stored; }

    int submit(const std::string& json) override
    {
        ++submits;
        lastJson = json;
        log_.push_back("submit");
        return 0;
    }

    // The curves the core currently holds. Submits do not update them, so tests set them between steps.
    AmbientCurveMap stored;
    telaviva::SettingsDraft draft;
    std::string lastJson;
    int submits = 0;

private:
    EventLog& log_;
};

// One temp directory, a fake power API with AC and DC values, a Laptop panel at `level`, and the fakes.
struct Fixture {
    explicit Fixture(uint32_t ac = 1, uint32_t dc = 1, std::optional<double> level = 44.0)
        : power(log, ac, dc),
          session(dir.path()),
          adaptive(power),
          panel(log, level),
          follow(log),
          monitors(log),
          settings(log),
          controller(panel, settings, follow, monitors, adaptive, session)
    {
        monitors.observed = {{"monitor-a", 62.0}, {"monitor-b", 30.0}};
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    bool payloadHas(const char* fragment) const { return contains(settings.lastJson, fragment); }

    EventLog log;
    TempDir dir;
    FakePower power;
    CalibrationSession session;
    AdaptiveBrightness adaptive;
    FakePanel panel;
    FakeFollow follow;
    FakeMonitors monitors;
    FakeSettings settings;
    AmbientCalibrationController controller;
};

}  // namespace

TEST(begin_captures_state_and_writes_first_point)
{
    Fixture f;
    f.controller.begin("monitor-a", 25);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 25 44"));
    // Adaptive brightness is saved and turned off, then the pause and the monitor and panel moves follow.
    CHECK(f.session.pending() == capturedState());
    CHECK_EQ(joined(f.log), std::string("adaptive ac 0; adaptive dc 0; setCalibrating true; "
                                        "setMonitor monitor-a 25; writeLevel 44"));
    CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
}

TEST(second_point_keeps_original_capture)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{50, 45.0}});
    f.controller.begin("monitor-a", 25);
    f.log.clear();
    f.controller.begin("monitor-a", 50);
    CHECK_EQ(joined(f.log), std::string("setMonitor monitor-a 50; writeLevel 45"));
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 45"));
    CHECK(f.session.pending() == capturedState());
}

TEST(initial_slider_falls_back_to_the_captured_builtin_level)
{
    Fixture f(1, 1, 37.0);
    f.controller.begin("monitor-a", 75);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 75 37"));
    CHECK_EQ(joined(f.log), std::string("adaptive ac 0; adaptive dc 0; setCalibrating true; "
                                        "setMonitor monitor-a 75; writeLevel 37"));
}

TEST(low_points_start_a_session_at_their_monitor_level)
{
    Fixture f;
    // A stored 10% point at 8 caps the 5% range at 8, so the captured Laptop level 44 is clamped to it.
    f.settings.stored["monitor-a"] = makeCurve({{10, 8.0}});
    f.controller.begin("monitor-a", 5);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 5 8"));
    CHECK_EQ(joined(f.log), std::string("adaptive ac 0; adaptive dc 0; setCalibrating true; "
                                        "setMonitor monitor-a 5; writeLevel 8"));
    f.log.clear();
    // 20% is bounded below by the 10% point and has no upper neighbor, so the captured level 44 is inside its range.
    f.controller.begin("monitor-a", 20);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 20 44"));
    CHECK_EQ(joined(f.log), std::string("setMonitor monitor-a 20; writeLevel 44"));
    CHECK(f.session.pending() == capturedState());
}

TEST(monitor_one_point_starts_a_session_at_monitor_one)
{
    Fixture f;
    f.controller.begin("monitor-a", 1);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 1 44"));
    CHECK_EQ(joined(f.log), std::string("adaptive ac 0; adaptive dc 0; setCalibrating true; "
                                        "setMonitor monitor-a 1; writeLevel 44"));
    CHECK(f.session.pending() == capturedState());
}

TEST(slider_range_is_bounded_by_neighbors)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{25, 30.0}, {75, 70.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(50, "monitor-a")), std::string("30..70"));
    CHECK_EQ(describeRange(f.controller.allowedRange(25, "monitor-a")), std::string("0..70"));
    CHECK_EQ(describeRange(f.controller.allowedRange(100, "monitor-a")), std::string("70..100"));
    CHECK_EQ(describeRange(f.controller.allowedRange(50, "monitor-b")), std::string("0..100"));
    f.controller.begin("monitor-a", 50);
    f.log.clear();
    f.controller.setSlider(10);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 30"));
    f.controller.setSlider(90);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 70"));
    f.controller.setSlider(50);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 50"));
    CHECK_EQ(joined(f.log), std::string("writeLevel 30; writeLevel 70; writeLevel 50"));
}

TEST(slider_range_is_bounded_by_low_point_neighbors)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{10, 8.0}, {20, 16.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(15, "monitor-a")), std::string("8..16"));
    CHECK_EQ(describeRange(f.controller.allowedRange(5, "monitor-a")), std::string("0..8"));
    f.settings.stored["monitor-a"] = makeCurve({{20, 16.0}, {50, 45.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(25, "monitor-a")), std::string("16..45"));
}

TEST(slider_range_is_bounded_by_monitor_one_neighbors)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{1, 2.0}, {10, 12.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(5, "monitor-a")), std::string("2..12"));
    f.settings.stored["monitor-a"] = makeCurve({{5, 6.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(1, "monitor-a")), std::string("0..6"));
    f.settings.stored["monitor-a"] = makeCurve({{10, 9.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(1, "monitor-a")), std::string("0..9"));
}

TEST(allowed_range_without_neighbors_is_the_full_range)
{
    Fixture f;
    CHECK_EQ(describeRange(f.controller.allowedRange(1, "monitor-a")), std::string("0..100"));
    CHECK_EQ(describeRange(f.controller.allowedRange(100, "monitor-a")), std::string("0..100"));
    f.settings.stored["monitor-a"] = makeCurve({{50, 40.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(25, "monitor-a")), std::string("0..40"));
    CHECK_EQ(describeRange(f.controller.allowedRange(60, "monitor-a")), std::string("40..100"));
}

TEST(allowed_range_with_equal_neighbors_is_zero_width)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{25, 40.0}, {75, 40.0}});
    CHECK_EQ(describeRange(f.controller.allowedRange(50, "monitor-a")), std::string("40..40"));
}

TEST(curve_set_keeps_ascending_order)
{
    AmbientCurve curve;
    curve.set(75, 70.0);
    curve.set(25, 20.0);
    curve.set(50, 40.0);
    CHECK_EQ(curveText(curve), std::string("25:20 50:40 75:70"));
    curve.set(50, 45.0);
    CHECK_EQ(curveText(curve), std::string("25:20 50:45 75:70"));
    curve.remove(25);
    CHECK_EQ(curveText(curve), std::string("50:45 75:70"));
}

TEST(add_keeps_ascending_order)
{
    Fixture f;
    f.controller.begin("monitor-a", 50);
    CHECK(f.controller.confirm());
    // The core's stored curve is set between steps, as the snapshot would report it.
    f.settings.stored["monitor-a"] = makeCurve({{50, 44.0}});
    f.controller.begin("monitor-a", 25);
    CHECK(f.controller.confirm());
    CHECK(f.payloadHas(R"({"monitor":25,"builtin":44},{"monitor":50,"builtin":44})"));
    f.settings.stored["monitor-a"] = makeCurve({{25, 44.0}, {50, 44.0}});
    f.controller.begin("monitor-a", 75);
    CHECK(f.controller.confirm());
    CHECK(f.payloadHas(R"({"monitor":25,"builtin":44},{"monitor":50,"builtin":44},{"monitor":75,"builtin":44})"));
}

TEST(confirm_saves_only_the_changed_point)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{25, 20.0}, {75, 70.0}});
    f.controller.begin("monitor-a", 50);
    f.log.clear();
    f.controller.setSlider(40);
    CHECK_EQ(joined(f.log), std::string("writeLevel 40"));
    f.log.clear();
    CHECK(f.controller.confirm());
    CHECK(f.payloadHas(R"({"monitor":25,"builtin":20},{"monitor":50,"builtin":40},{"monitor":75,"builtin":70})"));
    CHECK_EQ(joined(f.log), std::string("submit"));
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 40"));
    CHECK(f.session.pending() == capturedState());
}

TEST(confirm_on_an_existing_point_replaces_its_value)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{25, 20.0}, {50, 40.0}, {75, 70.0}});
    f.controller.begin("monitor-a", 50);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 40"));
    f.controller.setSlider(45);
    f.log.clear();
    CHECK(f.controller.confirm());
    CHECK(f.payloadHas(R"({"monitor":25,"builtin":20},{"monitor":50,"builtin":45},{"monitor":75,"builtin":70})"));
    CHECK_EQ(joined(f.log), std::string("submit"));
}

TEST(confirm_saves_a_low_point_only)
{
    Fixture f;
    // The stored 5% and 20% points bound the 10% range at 2...16, and the other points must survive the save.
    f.settings.stored["monitor-a"] = makeCurve({{5, 2.0}, {20, 16.0}, {25, 20.0}, {75, 70.0}});
    f.controller.begin("monitor-a", 10);
    f.log.clear();
    f.controller.setSlider(12);
    CHECK_EQ(joined(f.log), std::string("writeLevel 12"));
    f.log.clear();
    CHECK(f.controller.confirm());
    CHECK(f.payloadHas(R"({"monitor":5,"builtin":2},{"monitor":10,"builtin":12},{"monitor":20,"builtin":16},)"
                       R"({"monitor":25,"builtin":20},{"monitor":75,"builtin":70})"));
    CHECK_EQ(joined(f.log), std::string("submit"));
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 10 12"));
    CHECK(f.session.pending() == capturedState());
}

TEST(confirm_saves_only_the_monitor_one_point)
{
    Fixture f;
    // The stored 5% point bounds the 1% range at 0...6, and the other points must survive the save.
    f.settings.stored["monitor-a"] = makeCurve({{5, 6.0}, {10, 9.0}, {25, 20.0}});
    f.controller.begin("monitor-a", 1);
    f.log.clear();
    f.controller.setSlider(1);
    CHECK_EQ(joined(f.log), std::string("writeLevel 1"));
    f.log.clear();
    CHECK(f.controller.confirm());
    CHECK(f.payloadHas(R"({"monitor":1,"builtin":1},{"monitor":5,"builtin":6},{"monitor":10,"builtin":9},)"
                       R"({"monitor":25,"builtin":20})"));
    CHECK_EQ(joined(f.log), std::string("submit"));
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 1 1"));
    CHECK(f.session.pending() == capturedState());
}

TEST(remove_point_deletes_the_point)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{25, 20.0}, {50, 40.0}, {75, 70.0}});
    CHECK(f.controller.removePoint("monitor-a", 50));
    CHECK(f.payloadHas(R"({"monitor":25,"builtin":20},{"monitor":75,"builtin":70})"));
    CHECK(!f.payloadHas(R"("monitor":50)"));
    CHECK_EQ(joined(f.log), std::string("submit"));
    CHECK(!f.controller.isActive());
}

TEST(remove_point_is_refused_for_the_active_point)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{25, 20.0}, {50, 40.0}, {75, 70.0}});
    f.controller.begin("monitor-a", 50);
    f.log.clear();
    CHECK(!f.controller.removePoint("monitor-a", 50));
    CHECK(f.log.empty());
    CHECK_EQ(f.settings.submits, 0);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 50 40"));
    CHECK(f.controller.removePoint("monitor-a", 25));
    CHECK(f.payloadHas(R"({"monitor":50,"builtin":40},{"monitor":75,"builtin":70})"));
    CHECK_EQ(joined(f.log), std::string("submit"));
}

TEST(removing_below_two_points_makes_it_incomplete)
{
    AmbientCurve curve = makeCurve({{25, 20.0}, {50, 40.0}, {75, 70.0}});
    curve.remove(75);
    CHECK(curve.isComplete());
    curve.remove(50);
    CHECK(!curve.isComplete());
}

TEST(begin_ignores_points_outside_one_to_hundred)
{
    for (const int point : {0, 101}) {
        Fixture f;
        f.controller.begin("monitor-a", point);
        CHECK(!f.controller.isActive());
        CHECK(f.log.empty());
        CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
        CHECK(!f.session.pending().has_value());
        CHECK(f.settings.stored.empty());
    }
}

TEST(begin_on_a_new_point_is_ignored_at_thirty_two_points)
{
    Fixture f;
    AmbientCurve full;
    for (int point = 1; point <= 32; ++point) {
        full.points.push_back(AmbientCurvePoint{static_cast<double>(point), static_cast<double>(point)});
    }
    f.settings.stored["monitor-a"] = full;
    f.controller.begin("monitor-a", 50);
    CHECK(!f.controller.isActive());
    CHECK(f.log.empty());
    CHECK_EQ(f.settings.stored["monitor-a"].points.size(), static_cast<std::size_t>(32));
    f.controller.begin("monitor-a", 16);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 16 16"));
    CHECK_EQ(f.settings.stored["monitor-a"].points.size(), static_cast<std::size_t>(32));
}

TEST(anchor_reached_follows_the_monitor_level)
{
    Fixture f;
    CHECK(!f.controller.anchorReached("monitor-a"));
    f.controller.begin("monitor-a", 50);
    CHECK(!f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 51.0;
    CHECK(f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 49.0;
    CHECK(f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 53.0;
    CHECK(!f.controller.anchorReached("monitor-a"));
    f.monitors.observed.erase("monitor-a");
    CHECK(!f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 50.0;
    f.monitors.observed["monitor-b"] = 50.0;
    CHECK(!f.controller.anchorReached("monitor-b"));
    f.controller.end(CalibrationEndReason::Done);
    CHECK(!f.controller.anchorReached("monitor-a"));
}

TEST(anchor_reached_uses_the_low_point_level)
{
    Fixture f;
    f.controller.begin("monitor-a", 5);
    f.monitors.observed["monitor-a"] = 5.0;
    CHECK(f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 6.5;
    CHECK(f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 10.0;
    CHECK(!f.controller.anchorReached("monitor-a"));
}

TEST(anchor_reached_uses_the_monitor_one_level)
{
    Fixture f;
    f.controller.begin("monitor-a", 1);
    f.monitors.observed["monitor-a"] = 1.0;
    CHECK(f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 2.5;
    CHECK(f.controller.anchorReached("monitor-a"));
    f.monitors.observed["monitor-a"] = 5.0;
    CHECK(!f.controller.anchorReached("monitor-a"));
}

TEST(every_end_reason_restores_in_order)
{
    const CalibrationEndReason reasons[] = {
        CalibrationEndReason::Done,         CalibrationEndReason::Cancelled,
        CalibrationEndReason::LeftTab,      CalibrationEndReason::WindowClosed,
        CalibrationEndReason::AppQuit,      CalibrationEndReason::DisplaySleep,
        CalibrationEndReason::BuiltInUnavailable,
    };
    for (const CalibrationEndReason reason : reasons) {
        Fixture f;
        f.controller.begin("monitor-a", 25);
        f.log.clear();
        f.controller.end(reason);
        CHECK_EQ(joined(f.log), std::string("writeLevel 44; adaptive ac 1; adaptive dc 1; "
                                            "setMonitor monitor-a 62; setCalibrating false"));
        CHECK(!f.session.pending().has_value());
        CHECK(!f.controller.isActive());
    }
}

TEST(end_skips_automatic_brightness_when_it_was_off)
{
    Fixture f(0, 0);
    f.controller.begin("monitor-a", 25);
    f.log.clear();
    f.controller.end(CalibrationEndReason::Done);
    CHECK_EQ(joined(f.log), std::string("writeLevel 44; setMonitor monitor-a 62; setCalibrating false"));
    CHECK_EQ(f.power.ac(), static_cast<uint32_t>(0));
}

TEST(end_skips_monitor_when_its_brightness_was_unknown)
{
    Fixture f;
    f.monitors.observed.erase("monitor-a");
    f.settings.stored["monitor-a"] = makeCurve({{25, 20.0}});
    f.controller.begin("monitor-a", 25);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 25 20"));
    f.log.clear();
    f.controller.end(CalibrationEndReason::Done);
    CHECK_EQ(joined(f.log), std::string("writeLevel 44; adaptive ac 1; adaptive dc 1; setCalibrating false"));
}

TEST(begin_on_another_monitor_restores_first)
{
    Fixture f;
    f.controller.begin("monitor-a", 25);
    f.log.clear();
    f.controller.begin("monitor-b", 50);
    CHECK_EQ(joined(f.log), std::string("writeLevel 44; adaptive ac 1; adaptive dc 1; "
                                        "setMonitor monitor-a 62; setCalibrating false; "
                                        "adaptive ac 0; adaptive dc 0; setCalibrating true; "
                                        "setMonitor monitor-b 50; writeLevel 44"));
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-b 50 44"));
    CHECK(f.session.pending() == capturedState());
}

TEST(unreadable_built_in_level_starts_nothing)
{
    Fixture f(1, 1, std::nullopt);
    f.controller.begin("monitor-a", 25);
    CHECK(!f.controller.isActive());
    CHECK_EQ(noticeName(f.controller.notice()), std::string("builtInUnreadable"));
    CHECK(!f.session.pending().has_value());
    CHECK(f.log.empty());
}

TEST(begin_is_refused_while_the_builtin_screen_is_unavailable)
{
    Fixture f;
    f.follow.paused_ = true;
    f.controller.begin("monitor-a", 25);
    CHECK_EQ(noticeName(f.controller.notice()), std::string("builtInUnavailable"));
    CHECK(!f.controller.isActive());
    CHECK(f.log.empty());
    CHECK(!f.session.pending().has_value());
}

TEST(begin_is_refused_while_the_builtin_screen_is_asleep)
{
    // Display sleep and a closed lid both pause follow, so both reach begin as paused.
    Fixture f;
    f.follow.paused_ = true;
    f.controller.begin("monitor-a", 25);
    CHECK_EQ(noticeName(f.controller.notice()), std::string("builtInUnavailable"));
    CHECK(!f.controller.isActive());
    CHECK(f.log.empty());
    CHECK(!f.session.pending().has_value());
}

TEST(unknown_automatic_brightness_still_calibrates)
{
    Fixture f;
    // A failed read of the power settings writes nothing and records nothing: the value could not be checked.
    f.power.failReads = true;
    f.controller.begin("monitor-a", 25);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 25 44"));
    CHECK_EQ(noticeName(f.controller.notice()), std::string("autoBrightnessUnknown"));
    CHECK_EQ(joined(f.log), std::string("setCalibrating true; setMonitor monitor-a 25; writeLevel 44"));
    CHECK(!f.session.pending().has_value());
    f.log.clear();
    f.controller.end(CalibrationEndReason::Done);
    CHECK_EQ(joined(f.log), std::string("writeLevel 44; setMonitor monitor-a 62; setCalibrating false"));
}

TEST(automatic_brightness_refusal_still_calibrates)
{
    Fixture f;
    // The write is refused, so the rollback is refused too and the record stays with the saved values.
    f.power.failWrites = true;
    f.controller.begin("monitor-a", 25);
    CHECK_EQ(describe(f.controller.active()), std::string("monitor-a 25 44"));
    CHECK_EQ(noticeName(f.controller.notice()), std::string("autoBrightnessNotChanged"));
    CHECK_EQ(joined(f.log), std::string("adaptive ac 0; adaptive dc 0; adaptive ac 1; adaptive dc 1; "
                                        "setCalibrating true; setMonitor monitor-a 25; writeLevel 44"));
    CHECK(f.session.pending() == capturedState());
}

TEST(failed_first_write_restores_and_reports)
{
    Fixture f;
    // A stored point makes the first Laptop write 20, so the failing write is not the captured level.
    f.settings.stored["monitor-a"] = makeCurve({{25, 20.0}});
    f.panel.failedLevels = {20.0};
    f.controller.begin("monitor-a", 25);
    CHECK(!f.controller.isActive());
    CHECK_EQ(noticeName(f.controller.notice()), std::string("builtInWriteFailed"));
    CHECK(!f.session.pending().has_value());
    CHECK_EQ(joined(f.log), std::string("adaptive ac 0; adaptive dc 0; setCalibrating true; "
                                        "setMonitor monitor-a 25; writeLevel 20; writeLevel 44; "
                                        "adaptive ac 1; adaptive dc 1; setMonitor monitor-a 62; setCalibrating false"));
    CHECK_EQ(f.power.ac(), static_cast<uint32_t>(1));
    CHECK_EQ(f.power.dc(), static_cast<uint32_t>(1));
}

TEST(failed_slider_write_ends_the_session_and_restores)
{
    Fixture f;
    f.controller.begin("monitor-a", 50);
    f.panel.failedLevels = {30.0};
    f.log.clear();
    f.controller.setSlider(30);
    CHECK_EQ(joined(f.log), std::string("writeLevel 30; writeLevel 44; adaptive ac 1; adaptive dc 1; "
                                        "setMonitor monitor-a 62; setCalibrating false"));
    CHECK(!f.controller.isActive());
    CHECK_EQ(noticeName(f.controller.notice()), std::string("builtInWriteFailed"));
    CHECK(!f.session.pending().has_value());
    CHECK_EQ(f.power.ac(), static_cast<uint32_t>(1));
}

TEST(end_builtin_unavailable_reports_the_notice_when_a_session_was_running)
{
    Fixture f;
    f.controller.begin("monitor-a", 25);
    f.log.clear();
    f.controller.end(CalibrationEndReason::BuiltInUnavailable);
    CHECK_EQ(joined(f.log), std::string("writeLevel 44; adaptive ac 1; adaptive dc 1; "
                                        "setMonitor monitor-a 62; setCalibrating false"));
    CHECK_EQ(noticeName(f.controller.notice()), std::string("builtInUnavailable"));
    CHECK(!f.controller.isActive());
    CHECK(!f.session.pending().has_value());
}

TEST(end_builtin_unavailable_without_a_session_sets_no_notice)
{
    Fixture f;
    f.controller.end(CalibrationEndReason::BuiltInUnavailable);
    CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
    CHECK(f.log.empty());
}

TEST(other_end_reasons_leave_no_notice)
{
    const CalibrationEndReason reasons[] = {
        CalibrationEndReason::Done,      CalibrationEndReason::Cancelled,
        CalibrationEndReason::LeftTab,   CalibrationEndReason::WindowClosed,
        CalibrationEndReason::AppQuit,   CalibrationEndReason::DisplaySleep,
    };
    for (const CalibrationEndReason reason : reasons) {
        Fixture f;
        f.controller.begin("monitor-a", 25);
        f.controller.end(reason);
        CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
    }
}

TEST(restore_failure_keeps_session_file_and_reports)
{
    Fixture f;
    f.controller.begin("monitor-a", 25);
    f.power.failWrites = true;
    f.controller.end(CalibrationEndReason::Done);
    CHECK_EQ(noticeName(f.controller.notice()), std::string("autoBrightnessNotChanged"));
    CHECK(f.session.pending() == capturedState());
    CHECK(!f.controller.isActive());
    // A later launch with a working power API restores the values from the kept record.
    f.power.failWrites = false;
    CHECK(f.controller.recoverPending());
    CHECK(!f.session.pending().has_value());
    CHECK_EQ(f.power.ac(), static_cast<uint32_t>(1));
}

TEST(persist_failure_starts_nothing_and_reports_recovery_failed)
{
    Fixture f;
    // An unrecovered record blocks a new one, so the session file cannot be written.
    CHECK(f.session.begin(capturedState()));
    f.log.clear();
    f.controller.begin("monitor-a", 25);
    CHECK(!f.controller.isActive());
    CHECK_EQ(noticeName(f.controller.notice()), std::string("recoveryFailed"));
    CHECK(f.log.empty());
    CHECK_EQ(f.power.ac(), static_cast<uint32_t>(1));
}

TEST(save_refuses_a_curve_outside_the_core_limits)
{
    Fixture f;
    f.settings.stored["monitor-a"] = makeCurve({{150, 10.0}});
    f.controller.begin("monitor-a", 50);
    f.log.clear();
    CHECK(!f.controller.confirm());
    CHECK_EQ(f.settings.submits, 0);
    CHECK(f.log.empty());
}

TEST(payload_splices_every_monitor_curve_into_update_settings)
{
    AmbientCurveMap curves;
    curves["monitor-a"] = makeCurve({{25, 20.0}, {50, 40.0}});
    curves["monitor-b"] = makeCurve({{10, 8.0}});
    const std::string json = telaviva::buildCalibrationPayload(telaviva::SettingsDraft{}, curves);
    CHECK(contains(json, R"({"type":"update_settings","settings":{)"));
    CHECK(contains(json, R"(,"ambient_curves":{"monitor-a":{"points":[{"monitor":25,"builtin":20},)"
                         R"({"monitor":50,"builtin":40}]},"monitor-b":{"points":[{"monitor":10,"builtin":8}]}}})"));
    CHECK(json.size() >= 2 && json.compare(json.size() - 2, 2, "}}") == 0);
}

TEST(recovery_restores_leftover_session)
{
    // The crashed run left adaptive brightness off, so the power values are 0 until the restore.
    Fixture f(0, 0);
    CHECK(f.session.begin(capturedState()));
    f.log.clear();
    CHECK(f.controller.recoverPending());
    CHECK_EQ(joined(f.log), std::string("adaptive ac 1; adaptive dc 1"));
    CHECK(!f.session.pending().has_value());
    CHECK(!f.controller.isActive());
    CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
}

TEST(recovery_with_disconnected_monitor_skips_monitor)
{
    // The record holds no monitor or panel state, so recovery writes only the power values.
    Fixture f(0, 0);
    f.monitors.observed.erase("monitor-a");
    CHECK(f.session.begin(capturedState()));
    f.log.clear();
    CHECK(f.controller.recoverPending());
    CHECK_EQ(joined(f.log), std::string("adaptive ac 1; adaptive dc 1"));
    CHECK(!f.session.pending().has_value());
}

TEST(recovery_with_automatic_brightness_off_leaves_it_off)
{
    Fixture f(0, 0);
    SavedAdaptiveState off = capturedState();
    off.acValue = 0;
    off.dcValue = 0;
    CHECK(f.session.begin(off));
    CHECK(f.controller.recoverPending());
    CHECK_EQ(f.power.ac(), static_cast<uint32_t>(0));
    CHECK_EQ(f.power.dc(), static_cast<uint32_t>(0));
    CHECK(!f.session.pending().has_value());
}

TEST(recovery_reports_unreadable_file)
{
    // A corrupt file is quarantined by CalibrationSession and counts as none, so nothing is reported.
    Fixture f;
    {
        std::ofstream out(f.session.file(), std::ios::binary | std::ios::trunc);
        out << "not a session";
    }
    f.log.clear();
    CHECK(f.controller.recoverPending());
    CHECK(f.log.empty());
    CHECK(!f.session.pending().has_value());
    CHECK(fs::exists(f.dir.path() / "calibration-session.json.corrupt"));
    CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
}

TEST(recovery_without_file_does_nothing)
{
    Fixture f;
    CHECK(f.controller.recoverPending());
    CHECK(f.log.empty());
    CHECK_EQ(noticeName(f.controller.notice()), std::string("none"));
}

TEST(pause_follow_is_set_on_begin_and_cleared_on_end)
{
    Fixture f;
    f.controller.begin("monitor-a", 25);
    CHECK(f.follow.calibrating_);
    f.controller.end(CalibrationEndReason::Done);
    CHECK(!f.follow.calibrating_);
}

TEST(suspend_runs_before_the_follow_pause)
{
    // Adaptive brightness is off by the time follow is paused, so the follower never reads a moving level.
    Fixture f;
    f.controller.begin("monitor-a", 25);
    const std::string events = joined(f.log);
    CHECK(events.find("adaptive ac 0") < events.find("setCalibrating true"));
}
