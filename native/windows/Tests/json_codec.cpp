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
#include <utility>

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

TEST(json_codec_monitor_discovered_primary_true)
{
    telaviva::MonitorInfo monitor;
    monitor.id = "win-GSM-23456-873459-1f3a9c2b";
    monitor.name = "LG ULTRAFINE";
    monitor.primary = true;

    const std::string json = telaviva::buildMonitorDiscovered(monitor);
    CHECK(contains(json, R"("primary":true)"));
}

TEST(json_codec_monitor_discovered_primary_false)
{
    telaviva::MonitorInfo monitor;
    monitor.id = "win-GSM-23456-873459-1f3a9c2b";
    monitor.name = "LG ULTRAFINE";

    const std::string json = telaviva::buildMonitorDiscovered(monitor);
    CHECK(contains(json, R"("primary":false)"));
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

TEST(json_codec_builtin_display_protection_flags_true)
{
    const std::string json = telaviva::buildBuiltinDisplay(true, 40.0, false, true, true);
    CHECK(contains(json, R"("protectable":true)"));
    CHECK(contains(json, R"("holding":true)"));
    CHECK(contains(json, R"("available":true)"));
    CHECK(contains(json, R"("brightness":40)"));
    CHECK(contains(json, R"("calibrating":false)"));
}

TEST(json_codec_builtin_display_legacy_call_writes_flags_false)
{
    const std::string json = telaviva::buildBuiltinDisplay(false, std::nullopt, true);
    CHECK(contains(json, R"("protectable":false)"));
    CHECK(contains(json, R"("holding":false)"));
    CHECK(contains(json, R"("available":false)"));
    CHECK(contains(json, R"("brightness":null)"));
    CHECK(contains(json, R"("calibrating":true)"));
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

// The snapshot object builtin_protection is read field by field; every key is set.
TEST(json_codec_parses_builtin_protection_fields)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"builtin_protection":{"protectable":true,"protected":true,"available":true,"reasons":["presence","screensaver"],"target_percent":12.5,"hold_auto_brightness":true,"holding":true}})");
    const telaviva::BuiltinProtectionSnapshot& b = s.builtinProtection;
    CHECK(b.protectable);
    CHECK(b.protected_);
    CHECK(b.available);
    CHECK_EQ(b.reasons.size(), std::size_t{2});
    if (b.reasons.size() == 2) {
        CHECK_EQ(b.reasons[0], std::string("presence"));
        CHECK_EQ(b.reasons[1], std::string("screensaver"));
    }
    CHECK(nearlyOpt(b.targetPercent, 12.5));
    CHECK(b.holdAutoBrightness);
    CHECK(b.holding);
}

TEST(json_codec_builtin_protection_null_target_and_false_flags)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"builtin_protection":{"protectable":true,"protected":false,"available":false,"reasons":[],"target_percent":null,"hold_auto_brightness":false,"holding":false}})");
    CHECK(s.builtinProtection.protectable);
    CHECK(!s.builtinProtection.protected_);
    CHECK(!s.builtinProtection.available);
    CHECK(s.builtinProtection.reasons.empty());
    CHECK(!s.builtinProtection.targetPercent.has_value());
    CHECK(!s.builtinProtection.holdAutoBrightness);
    CHECK(!s.builtinProtection.holding);
}

// A snapshot without builtin_protection keeps the inert default: protectable is false.
TEST(json_codec_builtin_protection_absent_is_inert_default)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(R"({"revision":1})");
    CHECK(!s.builtinProtection.protectable);
    CHECK(!s.builtinProtection.protected_);
    CHECK(!s.builtinProtection.available);
    CHECK(s.builtinProtection.reasons.empty());
    CHECK(!s.builtinProtection.targetPercent.has_value());
    CHECK(!s.builtinProtection.holdAutoBrightness);
    CHECK(!s.builtinProtection.holding);
}

