import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("ProtectionBrightnessDraftTests: \(message)") }
}

@main
private struct ProtectionBrightnessDraftTests {
    static func main() {
        var keyboardDraft = ProtectionBrightnessDraft()
        _ = keyboardDraft.set(29)
        expect(keyboardDraft.displayed(snapshotValue: 1) == 29,
               "keyboard or accessibility changes hold the local value without an editing callback")

        var delayedBeginDraft = ProtectionBrightnessDraft()
        _ = delayedBeginDraft.set(31)
        delayedBeginDraft.begin(snapshotValue: 1)
        expect(delayedBeginDraft.displayed(snapshotValue: 1) == 31,
               "a late editing callback does not overwrite a setter value with an old snapshot")

        var draft = ProtectionBrightnessDraft()
        draft.begin(snapshotValue: 1)
        expect(draft.set(24) == 24, "drag value is kept as a local draft")
        draft.receive(snapshotValue: 1)
        expect(draft.displayed(snapshotValue: 1) == 24, "stale snapshots cannot snap the slider back during drag")

        _ = draft.set(37)
        draft.endEditing()
        draft.receive(snapshotValue: 24)
        expect(draft.displayed(snapshotValue: 24) == 37, "older queued acknowledgments cannot replace the final drag value")
        draft.receive(snapshotValue: 37)
        expect(draft.displayed(snapshotValue: 37) == 37, "matching snapshot acknowledges the draft")
        draft.receive(snapshotValue: 42)
        expect(draft.displayed(snapshotValue: 42) == 42, "later external snapshots update the settled control")

        var acknowledgedWhileEditing = ProtectionBrightnessDraft()
        acknowledgedWhileEditing.begin(snapshotValue: 1)
        _ = acknowledgedWhileEditing.set(23)
        acknowledgedWhileEditing.receive(snapshotValue: 23)
        acknowledgedWhileEditing.endEditing()
        acknowledgedWhileEditing.receive(snapshotValue: 27)
        expect(acknowledgedWhileEditing.displayed(snapshotValue: 27) == 27,
               "a snapshot acknowledged during drag does not leave the settled control stuck on its draft")

        expect(ProtectionBrightnessDraft.clamp(0) == 1, "range has a 1 percent minimum")
        expect(ProtectionBrightnessDraft.clamp(80) == 50, "range has a 50 percent maximum")
        print("Protection brightness draft tests passed")
    }
}
