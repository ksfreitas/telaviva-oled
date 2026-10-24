#pragma once
// Crash-safe record of a calibration that turned adaptive brightness off, stored as
// calibration-session.json. A present file at launch means the previous run did not
// restore. Writes go to a temp file that is renamed over the target. A corrupt or
// unknown-schema file is renamed to calibration-session.json.corrupt and treated as none.
// The file holds no personal data.
//
// Schema 1 keeps its layout. The adaptive_brightness object is the adaptive part. The optional
// monitor_id and monitor_percent (a pair) and panel_level hold the monitor and Laptop panel levels
// from before the session. Records written before these fields existed load with no monitor part.
// After the adaptive values are restored, the record is rewritten without the adaptive object, so a
// later launch never restores them again; a monitor-only record keeps the monitor part until the monitor
// reports again.
#include "AdaptiveBrightness.h"

#include <filesystem>
#include <optional>
#include <string>

namespace telaviva {

// The monitor's brightness before the session, written only when it was readable.
struct SavedMonitorState {
    std::string monitorId;
    double percent = 0.0;
};

// The whole record. The adaptive part is absent in the monitor-only shape.
struct CalibrationRecord {
    // Meaningful only when adaptiveValid is true.
    SavedAdaptiveState adaptive;
    // False when the record has no adaptive_brightness object (the monitor-only shape).
    bool adaptiveValid = true;
    std::optional<SavedMonitorState> monitor;
    // The Laptop panel level before the session. Absent in records written before this field existed.
    std::optional<double> panelLevel;
};

class CalibrationSession {
public:
    explicit CalibrationSession(std::filesystem::path directory);

    // Writes an adaptive-only record before adaptive brightness is changed. Returns false, writing nothing,
    // when the state is invalid or an unrecovered record exists: its saved values must not be lost.
    bool begin(const SavedAdaptiveState& saved);

    // Same as above for a record with the monitor and panel parts. The adaptive part must be valid.
    bool begin(const CalibrationRecord& record);

    // Overwrites the record with no check for an existing one. Used when a new session replaces a
    // monitor-only record, and to write the monitor-only shape after the adaptive values are restored.
    // False when the record is invalid or the file cannot be written.
    bool rewrite(const CalibrationRecord& record);

    // Deletes the record. Call only after the saved values are restored.
    void end();

    // The adaptive part of the record left by a previous run, or nullopt when there is none or it is monitor-only.
    std::optional<SavedAdaptiveState> pending() const;

    // The whole record left by a previous run, or nullopt when there is none (absent, corrupt or unreadable).
    std::optional<CalibrationRecord> pendingRecord() const;

    // Restores the adaptive part. A record with a monitor part is then rewritten as monitor-only; any other
    // record is deleted. A monitor-only record has nothing adaptive to restore and is left as it is.
    // Returns false, keeping the record, when the restore or the rewrite failed.
    bool recover(AdaptiveBrightness& adaptive);

    std::filesystem::path file() const;

private:
    enum class Load { Absent, Loaded, Corrupt, Unreadable };

    Load load(CalibrationRecord& out) const;
    bool save(const CalibrationRecord& record) const;
    void quarantine() const;
    bool removeFile() const;

    std::filesystem::path file_;
};

}  // namespace telaviva
