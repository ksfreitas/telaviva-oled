#include "TestHarness.h"

#include "Platform/InferenceFailureStore.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using telaviva::failureReasonName;
using telaviva::FailureKey;
using telaviva::FailureReason;
using telaviva::InferenceFailureStore;

namespace {

// Unique per call; the destructor removes the directory and everything in it.
class TempDir {
public:
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("telaviva-failures-" + std::to_string(stamp) + "-" + std::to_string(std::random_device{}()));
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

FailureKey key(const std::string& profile, const std::string& driver, const std::string& app)
{
    FailureKey value;
    value.profile = profile;
    value.driver = driver;
    value.app = app;
    return value;
}

const std::string kDriver = "32.0.21041.1000";
const std::string kApp = "0.1.0";

}  // namespace

TEST(inference_failures_missing_file_has_no_markers)
{
    const TempDir dir;
    const InferenceFailureStore store(dir.path());
    CHECK(!store.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK(store.records().empty());
    // Construction alone does not create the file.
    CHECK(!fs::exists(dir.path() / "inference-failures.json"));
}

TEST(inference_failures_record_writes_schema_1_file_and_marker_is_found)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    const FailureKey marker = key("amd-test", kDriver, kApp);
    store.record(marker, FailureReason::SelfTestMismatch);
    CHECK(store.hasMarker(marker));

    const fs::path file = dir.path() / "inference-failures.json";
    CHECK(fs::exists(file));
    const std::string text = readText(file);
    const bool hasSchema = text.find(R"("schema":1)") != std::string::npos;
    const bool hasReason = text.find(R"("reason":"self_test_mismatch")") != std::string::npos;
    const bool hasProfile = text.find(R"("profile":"amd-test")") != std::string::npos;
    CHECK(hasSchema);
    CHECK(hasReason);
    CHECK(hasProfile);
}

TEST(inference_failures_persist_across_instances)
{
    const TempDir dir;
    {
        InferenceFailureStore writer(dir.path());
        writer.record(key("amd-test", kDriver, kApp), FailureReason::RuntimeError);
    }
    const InferenceFailureStore reader(dir.path());
    CHECK(reader.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK_EQ(reader.records().size(), static_cast<size_t>(1));
    if (reader.records().size() == 1) {
        const telaviva::FailureRecord& record = reader.records()[0];
        CHECK(record.reason == FailureReason::RuntimeError);
        CHECK_EQ(record.key.profile, std::string("amd-test"));
        CHECK_EQ(record.key.driver, kDriver);
        CHECK_EQ(record.key.app, kApp);
    }
}

TEST(inference_failures_marker_requires_exact_profile_driver_and_app)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::SessionFailed);
    CHECK(store.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK(!store.hasMarker(key("amd-other", kDriver, kApp)));
    CHECK(!store.hasMarker(key("amd-test", "32.0.21041.1001", kApp)));
    CHECK(!store.hasMarker(key("amd-test", kDriver, "0.2.0")));
}

TEST(inference_failures_record_replaces_marker_with_same_key)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    const FailureKey marker = key("amd-test", kDriver, kApp);
    store.record(marker, FailureReason::RegisterFailed);
    store.record(marker, FailureReason::SelfTestSlow);
    CHECK_EQ(store.records().size(), static_cast<size_t>(1));
    if (store.records().size() == 1) {
        CHECK(store.records()[0].reason == FailureReason::SelfTestSlow);
    }
}

TEST(inference_failures_clear_removes_marker_and_persists)
{
    const TempDir dir;
    const FailureKey marker = key("amd-test", kDriver, kApp);
    {
        InferenceFailureStore store(dir.path());
        store.record(marker, FailureReason::SessionFailed);
        store.clear(marker);
        CHECK(!store.hasMarker(marker));
    }
    const InferenceFailureStore reader(dir.path());
    CHECK(!reader.hasMarker(marker));
}

TEST(inference_failures_clear_of_absent_key_keeps_others)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::SessionFailed);
    store.clear(key("amd-missing", kDriver, kApp));
    CHECK(store.hasMarker(key("amd-test", kDriver, kApp)));
}

TEST(inference_failures_clear_if_changed_keeps_matching_markers)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::SelfTestMismatch);
    store.record(key("amd-other", kDriver, kApp), FailureReason::SelfTestSlow);
    store.clearIfChanged(kDriver, kApp);
    CHECK_EQ(store.records().size(), static_cast<size_t>(2));
    CHECK(store.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK(store.hasMarker(key("amd-other", kDriver, kApp)));
}

TEST(inference_failures_clear_if_changed_drops_marker_on_driver_change)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::SelfTestMismatch);
    store.clearIfChanged("32.0.21041.1001", kApp);
    CHECK(!store.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK(store.records().empty());
}

TEST(inference_failures_clear_if_changed_drops_marker_on_app_change)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::RuntimeError);
    store.clearIfChanged(kDriver, "0.2.0");
    CHECK(!store.hasMarker(key("amd-test", kDriver, kApp)));
}

TEST(inference_failures_clear_if_changed_persists_the_removal)
{
    const TempDir dir;
    {
        InferenceFailureStore store(dir.path());
        store.record(key("amd-test", kDriver, kApp), FailureReason::RuntimeError);
        store.clearIfChanged("32.0.21041.1001", kApp);
    }
    const InferenceFailureStore reader(dir.path());
    CHECK(reader.records().empty());
}

