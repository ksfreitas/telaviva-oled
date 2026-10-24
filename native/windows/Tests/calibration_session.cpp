#include "TestHarness.h"

#include "../Platform/AdaptiveBrightness.h"
#include "../Platform/CalibrationSession.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using telaviva::AdaptiveBrightness;
using telaviva::CalibrationRecord;
using telaviva::CalibrationSession;
using telaviva::IPowerSettings;
using telaviva::PowerSource;
using telaviva::PowerStatus;
using telaviva::SavedAdaptiveState;
using telaviva::SavedMonitorState;
using telaviva::SuspendResult;

namespace {

// Unique per call; the destructor removes the directory and everything in it.
class TempDir {
public:
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("telaviva-calibration-" + std::to_string(stamp) + "-" + std::to_string(std::random_device{}()));
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

std::string readText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

const std::string kScheme = "381b4222-f694-41f0-9685-ff5bb260df2e";

// Minimal in-memory power API, the same shape as the one in adaptive_brightness.cpp.
class FakePower final : public IPowerSettings {
public:
    struct Scheme {
        uint32_t ac = 0;
        uint32_t dc = 0;
    };

    PowerStatus activeScheme(std::string& schemeGuid) override
    {
        schemeGuid = active;
        return PowerStatus::Ok;
    }

    PowerStatus readAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t& value) override
    {
        const auto it = schemes.find(schemeGuid);
        if (it == schemes.end()) {
            return PowerStatus::NotFound;
        }
        value = source == PowerSource::Ac ? it->second.ac : it->second.dc;
        return PowerStatus::Ok;
    }

    PowerStatus writeAdaptive(const std::string& schemeGuid, PowerSource source, uint32_t value) override
    {
        const auto it = schemes.find(schemeGuid);
        if (it == schemes.end()) {
            return PowerStatus::NotFound;
        }
        if (failWrites) {
            return PowerStatus::Failed;
        }
        if (source == PowerSource::Ac) {
            it->second.ac = value;
        } else {
            it->second.dc = value;
        }
        return PowerStatus::Ok;
    }

    PowerStatus setActiveScheme(const std::string& schemeGuid) override
    {
        if (schemes.find(schemeGuid) == schemes.end()) {
            return PowerStatus::NotFound;
        }
        active = schemeGuid;
        return PowerStatus::Ok;
    }

    std::map<std::string, Scheme> schemes;
    std::string active;
    bool failWrites = false;
};

FakePower makeLaptop()
{
    FakePower power;
    power.schemes[kScheme] = FakePower::Scheme{1, 1};
    power.active = kScheme;
    return power;
}

SavedAdaptiveState sampleState()
{
    SavedAdaptiveState state;
    state.schemeGuid = kScheme;
    state.acValue = 1;
    state.dcValue = 1;
    return state;
}

}  // namespace

TEST(calibration_session_missing_file_has_no_pending_state)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CHECK(!session.pending().has_value());
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    CHECK(session.recover(adaptive));
    CHECK(!fs::exists(session.file()));
}

TEST(calibration_session_begin_writes_file_and_pending_reads_it)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CHECK(session.begin(sampleState()));
    CHECK(fs::exists(session.file()));
    const std::optional<SavedAdaptiveState> pending = session.pending();
    CHECK(pending.has_value());
    if (pending) {
        CHECK_EQ(pending->schemeGuid, kScheme);
        CHECK_EQ(pending->acValue, static_cast<uint32_t>(1));
        CHECK_EQ(pending->dcValue, static_cast<uint32_t>(1));
    }
}

TEST(calibration_session_file_has_schema_1_and_saved_state)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CHECK(session.begin(sampleState()));
    const std::string text = readText(session.file());
    const bool hasSchema = text.find(R"("schema":1)") != std::string::npos;
    const bool hasScheme = text.find(R"("scheme":"381b4222-f694-41f0-9685-ff5bb260df2e")") != std::string::npos;
    const bool hasStarted = text.find(R"("started_at":")") != std::string::npos;
    CHECK(hasSchema);
    CHECK(hasScheme);
    CHECK(hasStarted);
    CHECK(!fs::exists(dir.path() / "calibration-session.json.tmp"));
}

TEST(calibration_session_end_deletes_file)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CHECK(session.begin(sampleState()));
    session.end();
    CHECK(!fs::exists(session.file()));
    CHECK(!session.pending().has_value());
}

