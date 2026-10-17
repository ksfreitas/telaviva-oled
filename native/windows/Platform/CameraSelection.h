#pragma once
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

struct CameraInfo {
    std::string id;  // DeviceInformation id or symbolic link; the stable key
    std::wstring name;
    bool infrared = false;
    bool builtIn = false;  // front panel or internal
};

// Infrared cameras are hidden until they are validated with the detector (Q8). Order is preserved.
std::vector<CameraInfo> selectableCameras(const std::vector<CameraInfo>& connected);

struct CameraChoice {
    std::optional<CameraInfo> effective;
    // True whenever the default camera was used instead of a history entry.
    // This is a runtime choice only and is never written back to settings.
    bool fallbackToDefault = false;
};

// history is ordered most recent first. The default is used only when no history entry is selectable.
CameraChoice chooseCamera(const std::vector<CameraInfo>& connected, const std::vector<std::string>& history);

// Value for the core camera lifecycle camera_id: history entries joined by "\n", or the selected id
// alone when the history is empty, or an empty string when neither exists.
std::string lifecycleCameraIdList(const std::vector<std::string>& history, const std::optional<std::string>& selectedId);

}  // namespace telaviva
