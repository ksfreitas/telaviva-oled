import Foundation

/// Picks the camera to use from the selection history, most recent first.
/// Returns the first history ID that is currently connected, or nil when none is, meaning the system default camera.
/// Duplicate history entries are harmless because the first match wins.
func resolveCamera(history: [String], connected: Set<String>) -> String? {
    history.first { connected.contains($0) }
}

/// A camera the session could use, with the one fact the lid rule needs.
struct CameraCandidate: Equatable {
    let id: String
    let isBuiltIn: Bool
}

/// The IDs usable now. While the lid is closed the built-in camera is left out, as if it were disconnected,
/// so it is never resolved, even when it is the remembered choice or the system default.
func selectableCameraIDs(_ candidates: [CameraCandidate], lidClosed: Bool) -> Set<String> {
    Set(candidates.filter { !(lidClosed && $0.isBuiltIn) }.map(\.id))
}