TEST(json_codec_parses_protection_and_monitored_display_settings)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"settings":{"builtin_auto_brightness_off_on_protection":true,"monitored_display_id":"builtin","camera_history":["cam-2","cam-1"],"presence_min_brightness_by_display":{"mon-1":35,"builtin":0},"selected_camera_id":"cam-2"},"monitored_display":{"id":"builtin","automatic":false}})");
    CHECK(s.settings.builtin_auto_brightness_off_on_protection);
    CHECK_EQ(s.settings.monitored_display_id.value_or(""), std::string("builtin"));
    CHECK_EQ(s.settings.camera_history.size(), std::size_t{2});
    if (s.settings.camera_history.size() == 2) {
        CHECK_EQ(s.settings.camera_history[0], std::string("cam-2"));
        CHECK_EQ(s.settings.camera_history[1], std::string("cam-1"));
    }
    CHECK_EQ(s.settings.presence_min_brightness_by_display.size(), std::size_t{2});
    const auto mon = s.settings.presence_min_brightness_by_display.find("mon-1");
    CHECK(mon != s.settings.presence_min_brightness_by_display.end());
    if (mon != s.settings.presence_min_brightness_by_display.end()) {
        CHECK(nearly(mon->second, 35.0));
    }
    const auto builtin = s.settings.presence_min_brightness_by_display.find("builtin");
    CHECK(builtin != s.settings.presence_min_brightness_by_display.end());
    if (builtin != s.settings.presence_min_brightness_by_display.end()) {
        CHECK(nearly(builtin->second, 0.0));
    }
    CHECK_EQ(s.settings.selected_camera_id.value_or(""), std::string("cam-2"));

    CHECK(s.monitoredDisplay.id.has_value());
    CHECK_EQ(s.monitoredDisplay.id.value_or(""), std::string("builtin"));
    CHECK(!s.monitoredDisplay.automatic);
}

TEST(json_codec_protection_and_monitored_display_settings_defaults)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"settings":{"monitored_display_id":null},"monitored_display":{"id":null,"automatic":true}})");
    CHECK(!s.settings.builtin_auto_brightness_off_on_protection);
    CHECK(!s.settings.monitored_display_id.has_value());
    CHECK(s.settings.camera_history.empty());
    CHECK(s.settings.presence_min_brightness_by_display.empty());
    CHECK(!s.monitoredDisplay.id.has_value());
    CHECK(s.monitoredDisplay.automatic);

    const telaviva::Snapshot absent = telaviva::parseSnapshot(R"({"revision":1})");
    CHECK(!absent.settings.builtin_auto_brightness_off_on_protection);
    CHECK(!absent.settings.monitored_display_id.has_value());
    CHECK(absent.settings.camera_history.empty());
    CHECK(absent.settings.presence_min_brightness_by_display.empty());
    CHECK(!absent.monitoredDisplay.id.has_value());
    CHECK(!absent.monitoredDisplay.automatic);
}

TEST(json_codec_camera_retry_in_ms_is_parsed)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(R"({"camera":{"status":"busy","retry_in_ms":30000}})");
    CHECK(nearlyOpt(s.camera.retryInMs, 30000.0));
}

