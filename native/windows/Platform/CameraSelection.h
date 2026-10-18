#pragma once
#include <cstddef>
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

// Raw inputs for mergeCameraLists, filled from WinRT by the backend. No WinRT types here.
enum class CameraSourceKind { Color, Infrared, Other };

struct CameraSourceRecord {
    CameraSourceKind kind = CameraSourceKind::Other;
    std::string deviceId;  // DeviceInformation id of the source; empty when it has none
};

struct CameraGroupRecord {
    std::wstring name;  // MediaFrameSourceGroup.DisplayName
    std::vector<CameraSourceRecord> sources;
};

struct CameraDeviceRecord {
    std::string id;  // DeviceInformation id of a VideoCapture device
    std::wstring name;
    bool builtIn = false;
};

inline constexpr std::size_t kNoCameraGroup = static_cast<std::size_t>(-1);

// Index of the group that backs a device id, or kNoCameraGroup. A group matches when one of its
// sources has that device id. A group whose name equals deviceName wins, then a group with a color
// source, then the first in order.
std::size_t chooseGroupForDevice(const std::vector<CameraGroupRecord>& groups, const std::string& deviceId,
                                 const std::wstring& deviceName);

// Camera list as other apps show it: one entry per VideoCapture device, with infrared classified
// through the matching group. A group with no matching device is not listed. Ids are unique.
std::vector<CameraInfo> mergeCameraLists(const std::vector<CameraDeviceRecord>& devices,
                                         const std::vector<CameraGroupRecord>& groups);

// Keeps one entry per id (ASCII case-insensitive), at the position of its first occurrence. An entry whose
// name equals the name of a device with the same id is kept in preference to earlier duplicates.
std::vector<CameraInfo> dedupeCameras(const std::vector<CameraInfo>& cameras,
                                      const std::vector<CameraDeviceRecord>& devices);

// Index of the camera whose id equals the stored id exactly (ASCII case-insensitive). Nullopt when the
// stored id is absent or no camera matches; the caller then shows the default item.
std::optional<std::size_t> findCameraIndex(const std::vector<CameraInfo>& cameras,
                                           const std::optional<std::string>& id);

struct CameraChoice {
    std::optional<CameraInfo> effective;
    // True whenever the default camera was used instead of a history entry.
    // This is a runtime choice only and is never written back to settings.
    bool fallbackToDefault = false;
};

// Infrared cameras are hidden until they are validated with the detector (Q8). Order is preserved.
std::vector<CameraInfo> selectableCameras(const std::vector<CameraInfo>& connected);

// history is ordered most recent first. The default is used only when no history entry is selectable.
CameraChoice chooseCamera(const std::vector<CameraInfo>& connected, const std::vector<std::string>& history);

// Value for the core camera lifecycle camera_id: history entries joined by "\n", or the selected id
// alone when the history is empty, or an empty string when neither exists.
std::string lifecycleCameraIdList(const std::vector<std::string>& history, const std::optional<std::string>& selectedId);

}  // namespace telaviva