TEST(calibration_session_begin_refuses_to_overwrite_unrecovered_record)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CHECK(session.begin(sampleState()));
    SavedAdaptiveState other = sampleState();
    other.acValue = 7;
    CHECK(!session.begin(other));
    const std::optional<SavedAdaptiveState> pending = session.pending();
    CHECK(pending.has_value());
    if (pending) {
        CHECK_EQ(pending->acValue, static_cast<uint32_t>(1));
    }
}

TEST(calibration_session_begin_rejects_invalid_scheme)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    SavedAdaptiveState bad = sampleState();
    bad.schemeGuid = "not-a-scheme";
    CHECK(!session.begin(bad));
    CHECK(!fs::exists(session.file()));
}

TEST(calibration_session_crash_recovery_restores_and_clears_file)
{
    const TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    {
        // The first run turns adaptive brightness off, then "crashes" before ending the session.
        CalibrationSession session(dir.path());
        SavedAdaptiveState saved;
        const SuspendResult result = adaptive.suspend(saved, [&session](const SavedAdaptiveState& state) {
            return session.begin(state);
        });
        CHECK(result == SuspendResult::Suspended);
    }
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(0));

    // The next launch finds the record and restores the values.
    CalibrationSession relaunch(dir.path());
    CHECK(relaunch.pending().has_value());
    CHECK(relaunch.recover(adaptive));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kScheme].dc, static_cast<uint32_t>(1));
    CHECK(!fs::exists(relaunch.file()));
}

TEST(calibration_session_corrupt_file_is_renamed_and_treated_as_none)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), "{not json");
    CHECK(!session.pending().has_value());
    CHECK(!fs::exists(session.file()));
    const fs::path corrupt = dir.path() / "calibration-session.json.corrupt";
    CHECK(fs::exists(corrupt));
    CHECK_EQ(readText(corrupt), std::string("{not json"));
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    CHECK(session.recover(adaptive));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
}

TEST(calibration_session_wrong_schema_is_treated_as_corrupt)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), R"({"schema":2,"adaptive_brightness":{"scheme":"381b4222-f694-41f0-9685-ff5bb260df2e","ac":1,"dc":1},"started_at":"2026-10-17T14:30:00Z"})");
    CHECK(!session.pending().has_value());
    CHECK(!fs::exists(session.file()));
    CHECK(fs::exists(dir.path() / "calibration-session.json.corrupt"));
}

TEST(calibration_session_missing_field_is_treated_as_corrupt)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), R"({"schema":1,"adaptive_brightness":{"scheme":"381b4222-f694-41f0-9685-ff5bb260df2e","ac":1},"started_at":"2026-10-17T14:30:00Z"})");
    CHECK(!session.pending().has_value());
    CHECK(fs::exists(dir.path() / "calibration-session.json.corrupt"));
}

TEST(calibration_session_old_format_record_loads_without_monitor_fields)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), R"({"schema":1,"adaptive_brightness":{"scheme":"381b4222-f694-41f0-9685-ff5bb260df2e","ac":1,"dc":1},"started_at":"2026-10-17T14:30:00Z"})");
    const std::optional<CalibrationRecord> record = session.pendingRecord();
    CHECK(record.has_value());
    if (record) {
        CHECK(record->adaptiveValid);
        CHECK(!record->monitor.has_value());
        CHECK(!record->panelLevel.has_value());
        CHECK_EQ(record->adaptive.acValue, static_cast<uint32_t>(1));
    }
    CHECK(session.pending().has_value());
    CHECK(!fs::exists(dir.path() / "calibration-session.json.corrupt"));
}

TEST(calibration_session_optional_monitor_and_panel_fields_parse)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), R"({"schema":1,"adaptive_brightness":{"scheme":"381b4222-f694-41f0-9685-ff5bb260df2e","ac":1,"dc":1},"monitor_id":"monitor-a","monitor_percent":62,"panel_level":44,"started_at":"2026-10-17T14:30:00Z"})");
    const std::optional<CalibrationRecord> record = session.pendingRecord();
    CHECK(record.has_value());
    if (record) {
        CHECK(record->adaptiveValid);
        CHECK(record->monitor.has_value());
        if (record->monitor) {
            CHECK_EQ(record->monitor->monitorId, std::string("monitor-a"));
            CHECK_EQ(record->monitor->percent, 62.0);
        }
        CHECK(record->panelLevel.has_value());
        if (record->panelLevel) {
            CHECK_EQ(*record->panelLevel, 44.0);
        }
    }
}

