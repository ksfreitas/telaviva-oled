// Tests for the JSON command builders and the snapshot parser. The builders are
// checked structurally after dropping whitespace, so formatting stays free to change.
#include <windows.h>

#include "TestHarness.h"
#include "Platform/JsonCodec.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// Removes whitespace outside string literals so checks ignore formatting.
std::string compact(const std::string& json)
{
    std::string out;
    bool inString = false;
    bool escaped = false;
    for (const char c : json) {
        if (inString) {
            out.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
            out.push_back(c);
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(c);
        }
    }
    return out;
}

bool contains(const std::string& json, const std::string& fragment)
{
    return compact(json).find(fragment) != std::string::npos;
}

bool nearly(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

bool nearlyOpt(const std::optional<double>& value, double expected)
{
    return value.has_value() && nearly(*value, expected);
}

// Fixtures are captured from the real core DLL. The test main may set
// TELAVIVA_TEST_FIXTURES; otherwise the directory is resolved from the exe location.
std::filesystem::path fixturesDirectory()
{
    char* override = nullptr;
    size_t length = 0;
    if (_dupenv_s(&override, &length, "TELAVIVA_TEST_FIXTURES") == 0 && override != nullptr) {
        const std::filesystem::path dir(override);
        std::free(override);
        return dir;
    }
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return std::filesystem::path(buffer).parent_path() / L".." / L".." / L".." / L"Tests" / L"fixtures";
}

std::string readFixture(const char* name)
{
    std::ifstream in(fixturesDirectory() / name, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Parse failures are reported as a failed check so the run continues.
telaviva::Snapshot parseFixture(const char* name)
{
    try {
        return telaviva::parseSnapshot(readFixture(name));
    } catch (const std::exception& error) {
        std::printf("%s: %s\n", name, error.what());
        CHECK(false);
        return telaviva::Snapshot{};
    }
}

}  // namespace

TEST(json_codec_monitor_discovered_fields)
{
    telaviva::MonitorInfo monitor;
    monitor.id = "win-GSM-23456-873459-1f3a9c2b";
    monitor.name = "LG ULTRAFINE";
    monitor.identityStable = true;
    // Raw path with real backslashes; the codec must escape each one in the JSON output.
    monitor.connectionPath = R"(\\?\DISPLAY#GSM5BC0#4&1a2b3c&0&UID4352)";
    monitor.brightness.supported = true;
    monitor.brightness.raw = 41.0;
    monitor.volume.supported = true;
    monitor.volume.raw = 30.0;

    const std::string json = telaviva::buildMonitorDiscovered(monitor);
    CHECK(contains(json, R"("type":"monitor_discovered")"));
    CHECK(contains(json, R"("id":"win-GSM-23456-873459-1f3a9c2b")"));
    CHECK(contains(json, R"("name":"LG ULTRAFINE")"));
    CHECK(contains(json, R"("identity_status":"stable")"));
    CHECK(contains(json, R"("connection_path":"\\\\?\\DISPLAY#GSM5BC0#4&1a2b3c&0&UID4352")"));
    CHECK(contains(json, R"("brightness":{)"));
    CHECK(contains(json, R"("raw":41)"));
    CHECK(contains(json, R"("raw":30)"));
}

TEST(json_codec_monitor_discovered_unsupported_feature)
{
    telaviva::MonitorInfo monitor;
    monitor.id = "win-GSM-23456-873459-1f3a9c2b";
    monitor.name = "LG ULTRAFINE";
    monitor.identityStable = false;
    monitor.connectionPath = "DISPLAY#GSM5BC0";
    monitor.brightness.supported = false;
    monitor.volume.supported = false;

    const std::string json = telaviva::buildMonitorDiscovered(monitor);
    // The contract sends only the supported flag when the first VCP read fails.
    CHECK(contains(json, R"("brightness":{"supported":false})"));
    CHECK(contains(json, R"("volume":{"supported":false})"));
    CHECK(contains(json, R"("identity_status":"ambiguous")"));
    CHECK(!contains(json, R"("raw")"));
}

TEST(json_codec_monitor_disconnected_fields)
{
    const std::string json = telaviva::buildMonitorDisconnected("win-GSM-23456-873459-1f3a9c2b");
    CHECK(contains(json, R"("type":"monitor_disconnected")"));
    CHECK(contains(json, R"("monitor_id":"win-GSM-23456-873459-1f3a9c2b")"));
}

TEST(json_codec_monitor_display_state_fields)
{
    const std::string json = telaviva::buildMonitorDisplayState("win-GSM-23456-873459-1f3a9c2b", 3, "asleep");
    CHECK(contains(json, R"("type":"monitor_display_state")"));
    CHECK(contains(json, R"("monitor_id":"win-GSM-23456-873459-1f3a9c2b")"));
    CHECK(contains(json, R"("generation":3)"));
    CHECK(contains(json, R"("state":"asleep")"));
}

TEST(json_codec_power_null_when_unknown)
{
    CHECK(contains(telaviva::buildPower(std::nullopt), R"("type":"power")"));
    CHECK(contains(telaviva::buildPower(std::nullopt), R"("on_battery":null)"));
    CHECK(contains(telaviva::buildPower(true), R"("on_battery":true)"));
    CHECK(contains(telaviva::buildPower(false), R"("on_battery":false)"));
}

TEST(json_codec_screensaver_state)
{
    const std::string json = telaviva::buildScreensaver("active");
    CHECK(contains(json, R"("type":"screensaver")"));
    CHECK(contains(json, R"("state":"active")"));
}

TEST(json_codec_presence_observation_fields)
{
    const std::string plain =
        telaviva::buildPresenceObservation("present", 1700000000123ULL, 4, std::nullopt);
    CHECK(contains(plain, R"("type":"presence_observation")"));
    CHECK(contains(plain, R"("status":"present")"));
    CHECK(contains(plain, R"("captured_at_ms":1700000000123)"));
    CHECK(contains(plain, R"("camera_generation":4)"));
    CHECK(!contains(plain, R"("camera_status")"));

    const std::string withStatus =
        telaviva::buildPresenceObservation("unavailable", 1700000000456ULL, 4, std::optional<std::string>{"busy"});
    CHECK(contains(withStatus, R"("status":"unavailable")"));
    CHECK(contains(withStatus, R"("camera_status":"busy")"));
}

TEST(json_codec_camera_status_fields)
{
    const std::string withGeneration = telaviva::buildCameraStatus("busy", 5ULL);
    CHECK(contains(withGeneration, R"("type":"camera_status")"));
    CHECK(contains(withGeneration, R"("status":"busy")"));
    CHECK(contains(withGeneration, R"("generation":5)"));

    const std::string withoutGeneration = telaviva::buildCameraStatus("unknown", std::nullopt);
    CHECK(contains(withoutGeneration, R"("status":"unknown")"));
    CHECK(!contains(withoutGeneration, R"("generation")"));
}

TEST(json_codec_builtin_display_null_brightness)
{
    const std::string unknown = telaviva::buildBuiltinDisplay(true, std::nullopt, false);
    CHECK(contains(unknown, R"("type":"builtin_display")"));
    CHECK(contains(unknown, R"("available":true)"));
    CHECK(contains(unknown, R"("brightness":null)"));
    CHECK(contains(unknown, R"("calibrating":false)"));

    const std::string known = telaviva::buildBuiltinDisplay(true, 42.0, true);
    CHECK(contains(known, R"("brightness":42)"));
    CHECK(contains(known, R"("calibrating":true)"));
}

TEST(json_codec_set_brightness_and_volume_fields)
{
    const std::string brightness = telaviva::buildSetBrightness("win-GSM-23456-873459-1f3a9c2b", 55.0, 7ULL);
    CHECK(contains(brightness, R"("type":"set_brightness")"));
    CHECK(contains(brightness, R"("monitor_id":"win-GSM-23456-873459-1f3a9c2b")"));
    CHECK(contains(brightness, R"("value":55)"));
    CHECK(contains(brightness, R"("connection_generation":7)"));

    const std::string volume = telaviva::buildSetVolume("win-GSM-23456-873459-1f3a9c2b", 30.0, std::nullopt);
    CHECK(contains(volume, R"("type":"set_volume")"));
    CHECK(contains(volume, R"("value":30)"));
    // The core treats a present connection_generation as a strict check, so absence must be omitted.
    CHECK(!contains(volume, R"("connection_generation")"));
}

TEST(json_codec_hardware_result_value_and_error)
{
    const std::string ok = telaviva::buildHardwareResult(42, 6, true, 55.0, std::nullopt);
    CHECK(contains(ok, R"("type":"hardware_result")"));
    CHECK(contains(ok, R"("action_id":42)"));
    CHECK(contains(ok, R"("generation":6)"));
    CHECK(contains(ok, R"("ok":true)"));
    CHECK(contains(ok, R"("value":55)"));
    CHECK(!contains(ok, R"("error")"));

    const std::string failed = telaviva::buildHardwareResult(43, 6, false, std::nullopt, std::optional<std::string>{"io"});
    CHECK(contains(failed, R"("ok":false)"));
    CHECK(contains(failed, R"("error":"io")"));
    CHECK(!contains(failed, R"("value")"));
}

TEST(json_codec_update_settings_wraps_object)
{
    const std::string json = telaviva::buildUpdateSettings(R"({"locale":"pt-BR","volume_key_step":5})");
    CHECK_EQ(compact(json), std::string(R"({"type":"update_settings","settings":{"locale":"pt-BR","volume_key_step":5}})"));
}

TEST(json_codec_parses_basic_snapshot)
{
    const telaviva::Snapshot s = parseFixture("snapshot-basic.json");
    CHECK_EQ(s.revision, uint64_t{1});

    CHECK(!s.settings.presence_protection_enabled);
    CHECK(!s.settings.hardware_keys_enabled);
    CHECK(!s.settings.ambient_follow_enabled);
    CHECK_EQ(s.settings.locale, std::string("auto"));
    CHECK(nearly(s.settings.presence_sampling_interval_s, 10.0));
    CHECK_EQ(s.settings.volume_key_step, 5);
    CHECK(!s.settings.selected_camera_id.has_value());

    CHECK(s.monitors.empty());
    CHECK(s.pendingActions.empty());

    CHECK(!s.ambient.enabled);
    CHECK(!s.ambient.available);
    CHECK(!s.ambient.active);
    CHECK(!s.ambient.calibrating);
    CHECK(!s.ambient.builtinPercent.has_value());

    CHECK_EQ(s.camera.mode, std::string("disabled"));
    CHECK_EQ(s.camera.captureMode, std::string("stopped"));
    CHECK_EQ(s.camera.status, std::string("unknown"));
    CHECK_EQ(s.camera.displayStatus, std::string("disabled"));
    CHECK(!s.camera.nextSampleInMs.has_value());
    CHECK_EQ(s.camera.generation, uint64_t{1});
    CHECK(!s.camera.presence.has_value());

    CHECK(!s.power.onBattery.has_value());
    CHECK_EQ(s.screensaver.state, std::string("unknown"));
}

TEST(json_codec_parses_snapshot_with_action)
{
    const telaviva::Snapshot s = parseFixture("snapshot-with-action.json");
    CHECK_EQ(s.revision, uint64_t{6});

    CHECK_EQ(s.monitors.size(), std::size_t{1});
    if (s.monitors.size() == 1) {
        const auto& m = s.monitors[0];
        CHECK_EQ(m.id, std::string("win-GSM-23456-873459-1f3a9c2b"));
        CHECK_EQ(m.name, std::string("LG ULTRAFINE"));
        CHECK_EQ(m.identityStatus, std::string("stable"));
        // The fixture escapes each backslash and encodes '&' as &.
        CHECK_EQ(m.connectionPath, std::string(R"(\\?\DISPLAY#GSM5BC0#4&1a2b3c&0&UID4352)"));
        CHECK_EQ(m.generation, uint64_t{1});
        CHECK_EQ(m.displayState, std::string("awake"));
        CHECK_EQ(m.follow, std::string("uncalibrated"));
        CHECK(m.protected_);

        CHECK(m.brightness.supported);
        CHECK(nearlyOpt(m.brightness.observed, 41.0));
        CHECK(nearlyOpt(m.brightness.raw, 41.0));
        CHECK(nearly(m.brightness.min, 0.0));
        CHECK(nearly(m.brightness.max, 100.0));
        CHECK_EQ(m.brightness.status, std::string("pending"));

        CHECK(m.volume.supported);
        CHECK(nearlyOpt(m.volume.observed, 30.0));
        CHECK_EQ(m.volume.status, std::string("ready"));
    }

    CHECK_EQ(s.pendingActions.size(), std::size_t{1});
    if (s.pendingActions.size() == 1) {
        const auto& a = s.pendingActions[0];
        CHECK_EQ(a.id, uint64_t{1});
        CHECK_EQ(a.monitorId, std::string("win-GSM-23456-873459-1f3a9c2b"));
        CHECK_EQ(a.generation, uint64_t{6});
        CHECK_EQ(a.kind, std::string("set_brightness"));
        CHECK(nearlyOpt(a.value, 55.0));
        CHECK(!a.target.has_value());
    }

    CHECK(s.power.onBattery.has_value() && !*s.power.onBattery);
    CHECK_EQ(s.screensaver.state, std::string("inactive"));
    CHECK_EQ(s.camera.generation, uint64_t{4});
    CHECK(!s.settings.presence_protection_enabled);
    CHECK_EQ(s.settings.locale, std::string("auto"));
}

TEST(json_codec_inference_device_defaults_to_auto_when_missing)
{
    CHECK_EQ(telaviva::parseSnapshot(R"({"settings":{"locale":"auto"}})").settings.inference_device,
             std::string("auto"));
}

TEST(json_codec_parses_inference_device_values)
{
    CHECK_EQ(telaviva::parseSnapshot(R"({"settings":{"inference_device":"gpu:1002:164e:0"}})")
                 .settings.inference_device,
             std::string("gpu:1002:164e:0"));
    CHECK_EQ(telaviva::parseSnapshot(R"({"settings":{"inference_device":"cpu"}})").settings.inference_device,
             std::string("cpu"));
}

TEST(json_codec_rejects_malformed_json)
{
    bool threw = false;
    try {
        telaviva::parseSnapshot(R"({"revision":)");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// No fixture carries ambient_curves, so these snapshots are inline.
TEST(json_codec_parses_ambient_curves_in_core_order)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"settings":{"ambient_curves":{"monitor-a":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":50.0,"builtin":45.0},{"monitor":75.0,"builtin":70.0},{"monitor":100.0,"builtin":90.0}]},"monitor-b":{"points":[{"monitor":10.0,"builtin":8.0},{"monitor":90.0,"builtin":80.0}]}}}})");
    CHECK_EQ(s.settings.ambientCurves.size(), std::size_t{2});

    const auto a = s.settings.ambientCurves.find("monitor-a");
    CHECK(a != s.settings.ambientCurves.end());
    if (a != s.settings.ambientCurves.end()) {
        CHECK_EQ(a->second.size(), std::size_t{4});
        if (a->second.size() == 4) {
            CHECK(nearly(a->second[0].monitor, 25.0));
            CHECK(nearly(a->second[0].builtin, 20.0));
            CHECK(nearly(a->second[1].monitor, 50.0));
            CHECK(nearly(a->second[2].builtin, 70.0));
            CHECK(nearly(a->second[3].monitor, 100.0));
            CHECK(nearly(a->second[3].builtin, 90.0));
        }
    }

    const auto b = s.settings.ambientCurves.find("monitor-b");
    CHECK(b != s.settings.ambientCurves.end());
    if (b != s.settings.ambientCurves.end()) {
        CHECK_EQ(b->second.size(), std::size_t{2});
        if (b->second.size() == 2) {
            CHECK(nearly(b->second[0].monitor, 10.0));
            CHECK(nearly(b->second[0].builtin, 8.0));
            CHECK(nearly(b->second[1].monitor, 90.0));
            CHECK(nearly(b->second[1].builtin, 80.0));
        }
    }
}

TEST(json_codec_ambient_curves_missing_or_wrong_type_is_empty)
{
    CHECK(telaviva::parseSnapshot(R"({"settings":{"locale":"auto"}})").settings.ambientCurves.empty());
    CHECK(telaviva::parseSnapshot(R"({"settings":{"ambient_curves":null}})").settings.ambientCurves.empty());
    CHECK(telaviva::parseSnapshot(R"({"settings":{"ambient_curves":"oops"}})").settings.ambientCurves.empty());
    CHECK(telaviva::parseSnapshot(R"({"settings":{"ambient_curves":[]}})").settings.ambientCurves.empty());
    CHECK(telaviva::parseSnapshot(R"({"revision":1})").settings.ambientCurves.empty());
}

TEST(json_codec_ambient_curves_skips_malformed_entries)
{
    // "bad-monitor" has no points array and "bad-value" is not an object, so both are dropped.
    // In "good", the point with a string monitor is dropped and the two valid points survive.
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"settings":{"ambient_curves":{"bad-monitor":{"points":"oops"},"bad-value":"not-an-object","good":{"points":[{"monitor":25.0,"builtin":20.0},{"monitor":"50","builtin":45.0},{"monitor":100.0,"builtin":90.0}]}}}})");
    CHECK_EQ(s.settings.ambientCurves.size(), std::size_t{1});
    CHECK(s.settings.ambientCurves.find("bad-monitor") == s.settings.ambientCurves.end());
    CHECK(s.settings.ambientCurves.find("bad-value") == s.settings.ambientCurves.end());

    const auto good = s.settings.ambientCurves.find("good");
    CHECK(good != s.settings.ambientCurves.end());
    if (good != s.settings.ambientCurves.end()) {
        CHECK_EQ(good->second.size(), std::size_t{2});
        if (good->second.size() == 2) {
            CHECK(nearly(good->second[0].monitor, 25.0));
            CHECK(nearly(good->second[1].builtin, 90.0));
        }
    }
}
