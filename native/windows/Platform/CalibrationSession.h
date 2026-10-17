#pragma once
// Crash-safe record of a calibration that turned adaptive brightness off, stored as
// calibration-session.json. A present file at launch means the previous run did not
// restore. Writes go to a temp file that is renamed over the target. A corrupt or
// unknown-schema file is renamed to calibration-session.json.corrupt and treated as none.
// The file holds no personal data.
#include "AdaptiveBrightness.h"

#include <filesystem>
#include <optional>

namespace telaviva {

class CalibrationSession {
public:
    explicit CalibrationSession(std::filesystem::path directory);

    // Writes the record before adaptive brightness is changed. Returns false, writing nothing,
    // when the state is invalid or an unrecovered record exists: its saved values must not be lost.
    bool begin(const SavedAdaptiveState& saved);

    // Deletes the record. Call only after the saved values are restored.
    void end();

    // The record left by a previous run, or nullopt when there is none.
    std::optional<SavedAdaptiveState> pending() const;

    // Restores the record and deletes it only when the restore succeeded. True when nothing is left to recover.
    bool recover(AdaptiveBrightness& adaptive);

    std::filesystem::path file() const;

private:
    enum class Load { Absent, Loaded, Corrupt, Unreadable };

    Load load(SavedAdaptiveState& out) const;
    bool save(const SavedAdaptiveState& state) const;
    void quarantine() const;
    bool removeFile() const;

    std::filesystem::path file_;
};

}  // namespace telaviva