TEST(json_codec_camera_retry_in_ms_is_empty_when_missing_or_null)
{
    const telaviva::Snapshot missing = telaviva::parseSnapshot(R"({"camera":{"status":"ready"}})");
    CHECK(!missing.camera.retryInMs.has_value());

    const telaviva::Snapshot explicitNull =
        telaviva::parseSnapshot(R"({"camera":{"status":"busy","retry_in_ms":null}})");
    CHECK(!explicitNull.camera.retryInMs.has_value());
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

// Parses the normalized channel field and keeps observed as the raw value.
TEST(json_codec_controls_parses_normalized_channel)
{
    const telaviva::Snapshot s = telaviva::parseSnapshot(
        R"({"monitors":[{"id":"mon-1","brightness":{"supported":true,"observed":104,"normalized":41,"min":0,"max":255}},{"id":"mon-2","brightness":{"supported":true,"observed":7,"min":0,"max":100}}]})");
    CHECK_EQ(s.monitors.size(), std::size_t{2});
    if (s.monitors.size() == 2) {
        CHECK(nearlyOpt(s.monitors[0].brightness.observed, 104.0));
        CHECK(nearlyOpt(s.monitors[0].brightness.normalized, 41.0));
        CHECK(!s.monitors[1].brightness.normalized.has_value());
    }
}

namespace {

bool parseControlsThrows(const std::string& json)
{
    try {
        telaviva::parseControlsResult(json);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(json_codec_controls_configure_volume_owner_states)
{
    CHECK_EQ(compact(telaviva::buildControlsConfigure(1.5, 5, 5, true, std::nullopt)),
             std::string(R"({"type":"configure","now":1.5,"brightness_step":5,"volume_step":5,"keys_enabled":true})"));

    const std::optional<std::optional<std::string>> cleared(std::in_place, std::nullopt);
    CHECK_EQ(compact(telaviva::buildControlsConfigure(1.5, 5, 5, false, cleared)),
             std::string(R"({"type":"configure","now":1.5,"brightness_step":5,"volume_step":5,"keys_enabled":false,"volume_owner":null})"));

    const std::optional<std::optional<std::string>> named(std::in_place, "mon-1");
    CHECK_EQ(compact(telaviva::buildControlsConfigure(1.5, 5, 5, true, named)),
             std::string(R"({"type":"configure","now":1.5,"brightness_step":5,"volume_step":5,"keys_enabled":true,"volume_owner":"mon-1"})"));
}

TEST(json_codec_controls_observe_two_monitors_and_pending_action)
{
    telaviva::Snapshot snapshot;
    snapshot.revision = 42;

    telaviva::MonitorSnapshot first;
    first.id = "mon-1";
    first.generation = 3;
    first.displayState = "awake";
    first.identityStatus = "stable";
    // The raw observed value (104 of 0 to 255) must not reach the event; the normalized 41 must.
    first.brightness.supported = true;
    first.brightness.observed = 104.0;
    first.brightness.normalized = 41.0;
    first.brightness.min = 0.0;
    first.brightness.max = 255.0;
    first.volume.supported = true;
    first.volume.normalized = 20.0;
    first.volume.min = 0.0;
    first.volume.max = 100.0;
    snapshot.monitors.push_back(first);

    telaviva::MonitorSnapshot second;
    second.id = "mon-2";
    second.generation = 5;
    second.displayState = "asleep";
    second.identityStatus = "ambiguous";
    second.brightness.supported = false;
    second.volume.supported = true;
    second.volume.min = 0.0;
    second.volume.max = 100.0;
    snapshot.monitors.push_back(second);

    telaviva::PendingAction write;
    write.id = 17;
    write.generation = 8;
    write.monitorId = "mon-1";
    write.kind = "set_brightness";
    write.value = 37.0;
    telaviva::PendingAction read;
    read.id = 18;
    read.generation = 5;
    read.monitorId = "mon-2";
    read.kind = "read_brightness";
    snapshot.pendingActions = {write, read};

    CHECK_EQ(compact(telaviva::buildControlsObserve(120.5, snapshot)),
             std::string(R"({"type":"observe","now":120.5,"revision":42,)"
                         R"("monitors":[{"id":"mon-1","generation":3,"display_state":"awake","identity_status":"stable",)"
                         R"("brightness":{"supported":true,"observed":41,"min":0,"max":255},)"
                         R"("volume":{"supported":true,"observed":20,"min":0,"max":100}},)"
                         R"({"id":"mon-2","generation":5,"display_state":"asleep","identity_status":"ambiguous",)"
                         R"("brightness":{"supported":false,"observed":null,"min":0,"max":0},)"
                         R"("volume":{"supported":true,"observed":null,"min":0,"max":100}}],)"
                         R"("pending_actions":[{"id":17,"generation":8,"monitor_id":"mon-1","kind":"set_brightness","value":37},)"
                         R"({"id":18,"generation":5,"monitor_id":"mon-2","kind":"read_brightness","value":null}]})"));
}

TEST(json_codec_controls_slider_fields)
{
    CHECK_EQ(compact(telaviva::buildControlsSlider(3.0, "mon-1", "brightness", 37.5)),
             std::string(R"({"type":"slider","now":3,"monitor_id":"mon-1","feature":"brightness","value":37.5})"));
}

TEST(json_codec_controls_key_owner_null_and_set)
{
    CHECK_EQ(compact(telaviva::buildControlsKey(120.6, "volume_up", true, false, false, std::nullopt)),
             std::string(R"({"type":"key","now":120.6,"key":"volume_up","phase":"down","repeat":false,"fine":false,"owner":null})"));

    CHECK_EQ(compact(telaviva::buildControlsKey(120.7, "brightness_down", false, true, true,
                                                telaviva::KeyOwnerRef{"mon-1", 3})),
             std::string(R"({"type":"key","now":120.7,"key":"brightness_down","phase":"up","repeat":true,"fine":true,"owner":{"monitor_id":"mon-1","generation":3}})"));
}

TEST(json_codec_controls_query_draft_and_monitor_events)
{
    CHECK_EQ(compact(telaviva::buildControlsQueryDraft(1.0, "mon-1", "volume")),
             std::string(R"({"type":"query_draft","now":1,"monitor_id":"mon-1","feature":"volume"})"));
    CHECK_EQ(compact(telaviva::buildControlsMonitorProfile(2.0, "mon-1", "GSM")),
             std::string(R"({"type":"monitor_profile","now":2,"monitor_id":"mon-1","manufacturer":"GSM"})"));
    CHECK_EQ(compact(telaviva::buildControlsMonitorRemoved(2.0, "mon-1")),
             std::string(R"({"type":"monitor_removed","now":2,"monitor_id":"mon-1"})"));
}

TEST(json_codec_controls_display_power_fields)
{
    CHECK_EQ(compact(telaviva::buildControlsDisplayPower(4.0, true)),
             std::string(R"({"type":"display_power","now":4,"on":true})"));
    CHECK_EQ(compact(telaviva::buildControlsDisplayPower(4.0, false)),
             std::string(R"({"type":"display_power","now":4,"on":false})"));
}

TEST(json_codec_controls_submitted_fields)
{
    CHECK_EQ(compact(telaviva::buildControlsSubmitted(5.0, 9, false)),
             std::string(R"({"type":"submitted","now":5,"token":9,"accepted":false})"));
    CHECK_EQ(compact(telaviva::buildControlsSubmitted(5.0, 9, true)),
             std::string(R"({"type":"submitted","now":5,"token":9,"accepted":true})"));
}

TEST(json_codec_controls_attempt_done_success_and_failure)
{
    CHECK_EQ(compact(telaviva::buildControlsAttemptDone(5.0, 9, true, "", 30.0)),
             std::string(R"({"type":"attempt_done","now":5,"token":9,"ok":true,"value":30})"));
    CHECK_EQ(compact(telaviva::buildControlsAttemptDone(5.0, 9, true, "", std::nullopt)),
             std::string(R"({"type":"attempt_done","now":5,"token":9,"ok":true,"value":null})"));
    // The error name is present only on failure.
    CHECK_EQ(compact(telaviva::buildControlsAttemptDone(5.0, 9, false, "io", std::nullopt)),
             std::string(R"({"type":"attempt_done","now":5,"token":9,"ok":false,"error":"io","value":null})"));
}

TEST(json_codec_controls_tick_and_shutdown)
{
    CHECK_EQ(compact(telaviva::buildControlsTick(6.0)), std::string(R"({"type":"tick","now":6})"));
    CHECK_EQ(compact(telaviva::buildControlsShutdown(7.0)), std::string(R"({"type":"shutdown","now":7})"));
}

TEST(json_codec_controls_result_parses_all_effect_types)
{
    const telaviva::ControlsResult r = telaviva::parseControlsResult(
        R"json({"value":{"draft":37.5,"status":"pending"},"effects":[)json"
        R"json({"type":"submit_command","token":1,"command_json":"{\"type\":\"set_brightness\",\"monitor_id\":\"mon-1\",\"value\":37.5,\"connection_generation\":3}"},)json"
        R"json({"type":"run_attempt","token":2,"monitor_id":"mon-1","op":"write","code":16,"value":37.5},)json"
        R"json({"type":"submit_result","token":3,"result_json":"{\"type\":\"hardware_result\",\"action_id\":1,\"generation\":3,\"ok\":true,\"value\":37}"},)json"
        R"json({"type":"draft_changed","monitor_id":"mon-1","feature":"brightness"}],"next_deadline":120.52})json");

    CHECK_EQ(r.effects.size(), std::size_t{4});
    if (r.effects.size() == 4) {
        const telaviva::ControlsEffect& command = r.effects[0];
        CHECK(command.type == telaviva::ControlsEffect::Type::SubmitCommand);
        CHECK_EQ(command.token, uint64_t{1});
        CHECK_EQ(command.json, std::string(R"({"type":"set_brightness","monitor_id":"mon-1","value":37.5,"connection_generation":3})"));

        const telaviva::ControlsEffect& attempt = r.effects[1];
        CHECK(attempt.type == telaviva::ControlsEffect::Type::RunAttempt);
        CHECK_EQ(attempt.token, uint64_t{2});
        CHECK_EQ(attempt.monitorId, std::string("mon-1"));
        CHECK(attempt.write);
        CHECK(attempt.code == 16);
        CHECK(nearlyOpt(attempt.value, 37.5));

        const telaviva::ControlsEffect& result = r.effects[2];
        CHECK(result.type == telaviva::ControlsEffect::Type::SubmitResult);
        CHECK_EQ(result.token, uint64_t{3});
        CHECK_EQ(result.json, std::string(R"({"type":"hardware_result","action_id":1,"generation":3,"ok":true,"value":37})"));

        const telaviva::ControlsEffect& changed = r.effects[3];
        CHECK(changed.type == telaviva::ControlsEffect::Type::DraftChanged);
        CHECK_EQ(changed.monitorId, std::string("mon-1"));
        CHECK_EQ(changed.feature, std::string("brightness"));
    }

    CHECK(r.draft.has_value());
    if (r.draft) {
        CHECK(r.draft->hasDraft);
        CHECK(nearly(r.draft->draft, 37.5));
        CHECK_EQ(r.draft->status, std::string("pending"));
    }
    CHECK(!r.consume.has_value());
    CHECK(nearlyOpt(r.nextDeadline, 120.52));
}

TEST(json_codec_controls_result_read_attempt_has_no_value)
{
    const telaviva::ControlsResult r = telaviva::parseControlsResult(
        R"({"value":null,"effects":[{"type":"run_attempt","token":4,"monitor_id":"mon-2","op":"read","code":98}],"next_deadline":null})");
    CHECK_EQ(r.effects.size(), std::size_t{1});
    if (r.effects.size() == 1) {
        CHECK(!r.effects[0].write);
        CHECK(r.effects[0].code == 98);
        CHECK(!r.effects[0].value.has_value());
    }
    CHECK(!r.draft.has_value());
    CHECK(!r.consume.has_value());
    CHECK(!r.nextDeadline.has_value());
}

TEST(json_codec_controls_result_null_value_and_empty_effects)
{
    const telaviva::ControlsResult r =
        telaviva::parseControlsResult(R"({"value":null,"effects":[],"next_deadline":null})");
    CHECK(r.effects.empty());
    CHECK(!r.draft.has_value());
    CHECK(!r.consume.has_value());
    CHECK(!r.nextDeadline.has_value());
}

TEST(json_codec_controls_result_key_consume)
{
    const telaviva::ControlsResult consumed =
        telaviva::parseControlsResult(R"({"value":{"consume":true},"effects":[],"next_deadline":20.5})");
    CHECK(consumed.consume.has_value() && *consumed.consume);
    CHECK(!consumed.draft.has_value());
    CHECK(nearlyOpt(consumed.nextDeadline, 20.5));

    const telaviva::ControlsResult passed =
        telaviva::parseControlsResult(R"({"value":{"consume":false},"effects":[],"next_deadline":null})");
    CHECK(passed.consume.has_value() && !*passed.consume);
}

TEST(json_codec_controls_result_query_draft_carries_has_draft)
{
    const telaviva::ControlsResult r = telaviva::parseControlsResult(
        R"({"value":{"has_draft":false,"draft":40,"status":"ready"},"effects":[],"next_deadline":null})");
    CHECK(r.draft.has_value());
    if (r.draft) {
        CHECK(!r.draft->hasDraft);
        CHECK(nearly(r.draft->draft, 40.0));
        CHECK_EQ(r.draft->status, std::string("ready"));
    }
}

TEST(json_codec_controls_result_rejects_malformed_input)
{
    // Truncated JSON, a non-object root and a missing top-level field.
    CHECK(parseControlsThrows(R"({"value":null,"effects":[],"next_deadline":null)"));
    CHECK(parseControlsThrows(R"([])"));
    CHECK(parseControlsThrows(R"({"effects":[],"next_deadline":null})"));
    CHECK(parseControlsThrows(R"({"value":null,"next_deadline":null})"));
    CHECK(parseControlsThrows(R"({"value":null,"effects":"none","next_deadline":null})"));
    CHECK(parseControlsThrows(R"({"value":null,"effects":[],"next_deadline":"soon"})"));
    CHECK(parseControlsThrows(R"({"value":"yes","effects":[],"next_deadline":null})"));
    CHECK(parseControlsThrows(R"({"value":{"consume":"yes"},"effects":[],"next_deadline":null})"));
    // Unknown effect type, unknown op, out-of-range VCP code and an unknown feature.
    CHECK(parseControlsThrows(R"({"value":null,"effects":[{"type":"bogus","token":1}],"next_deadline":null})"));
    CHECK(parseControlsThrows(
        R"({"value":null,"effects":[{"type":"run_attempt","token":1,"monitor_id":"m","op":"verify","code":16}],"next_deadline":null})"));
    CHECK(parseControlsThrows(
        R"({"value":null,"effects":[{"type":"run_attempt","token":1,"monitor_id":"m","op":"read","code":300}],"next_deadline":null})"));
    CHECK(parseControlsThrows(
        R"({"value":null,"effects":[{"type":"draft_changed","monitor_id":"m","feature":"contrast"}],"next_deadline":null})"));
}
