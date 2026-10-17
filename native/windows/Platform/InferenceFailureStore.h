#pragma once
// Failure markers for DirectML profiles, persisted as inference-failures.json.
// A profile is skipped while a marker exists for its (profile, driver, app) key.
// The file holds no camera data. Writes go to a temp file that is renamed over
// the target. A corrupt file is renamed to inference-failures.json.corrupt and
// the store starts empty. Persistence failures keep the in-memory state.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace telaviva {

enum class FailureReason { RegisterFailed, SessionFailed, SelfTestMismatch, SelfTestSlow, RuntimeError };

// The reason string written to the file, for example "self_test_mismatch".
std::string_view failureReasonName(FailureReason reason);

struct FailureKey {
    std::string profile;
    std::string driver;
    std::string app;
    bool operator==(const FailureKey& other) const = default;
};

struct FailureRecord {
    FailureKey key;
    FailureReason reason = FailureReason::RegisterFailed;
    std::string at;  // UTC, "YYYY-MM-DDTHH:MM:SSZ"
};

class InferenceFailureStore {
public:
    // Reads <directory>/inference-failures.json now. Never throws.
    explicit InferenceFailureStore(std::filesystem::path directory);

    bool hasMarker(const FailureKey& key) const;

    // Replaces any marker with the same key and writes the file.
    void record(const FailureKey& key, FailureReason reason);

    void clear(const FailureKey& key);

    // Drops every marker whose driver or app differs from the current values, so
    // a changed driver or app is eligible again. The driver is a single value, so
    // a marker for another adapter's driver is dropped too; that only costs one retry.
    void clearIfChanged(const std::string& currentDriver, const std::string& currentApp);

    const std::vector<FailureRecord>& records() const;
    std::filesystem::path file() const;

private:
    void load();
    void save() const;
    void quarantine() const;

    std::filesystem::path file_;
    std::vector<FailureRecord> records_;
};

}  // namespace telaviva
