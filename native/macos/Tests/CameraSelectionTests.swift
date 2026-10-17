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

        print("Camera selection resolver tests passed")
    }
}
