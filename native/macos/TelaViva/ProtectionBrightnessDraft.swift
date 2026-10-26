/// Keeps a protection target responsive while settings updates and snapshots travel asynchronously.
struct ProtectionBrightnessDraft {
    private(set) var value: Double?
    private(set) var isEditing = false
    private var awaitingSnapshot = false
    private var lastSnapshotValue: Double?

    var isHoldingLocalValue: Bool { isEditing || awaitingSnapshot }

    func displayed(snapshotValue: Double?) -> Double {
        if isHoldingLocalValue, let value { return value }
        return Self.clamp(snapshotValue ?? 1)
    }

    mutating func begin(snapshotValue: Double?) {
        if !awaitingSnapshot { value = Self.clamp(snapshotValue ?? 1) }
        isEditing = true
    }

    mutating func set(_ newValue: Double) -> Double {
        let clamped = Self.clamp(newValue)
        value = clamped
        if !isEditing { awaitingSnapshot = true }
        return clamped
    }

    mutating func endEditing() {
        isEditing = false
        if let value, lastSnapshotValue == value {
            awaitingSnapshot = false
            self.value = nil
        } else {
            awaitingSnapshot = value != nil
        }
    }

    mutating func receive(snapshotValue: Double?) {
        guard let snapshotValue else { return }
        let snapshot = Self.clamp(snapshotValue)
        lastSnapshotValue = snapshot
        if awaitingSnapshot, let value, snapshot == value {
            if !isEditing {
                awaitingSnapshot = false
                self.value = nil
            }
        } else if !isEditing && !awaitingSnapshot {
            value = snapshot
        }
    }

    static func clamp(_ value: Double) -> Double {
        min(max(value, 1), 50)
    }
}
