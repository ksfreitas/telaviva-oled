import Foundation

/// The features that may ask the shared owner to turn the system's automatic brightness off.
enum AutoBrightnessHolder: Hashable {
    case calibration
    case protection
}

/// What the app leaves on disk while it holds automatic brightness off. It is written before the setting changes and
/// deleted after the setting is restored, so a crash in between leaves a record that launch recovery can act on.
struct AutoBrightnessHoldRecord: Codable, Equatable {
    var version = 1
    var original_enabled: Bool
}

/// Reads and writes `auto-brightness-hold.json`. Writes are atomic, so a crash never leaves a partial file.
/// Mirrors `BuiltInProtectionStore`.
final class AutoBrightnessHoldStore {
    enum Loaded: Equatable {
        case none
        case record(AutoBrightnessHoldRecord)
        case unreadable
    }

    private let directory: URL
    private let fileURL: URL

    init(directory: URL) {
        self.directory = directory
        self.fileURL = directory.appendingPathComponent("auto-brightness-hold.json")
    }

    /// Returns false when the directory or the file cannot be written.
    func save(_ record: AutoBrightnessHoldRecord) -> Bool {
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(record)
            try data.write(to: fileURL, options: .atomic)
            return true
        } catch {
            return false
        }
    }

    func load() -> Loaded {
        guard FileManager.default.fileExists(atPath: fileURL.path) else { return .none }
        guard let data = try? Data(contentsOf: fileURL),
              let record = try? JSONDecoder().decode(AutoBrightnessHoldRecord.self, from: data) else {
            return .unreadable
        }
        return .record(record)
    }

    func delete() {
        try? FileManager.default.removeItem(at: fileURL)
    }
}

/// The one owner of the system's automatic brightness. Calibration and protection both take the setting through
/// this owner, so the first holder records the state and the last release restores it exactly once.
/// Main queue only, like the other built-in display code.
final class BuiltInAutoBrightnessOwner {
    private let backend: BuiltInDisplayBackend
    private let store: AutoBrightnessHoldStore
    private var holders: Set<AutoBrightnessHolder> = []
    /// Outcome of the first acquire of the current hold; nil while no holder is active.
    private var held: AutoBrightnessAcquireOutcome?
    /// Set when a restore this owner owes has failed and its record is kept. The setting then reads "off" because of
    /// this owner rather than the user, so the next acquire must not read it as the user's choice.
    private var pendingRestore = false

    init(backend: BuiltInDisplayBackend, store: AutoBrightnessHoldStore) {
        self.backend = backend
        self.store = store
    }

    /// The first holder reads the setting and turns it off when it is on. Later holders only join the hold and never
    /// read the setting again, so every holder receives the outcome of the first one. While a failed restore is still
    /// owed, the first holder joins that hold without reading or writing the setting.
    @discardableResult
    func acquire(_ holder: AutoBrightnessHolder) -> AutoBrightnessAcquireOutcome {
        dispatchPrecondition(condition: .onQueue(.main))
        if !holders.isEmpty {
            holders.insert(holder)
            return held ?? .alreadyOff
        }
        let outcome: AutoBrightnessAcquireOutcome
        if pendingRestore {
            // The setting reads "off" because our restore failed, not because the user chose it. Reading it would report
            // alreadyOff and let the next release delete the record without restoring. Keep the record instead.
            outcome = .suspended
        } else if let enabled = backend.autoBrightnessEnabled() {
            // Only an "on" state needs a hold: there is nothing to restore when it is already off.
            outcome = enabled ? suspendAutoBrightness() : .alreadyOff
        } else {
            DiagnosticLog.core("auto_brightness_unknown")
            outcome = .unknown
        }
        held = outcome
        holders.insert(holder)
        return outcome
    }

    /// Saves the record before the setter runs, so a crash between the two leaves a record that launch recovery restores.
    private func suspendAutoBrightness() -> AutoBrightnessAcquireOutcome {
        guard store.save(AutoBrightnessHoldRecord(original_enabled: true)) else {
            DiagnosticLog.core("auto_brightness_not_changed")
            return .notChanged
        }
        guard backend.setAutoBrightness(false) else {
            // The setting did not change, so the record must not claim that a restore is owed.
            store.delete()
            DiagnosticLog.core("auto_brightness_not_changed")
            return .notChanged
        }
        return .suspended
    }

    /// Removes the holder. The last holder out restores the setting unless a fresh read says on: the user may have
    /// turned automatic brightness on while the screen was held. An unknown or off read still restores, because this
    /// owner turned the setting off. A failed restore keeps the record, so launch recovery can retry it, and the next
    /// acquire joins that owed restore.
    func release(_ holder: AutoBrightnessHolder) {
        dispatchPrecondition(condition: .onQueue(.main))
        // A holder that never acquired, or already released, changes nothing.
        guard holders.remove(holder) != nil else { return }
        guard holders.isEmpty else { return }
        // Only a fresh read of "on" skips the write: the user turned it on while the screen was held.
        // "Off" and "unknown" both restore, because this hold turned the setting off.
        if held == .suspended, backend.autoBrightnessEnabled() != true {
            guard backend.setAutoBrightness(true) else {
                // Keep the record: deleting it would leave automatic brightness off with no retry.
                DiagnosticLog.core("auto_brightness_restore_failed")
                pendingRestore = true
                held = nil
                return
            }
        }
        // Reached when nothing is owed: the restore succeeded, no hold of ours turned the setting off, or a fresh read
        // says on. The record and the owed restore are cleared together.
        store.delete()
        pendingRestore = false
        held = nil
    }

    /// Launch path. A leftover record means the last run turned automatic brightness off and did not restore it.
    /// Only a fresh read of "on" skips the restore, as in `release(_:)`. The record is deleted once the restore
    /// succeeds or when none is owed. A failed restore keeps the record, so the next launch retries it.
    func recoverIfNeeded() {
        dispatchPrecondition(condition: .onQueue(.main))
        switch store.load() {
        case .none:
            return
        case .unreadable:
            store.delete()
            pendingRestore = false
            DiagnosticLog.core("auto_brightness_record_unreadable")
        case .record(let record):
            // Only a fresh read of "on" skips the write. Off and unknown both restore, because the record says this
            // app turned the setting off.
            if record.original_enabled, backend.autoBrightnessEnabled() != true {
                guard backend.setAutoBrightness(true) else {
                    // Keep the record: deleting it would leave automatic brightness off with no retry.
                    DiagnosticLog.core("auto_brightness_restore_failed")
                    // The setting reads "off" because this restore failed, so a later acquire in this session must join it.
                    pendingRestore = true
                    return
                }
            }
            store.delete()
            pendingRestore = false
            DiagnosticLog.core("auto_brightness_recovered")
        }
    }

    /// The handle one holder uses, so the protection controller can keep the holder-agnostic protocol it already takes.
    func handle(for holder: AutoBrightnessHolder) -> BuiltInAutoBrightnessHolding {
        dispatchPrecondition(condition: .onQueue(.main))
        return AutoBrightnessHolderHandle(owner: self, holder: holder)
    }
}

/// Forwards one holder's acquire and release to the owner.
final class AutoBrightnessHolderHandle: BuiltInAutoBrightnessHolding {
    private let owner: BuiltInAutoBrightnessOwner
    private let holder: AutoBrightnessHolder

    init(owner: BuiltInAutoBrightnessOwner, holder: AutoBrightnessHolder) {
        self.owner = owner
        self.holder = holder
    }

    @discardableResult
    func acquire() -> AutoBrightnessAcquireOutcome {
        owner.acquire(holder)
    }

    func release() {
        owner.release(holder)
    }
}
