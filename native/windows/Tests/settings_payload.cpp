// Tests for the settings draft, its range clamping and the update_settings payloads.
// Payloads are checked after dropping whitespace, and member names are read by depth,
// so the tests do not depend on key order inside the settings object.
#include <windows.h>

#include "TestHarness.h"
#include "Platform/JsonCodec.h"
#include "Platform/SettingsModel.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

// Returns the member names whose object (or array) nesting is depth levels deep.
// The root object is depth 1, so the settings members are at depth 2.
std::vector<std::string> keysAtDepth(const std::string& json, int depth)
{
    std::vector<std::string> keys;
    const std::string text = compact(json);
    int level = 0;
    bool inString = false;
    bool escaped = false;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) {
                escaped = false;
                current.push_back(c);
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
                // A string followed by ':' at the wanted depth is a member name.
                if (level == depth && i + 1 < text.size() && text[i + 1] == ':') {
                    keys.push_back(current);
                }
                current.clear();
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            ++level;
        } else if (c == '}' || c == ']') {
            --level;
        }
    }
    return keys;
}

// Returns the text of a scalar member value, such as 5, true or "auto" with its quotes.
// Only valid for scalars, because the value ends at the next comma or closing bracket.
std::string scalarOf(const std::string& json, const std::string& key)
{
    const std::string text = compact(json);
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return "<missing>";
    }
    const std::size_t start = pos + needle.size();
    const std::size_t end = text.find_first_of(",}]", start);
    return text.substr(start, end - start);
}

std::string scalarFor(const telaviva::SettingsDraft& draft, const std::string& key)
{
    return scalarOf(telaviva::buildSettingsPayload(draft), key);
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

const std::set<std::string> kSettingsKeys = {
    "locale",
    "presence_sampling_interval_s",
    "return_sampling_interval_s",
    "suspend_presence_on_battery",
    "presence_protection_enabled",
    "presence_min_brightness_percent",
    "always_on_camera",
    "screensaver_protection_enabled",
    "launch_at_login",
    "hardware_keys_enabled",
    "brightness_key_step",
    "volume_key_step",
    "screensaver_targets",
    "ambient_follow_enabled",
    "protection_disabled",
};

}  // namespace

TEST(settings_payload_has_exactly_the_core_keys)
{
    const std::string json = telaviva::buildSettingsPayload(telaviva::SettingsDraft{});
    CHECK(compact(json).rfind(R"({"type":"update_settings","settings":{)", 0) == 0);

    const std::vector<std::string> root = keysAtDepth(json, 1);
    CHECK_EQ(root.size(), std::size_t{2});

    const std::vector<std::string> keys = keysAtDepth(json, 2);
    CHECK_EQ(keys.size(), std::size_t{15});
    const std::set<std::string> actual(keys.begin(), keys.end());
    CHECK(actual == kSettingsKeys);

    // The core rejects these keys in update_settings, so they must never appear.
    CHECK(!contains(json, R"("selected_camera_id")"));
    CHECK(!contains(json, R"("camera_history")"));
    CHECK(!contains(json, R"("ambient_curves")"));
    CHECK(!contains(json, R"("schema_version")"));
}

TEST(settings_payload_default_values)
{
    const telaviva::SettingsDraft draft{};
    CHECK_EQ(scalarFor(draft, "locale"), std::string(R"("auto")"));
    CHECK_EQ(scalarFor(draft, "presence_sampling_interval_s"), std::string("10"));
    CHECK_EQ(scalarFor(draft, "return_sampling_interval_s"), std::string("1"));
    CHECK_EQ(scalarFor(draft, "suspend_presence_on_battery"), std::string("true"));
    CHECK_EQ(scalarFor(draft, "presence_protection_enabled"), std::string("false"));
    CHECK_EQ(scalarFor(draft, "presence_min_brightness_percent"), std::string("20"));
    CHECK_EQ(scalarFor(draft, "always_on_camera"), std::string("false"));
    CHECK_EQ(scalarFor(draft, "screensaver_protection_enabled"), std::string("false"));
    CHECK_EQ(scalarFor(draft, "launch_at_login"), std::string("false"));
    CHECK_EQ(scalarFor(draft, "hardware_keys_enabled"), std::string("false"));
    CHECK_EQ(scalarFor(draft, "brightness_key_step"), std::string("5"));
    CHECK_EQ(scalarFor(draft, "volume_key_step"), std::string("5"));
    CHECK_EQ(scalarFor(draft, "ambient_follow_enabled"), std::string("false"));

    const std::string json = telaviva::buildSettingsPayload(draft);
    CHECK(contains(json, R"("screensaver_targets":{})"));
    CHECK(contains(json, R"("protection_disabled":[])"));
}

