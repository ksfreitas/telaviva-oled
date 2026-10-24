#pragma once
// The one owner of Windows adaptive brightness (the OS "automatic brightness" setting of the power
// scheme). Calibration and built-in protection both take the setting through this owner, so the first
// holder records the state and the last release restores it once. This is the Windows mirror of
// BuiltInAutoBrightnessOwner.swift. The OS feature is not the app's "Automatic brightness" page.
//
// The hold record auto-brightness-hold.json (schema 1: scheme GUID, AC value, DC value) is written before
// the setting changes and deleted after it is restored, so a crash in between leaves a record for launch
// recovery. Writes are atomic. Never throws.
#include "AdaptiveBrightness.h"
#include "Diagnostics.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>

namespace telaviva {

// The features that may ask the shared owner to turn adaptive brightness off.
enum class AutoBrightnessHolder { calibration, protection };

// What the first acquire of a hold found. Every holder of that hold receives the same outcome.
enum class AutoBrightnessOutcome {
    turnedOff,      // the setting was on and is now off; the hold owes a restore
    notApplicable,  // already off, or absent (desktop); nothing was written and no record exists
    notChanged,     // the setting was on but turning it off failed (the write failed, or persistFirst refused); nothing was kept
    unknown         // the state could not be read; nothing was written
};

class AutoBrightnessOwner {
public:
    // adaptive must outlive the owner. recordFile is auto-brightness-hold.json. diagnostics may be null.
    AutoBrightnessOwner(AdaptiveBrightness& adaptive, std::filesystem::path recordFile, Diagnostics* diagnostics);

    AutoBrightnessOwner(const AutoBrightnessOwner&) = delete;
    AutoBrightnessOwner& operator=(const AutoBrightnessOwner&) = delete;

    // Adds the holder. The first acquire of a hold reads the setting. When it is on, it runs persistFirst
    // (the calibration passes CalibrationSession::begin), then writes the hold record, then turns the
    // setting off. If persistFirst returns false, or the record cannot be written, the setting is not
    // turned off and the outcome is notChanged. An unreadable state is unknown. Later acquires only join the hold and ignore persistFirst.
    // While a failed restore is still owed, the acquire joins it and returns turnedOff without reading.
    // persistFirst runs under the owner's lock and must not call back into this owner.
    AutoBrightnessOutcome acquire(AutoBrightnessHolder holder,
                                  const AdaptiveBrightness::PersistFirst& persistFirst = AdaptiveBrightness::PersistFirst());

    // Removes the holder. The last holder out restores the saved values, unless a fresh read finds the
    // setting on (the user turned it on during the hold). A nil or off read still restores. On success the
    // record is deleted. On failure the record is kept, a restore is marked pending, and this returns false.
    // A holder that is not held changes nothing and returns true.
    bool release(AutoBrightnessHolder holder);

    bool isHeld(AutoBrightnessHolder holder) const;
    bool anyHeld() const;

    // Launch path, before any acquire. A leftover record means the last run turned the setting off and did
    // not restore it: restore it with the same fresh-read rule, then delete the record. A failed restore keeps
    // the record and marks a pending restore. An unparsable record is deleted and logged.
    void recoverIfNeeded();

private:
    AutoBrightnessOutcome beginHoldLocked(const AdaptiveBrightness::PersistFirst& persistFirst);
    // True when nothing is owed: a fresh read says on, or the restore succeeded.
    bool restoreLocked(const SavedAdaptiveState& state);
    bool freshReadOnLocked();
    void removeRecordLocked();
    void logLocked(LogLevel level, const char* message);

    AdaptiveBrightness& adaptive_;
    std::filesystem::path recordFile_;
    Diagnostics* diagnostics_;
    mutable std::mutex mutex_;
    std::set<AutoBrightnessHolder> holders_;
    // Outcome of the first acquire of the current hold; empty while no holder is active.
    std::optional<AutoBrightnessOutcome> held_;
    // Set when a restore this owner owes has failed and its record is kept. The setting then reads off
    // because of this owner, not the user, so the next acquire joins the owed restore.
    bool pendingRestore_ = false;
    // The values saved by the hold, or loaded from the record at launch. Kept for a retry.
    SavedAdaptiveState saved_;
};

}  // namespace telaviva
