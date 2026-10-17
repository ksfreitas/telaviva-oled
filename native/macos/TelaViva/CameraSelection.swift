import Foundation

/// Picks the camera to use from the selection history, most recent first.
/// Returns the first history ID that is currently connected, or nil when none is, meaning the system default camera.
/// Duplicate history entries are harmless because the first match wins.
func resolveCamera(history: [String], connected: Set<String>) -> String? {
    history.first { connected.contains($0) }
}