TEST(settings_payload_clamps_presence_interval_both_ends)
{
    telaviva::SettingsDraft draft;

    draft.presenceSamplingIntervalS = 1.0;
    CHECK_EQ(scalarFor(draft, "presence_sampling_interval_s"), std::string("5"));
    draft.presenceSamplingIntervalS = 1000.0;
    CHECK_EQ(scalarFor(draft, "presence_sampling_interval_s"), std::string("300"));
    draft.presenceSamplingIntervalS = std::numeric_limits<double>::infinity();
    CHECK_EQ(scalarFor(draft, "presence_sampling_interval_s"), std::string("300"));
    // NaN has no order, so it takes the field default (10).
    draft.presenceSamplingIntervalS = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(scalarFor(draft, "presence_sampling_interval_s"), std::string("10"));
    draft.presenceSamplingIntervalS = 42.5;
    CHECK_EQ(scalarFor(draft, "presence_sampling_interval_s"), std::string("42.5"));
}

TEST(settings_payload_clamps_return_interval_both_ends)
{
    telaviva::SettingsDraft draft;

    draft.returnSamplingIntervalS = 0.1;
    CHECK_EQ(scalarFor(draft, "return_sampling_interval_s"), std::string("0.25"));
    draft.returnSamplingIntervalS = 50.0;
    CHECK_EQ(scalarFor(draft, "return_sampling_interval_s"), std::string("5"));
    draft.returnSamplingIntervalS = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(scalarFor(draft, "return_sampling_interval_s"), std::string("1"));
    draft.returnSamplingIntervalS = 2.5;
    CHECK_EQ(scalarFor(draft, "return_sampling_interval_s"), std::string("2.5"));
}

TEST(settings_payload_clamps_min_brightness_both_ends)
{
    telaviva::SettingsDraft draft;

    draft.presenceMinBrightnessPercent = -5.0;
    CHECK_EQ(scalarFor(draft, "presence_min_brightness_percent"), std::string("10"));
    draft.presenceMinBrightnessPercent = 100.0;
    CHECK_EQ(scalarFor(draft, "presence_min_brightness_percent"), std::string("90"));
    draft.presenceMinBrightnessPercent = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(scalarFor(draft, "presence_min_brightness_percent"), std::string("20"));
    draft.presenceMinBrightnessPercent = 35.0;
    CHECK_EQ(scalarFor(draft, "presence_min_brightness_percent"), std::string("35"));
}

TEST(settings_payload_clamps_key_steps_both_ends)
{
    telaviva::SettingsDraft draft;

    draft.brightnessKeyStep = 0;
    CHECK_EQ(scalarFor(draft, "brightness_key_step"), std::string("1"));
    draft.brightnessKeyStep = 99;
    CHECK_EQ(scalarFor(draft, "brightness_key_step"), std::string("15"));
    draft.volumeKeyStep = -3;
    CHECK_EQ(scalarFor(draft, "volume_key_step"), std::string("1"));
    draft.volumeKeyStep = 16;
    CHECK_EQ(scalarFor(draft, "volume_key_step"), std::string("15"));
    draft.volumeKeyStep = 7;
    CHECK_EQ(scalarFor(draft, "volume_key_step"), std::string("7"));
}

TEST(settings_payload_locale_fallback)
{
    const std::vector<std::string> kept = {"auto", "en-US", "pt-BR"};
    for (const std::string& locale : kept) {
        telaviva::SettingsDraft draft;
        draft.locale = locale;
        CHECK_EQ(scalarFor(draft, "locale"), "\"" + locale + "\"");
    }

    const std::vector<std::string> replaced = {"fr-FR", "", "pt-br", "en-us", "PT-BR"};
    for (const std::string& locale : replaced) {
        telaviva::SettingsDraft draft;
        draft.locale = locale;
        CHECK_EQ(scalarFor(draft, "locale"), std::string(R"("auto")"));
    }
}

TEST(settings_payload_protection_disabled_sorted_unique_and_valid)
{
    telaviva::SettingsDraft draft;
    // A set cannot hold duplicates, so the sorted order and the empty or oversized drops are what is checked.
    draft.protectionDisabled = {"win-b", "win-a", "", std::string(513, 'x'), std::string(512, 'y')};
    const std::string json = telaviva::buildSettingsPayload(draft);
    CHECK(contains(json, R"("protection_disabled":["win-a","win-b",")" + std::string(512, 'y') + R"("])"));
    CHECK(!contains(json, std::string(513, 'x')));
}

TEST(settings_payload_protection_disabled_caps_at_core_limit)
{
    telaviva::SettingsDraft draft;
    for (int i = 0; i < 130; ++i) {
        char id[16];
        std::snprintf(id, sizeof id, "win-%03d", i);
        draft.protectionDisabled.insert(std::string(id));
    }
    const std::string json = telaviva::buildSettingsPayload(draft);
    // The sorted first 128 end at win-127; win-128 and win-129 are dropped.
    CHECK(contains(json, R"("win-127"])"));
    CHECK(!contains(json, R"("win-128")"));
}