TEST(calibration_session_monitor_only_record_parses_and_is_not_adaptive)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), R"({"schema":1,"monitor_id":"monitor-a","monitor_percent":62,"started_at":"2026-10-17T14:30:00Z"})");
    const std::optional<CalibrationRecord> record = session.pendingRecord();
    CHECK(record.has_value());
    if (record) {
        CHECK(!record->adaptiveValid);
        CHECK(record->monitor.has_value());
    }
    CHECK(!session.pending().has_value());
    CHECK(!fs::exists(dir.path() / "calibration-session.json.corrupt"));
}

TEST(calibration_session_monitor_id_without_percent_is_treated_as_corrupt)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    writeText(session.file(), R"({"schema":1,"adaptive_brightness":{"scheme":"381b4222-f694-41f0-9685-ff5bb260df2e","ac":1,"dc":1},"monitor_id":"monitor-a","started_at":"2026-10-17T14:30:00Z"})");
    CHECK(!session.pendingRecord().has_value());
    CHECK(fs::exists(dir.path() / "calibration-session.json.corrupt"));
}

TEST(calibration_session_recover_drops_the_adaptive_part_and_keeps_the_monitor)
{
    const TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    CalibrationSession session(dir.path());
    CalibrationRecord record;
    record.adaptive = sampleState();
    record.panelLevel = 44.0;
    record.monitor = SavedMonitorState{"monitor-a", 62.0};
    CHECK(session.begin(record));
    power.schemes[kScheme] = FakePower::Scheme{0, 0};
    CHECK(session.recover(adaptive));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
    CHECK_EQ(power.schemes[kScheme].dc, static_cast<uint32_t>(1));
    // Only the monitor part is left, and the adaptive values are not in the file.
    const std::string text = readText(session.file());
    CHECK(text.find("adaptive_brightness") == std::string::npos);
    CHECK(text.find(R"("monitor_id":"monitor-a")") != std::string::npos);
    const std::optional<CalibrationRecord> kept = session.pendingRecord();
    CHECK(kept.has_value());
    if (kept) {
        CHECK(!kept->adaptiveValid);
        CHECK(!kept->panelLevel.has_value());
        CHECK(kept->monitor.has_value());
    }
    // A second recovery restores nothing and keeps the monitor part.
    power.schemes[kScheme] = FakePower::Scheme{0, 0};
    CHECK(session.recover(adaptive));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(0));
    CHECK(session.pendingRecord().has_value());
}

TEST(calibration_session_begin_with_monitor_and_panel_writes_them)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CalibrationRecord record;
    record.adaptive = sampleState();
    record.panelLevel = 44.0;
    record.monitor = SavedMonitorState{"monitor-a", 62.0};
    CHECK(session.begin(record));
    const std::string text = readText(session.file());
    CHECK(text.find(R"("monitor_id":"monitor-a")") != std::string::npos);
    CHECK(text.find(R"("monitor_percent":62)") != std::string::npos);
    CHECK(text.find(R"("panel_level":44)") != std::string::npos);
    CHECK(session.pending() == sampleState());
}

TEST(calibration_session_rewrite_replaces_the_record)
{
    const TempDir dir;
    CalibrationSession session(dir.path());
    CHECK(session.begin(sampleState()));
    CalibrationRecord monitorOnly;
    monitorOnly.adaptiveValid = false;
    monitorOnly.monitor = SavedMonitorState{"monitor-b", 30.0};
    CHECK(session.rewrite(monitorOnly));
    CHECK(!session.pending().has_value());
    const std::optional<CalibrationRecord> record = session.pendingRecord();
    CHECK(record.has_value());
    if (record) {
        CHECK(!record->adaptiveValid);
        CHECK(record->monitor.has_value());
        if (record->monitor) {
            CHECK_EQ(record->monitor->monitorId, std::string("monitor-b"));
        }
    }
}

TEST(calibration_session_recovery_failure_keeps_file_for_next_launch)
{
    const TempDir dir;
    FakePower power = makeLaptop();
    AdaptiveBrightness adaptive(power);
    CalibrationSession session(dir.path());
    SavedAdaptiveState saved;
    CHECK(adaptive.suspend(saved, [&session](const SavedAdaptiveState& state) {
        return session.begin(state);
    }) == SuspendResult::Suspended);

    power.failWrites = true;
    CHECK(!session.recover(adaptive));
    CHECK(fs::exists(session.file()));
    CHECK(session.pending().has_value());

    // A later launch with a working power API restores and clears the record.
    power.failWrites = false;
    CHECK(session.recover(adaptive));
    CHECK(!fs::exists(session.file()));
    CHECK_EQ(power.schemes[kScheme].ac, static_cast<uint32_t>(1));
}