TEST(inference_failures_corrupt_file_is_renamed_and_store_starts_empty)
{
    const TempDir dir;
    const fs::path file = dir.path() / "inference-failures.json";
    writeText(file, "{not json");
    const InferenceFailureStore store(dir.path());
    CHECK(store.records().empty());
    CHECK(!store.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK(!fs::exists(file));
    const fs::path corrupt = dir.path() / "inference-failures.json.corrupt";
    CHECK(fs::exists(corrupt));
    CHECK_EQ(readText(corrupt), std::string("{not json"));
}

TEST(inference_failures_wrong_schema_is_treated_as_corrupt)
{
    const TempDir dir;
    const fs::path file = dir.path() / "inference-failures.json";
    writeText(file, R"({"schema":2,"failures":[]})");
    const InferenceFailureStore store(dir.path());
    CHECK(store.records().empty());
    CHECK(!fs::exists(file));
    CHECK(fs::exists(dir.path() / "inference-failures.json.corrupt"));
}

TEST(inference_failures_unknown_reason_is_treated_as_corrupt)
{
    const TempDir dir;
    const fs::path file = dir.path() / "inference-failures.json";
    writeText(file, R"({"schema":1,"failures":[{"profile":"amd-test","driver":"32.0.21041.1000","app":"0.1.0","reason":"bogus","at":"2026-10-17T14:30:00Z"}]})");
    const InferenceFailureStore store(dir.path());
    CHECK(store.records().empty());
    CHECK(fs::exists(dir.path() / "inference-failures.json.corrupt"));
}

TEST(inference_failures_missing_field_is_treated_as_corrupt)
{
    const TempDir dir;
    const fs::path file = dir.path() / "inference-failures.json";
    writeText(file, R"({"schema":1,"failures":[{"profile":"amd-test","driver":"32.0.21041.1000","reason":"runtime_error","at":"2026-10-17T14:30:00Z"}]})");
    const InferenceFailureStore store(dir.path());
    CHECK(store.records().empty());
    CHECK(fs::exists(dir.path() / "inference-failures.json.corrupt"));
}

TEST(inference_failures_valid_file_is_read_not_renamed)
{
    const TempDir dir;
    const fs::path file = dir.path() / "inference-failures.json";
    writeText(file, R"({"schema":1,"failures":[{"profile":"amd-test","driver":"32.0.21041.1000","app":"0.1.0","reason":"self_test_slow","at":"2026-10-17T14:30:00Z"}]})");
    const InferenceFailureStore store(dir.path());
    CHECK(store.hasMarker(key("amd-test", kDriver, kApp)));
    CHECK(fs::exists(file));
    CHECK(!fs::exists(dir.path() / "inference-failures.json.corrupt"));
}

TEST(inference_failures_reason_names_match_contract)
{
    CHECK_EQ(std::string(failureReasonName(FailureReason::RegisterFailed)), std::string("register_failed"));
    CHECK_EQ(std::string(failureReasonName(FailureReason::SessionFailed)), std::string("session_failed"));
    CHECK_EQ(std::string(failureReasonName(FailureReason::SelfTestMismatch)), std::string("self_test_mismatch"));
    CHECK_EQ(std::string(failureReasonName(FailureReason::SelfTestSlow)), std::string("self_test_slow"));
    CHECK_EQ(std::string(failureReasonName(FailureReason::RuntimeError)), std::string("runtime_error"));
}

TEST(inference_failures_every_reason_round_trips_through_file)
{
    const TempDir dir;
    const FailureReason reasons[] = {FailureReason::RegisterFailed, FailureReason::SessionFailed,
                                     FailureReason::SelfTestMismatch, FailureReason::SelfTestSlow,
                                     FailureReason::RuntimeError};
    {
        InferenceFailureStore writer(dir.path());
        int index = 0;
        for (FailureReason reason : reasons) {
            writer.record(key("profile-" + std::to_string(index), kDriver, kApp), reason);
            ++index;
        }
    }
    const InferenceFailureStore reader(dir.path());
    CHECK_EQ(reader.records().size(), static_cast<size_t>(5));
    CHECK(!fs::exists(dir.path() / "inference-failures.json.corrupt"));
}

TEST(inference_failures_timestamp_is_utc_iso_8601)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::SelfTestSlow);
    CHECK_EQ(store.records().size(), static_cast<size_t>(1));
    if (store.records().size() == 1) {
        const std::string& at = store.records()[0].at;
        CHECK_EQ(at.size(), static_cast<size_t>(20));
        if (at.size() == 20) {
            CHECK(at[4] == '-');
            CHECK(at[7] == '-');
            CHECK(at[10] == 'T');
            CHECK(at[13] == ':');
            CHECK(at[16] == ':');
            CHECK(at[19] == 'Z');
        }
    }
}

TEST(inference_failures_write_leaves_no_temp_file)
{
    const TempDir dir;
    InferenceFailureStore store(dir.path());
    store.record(key("amd-test", kDriver, kApp), FailureReason::SessionFailed);
    CHECK(fs::exists(dir.path() / "inference-failures.json"));
    CHECK(!fs::exists(dir.path() / "inference-failures.json.tmp"));
}

TEST(inference_failures_record_creates_missing_directory)
{
    const TempDir dir;
    const fs::path nested = dir.path() / "nested" / "TelaVivaOLED";
    InferenceFailureStore store(nested);
    store.record(key("amd-test", kDriver, kApp), FailureReason::SessionFailed);
    CHECK(fs::exists(nested / "inference-failures.json"));
}

TEST(inference_failures_escapes_quotes_in_strings)
{
    const TempDir dir;
    const std::string quoted = "amd\"quoted";
    {
        InferenceFailureStore writer(dir.path());
        writer.record(key(quoted, kDriver, kApp), FailureReason::RuntimeError);
    }
    const InferenceFailureStore reader(dir.path());
    CHECK(reader.hasMarker(key(quoted, kDriver, kApp)));
    CHECK(!fs::exists(dir.path() / "inference-failures.json.corrupt"));
}