TEST(settings_payload_screensaver_targets_clamp_and_validate)
{
    telaviva::SettingsDraft draft;
    const std::string longKey(512, 'k');
    draft.screensaverTargets = {
        {"win-a", -5.0},
        {"win-b", 150.0},
        {"win-c", std::numeric_limits<double>::quiet_NaN()},
        {"win-d", 42.5},
        {"", 40.0},
        {longKey, 10.0},
        {std::string(513, 'x'), 10.0},
    };
    const std::string json = telaviva::buildSettingsPayload(draft);
    // std::map keeps the keys sorted, and 'k' sorts before 'w'.
    CHECK(contains(json, R"("screensaver_targets":{")" + longKey +
                             R"(":10,"win-a":0,"win-b":100,"win-d":42.5})"));
    CHECK(!contains(json, "win-c"));
    CHECK(!contains(json, std::string(513, 'x')));
}

TEST(settings_payload_camera_escapes_quotes_and_backslashes)
{
    const std::string id = std::string("cam\"era\\one");
    const std::string json = telaviva::buildSelectCameraPayload(std::optional<std::string>{id});
    // Kept out of the macro: MSVC mis-stringizes backslashes in raw literals inside CHECK_EQ.
    const std::string expected = R"({"type":"update_settings","settings":{"selected_camera_id":"cam\"era\\one"}})";
    CHECK_EQ(compact(json), expected);
    CHECK_EQ(keysAtDepth(json, 2).size(), std::size_t{1});
}

TEST(settings_payload_camera_null_and_length_limits)
{
    CHECK_EQ(compact(telaviva::buildSelectCameraPayload(std::nullopt)),
             std::string(R"({"type":"update_settings","settings":{"selected_camera_id":null}})"));
    CHECK_EQ(compact(telaviva::buildSelectCameraPayload(std::optional<std::string>{""})),
             std::string(R"({"type":"update_settings","settings":{"selected_camera_id":null}})"));
    CHECK_EQ(compact(telaviva::buildSelectCameraPayload(std::optional<std::string>{std::string(513, 'x')})),
             std::string(R"({"type":"update_settings","settings":{"selected_camera_id":null}})"));

    const std::string longest(512, 'x');
    CHECK(contains(telaviva::buildSelectCameraPayload(std::optional<std::string>{longest}),
                   R"("selected_camera_id":")" + longest + R"("})"));
}

TEST(settings_payload_round_trip_basic_fixture_gives_defaults)
{
    const telaviva::Snapshot s = parseFixture("snapshot-basic.json");
    const std::string fromFixture = telaviva::buildSettingsPayload(telaviva::draftFromSnapshotSettings(s.settings));
    CHECK_EQ(fromFixture, telaviva::buildSettingsPayload(telaviva::SettingsDraft{}));
}

TEST(settings_payload_round_trip_action_fixture_gives_defaults)
{
    const telaviva::Snapshot s = parseFixture("snapshot-with-action.json");
    const std::string fromFixture = telaviva::buildSettingsPayload(telaviva::draftFromSnapshotSettings(s.settings));
    CHECK_EQ(fromFixture, telaviva::buildSettingsPayload(telaviva::SettingsDraft{}));
}

TEST(settings_payload_round_trip_non_default_values)
{
    // Covers the parser fields added for the draft, not only the fixture defaults.
    const std::string snapshot = R"({"settings":{"locale":"pt-BR","presence_sampling_interval_s":30,)"
                                 R"("return_sampling_interval_s":2.5,"suspend_presence_on_battery":false,)"
                                 R"("presence_protection_enabled":true,"presence_min_brightness_percent":35,)"
                                 R"("always_on_camera":true,"screensaver_protection_enabled":true,)"
                                 R"("launch_at_login":true,"hardware_keys_enabled":true,)"
                                 R"("brightness_key_step":3,"volume_key_step":7,)"
                                 R"("screensaver_targets":{"win-a":25},"ambient_follow_enabled":true,)"
                                 R"("protection_disabled":["win-b"]}})";
    const telaviva::Snapshot s = telaviva::parseSnapshot(snapshot);
    const std::string json = telaviva::buildSettingsPayload(telaviva::draftFromSnapshotSettings(s.settings));
    CHECK_EQ(compact(json),
             std::string(R"({"type":"update_settings","settings":{"locale":"pt-BR",)"
                         R"("presence_sampling_interval_s":30,"return_sampling_interval_s":2.5,)"
                         R"("suspend_presence_on_battery":false,"presence_protection_enabled":true,)"
                         R"("presence_min_brightness_percent":35,"always_on_camera":true,)"
                         R"("screensaver_protection_enabled":true,"launch_at_login":true,)"
                         R"("hardware_keys_enabled":true,"brightness_key_step":3,"volume_key_step":7,)"
                         R"("screensaver_targets":{"win-a":25},"ambient_follow_enabled":true,)"
                         R"("protection_disabled":["win-b"]}})"));
}
