import Foundation

private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError("CameraSelectionTests: \(message)") }
}

@main
private struct CameraSelectionTests {
    static func main() {
        // The most recent connected selection wins.
        expect(resolveCamera(history: ["camera-a", "camera-b"], connected: ["camera-a", "camera-b"]) == "camera-a",
               "connected front entry must be selected")

        // A missing front entry falls back to the next connected history entry.
        expect(resolveCamera(history: ["camera-a", "camera-b"], connected: ["camera-b"]) == "camera-b",
               "second entry must be selected when the front is missing")

        // No connected history entry resolves to nil, which means the system default camera.
        expect(resolveCamera(history: ["camera-a", "camera-b"], connected: ["camera-c"]) == nil,
               "unconnected history must resolve to nil")
        expect(resolveCamera(history: ["camera-a", "camera-b"], connected: []) == nil,
               "no connected devices must resolve to nil")

        // An empty history resolves to nil, which means the system default camera.
        expect(resolveCamera(history: [], connected: ["camera-a"]) == nil,
               "empty history must resolve to nil")

        // Duplicate history entries do not change the first-match result.
        expect(resolveCamera(history: ["camera-a", "camera-a", "camera-b"], connected: ["camera-b"]) == "camera-b",
               "duplicate missing entries must be skipped")
        expect(resolveCamera(history: ["camera-a", "camera-b", "camera-a"], connected: ["camera-a"]) == "camera-a",
               "duplicate later entries must not change the match")

        // Lid closed: the built-in camera is left out, so only the external camera is usable.
        let candidates = [CameraCandidate(id: "bi", isBuiltIn: true), CameraCandidate(id: "ext", isBuiltIn: false)]
        let closedSet = selectableCameraIDs(candidates, lidClosed: true)
        expect(closedSet == ["ext"], "closed lid must leave only the external camera")
        expect(resolveCamera(history: ["bi", "ext"], connected: closedSet) == "ext",
               "closed lid must skip a built-in choice that ranks ahead of the external camera")
        expect(resolveCamera(history: ["bi"], connected: closedSet) == nil,
               "closed lid with only the built-in camera in history must resolve to nil")

        // Lid open: the built-in camera is usable again and the remembered choice resolves to it.
        let openSet = selectableCameraIDs(candidates, lidClosed: false)
        expect(openSet == ["bi", "ext"], "open lid must keep both cameras")
        expect(resolveCamera(history: ["bi"], connected: openSet) == "bi",
               "open lid must resolve the built-in camera again")

        print("Camera selection resolver tests passed")
    }
}
