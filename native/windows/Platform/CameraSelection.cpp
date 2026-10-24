#include "CameraSelection.h"

#include "CameraFormatSelection.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace telaviva {

namespace {

using camera_format_detail::equalsIgnoreAsciiCase;

bool groupHasDevice(const CameraGroupRecord& group, const std::string& deviceId)
{
    for (const CameraSourceRecord& source : group.sources) {
        if (!source.deviceId.empty() && equalsIgnoreAsciiCase(source.deviceId, deviceId)) {
            return true;
        }
    }
    return false;
}

bool groupHasKind(const CameraGroupRecord& group, CameraSourceKind kind)
{
    for (const CameraSourceRecord& source : group.sources) {
        if (source.kind == kind) {
            return true;
        }
    }
    return false;
}

// Infrared when the device's own source is infrared and the group has no color source.
bool deviceIsInfrared(const CameraGroupRecord& group, const std::string& deviceId)
{
    bool matchedInfrared = false;
    for (const CameraSourceRecord& source : group.sources) {
        if (source.kind == CameraSourceKind::Infrared && !source.deviceId.empty() &&
            equalsIgnoreAsciiCase(source.deviceId, deviceId)) {
            matchedInfrared = true;
        }
    }
    return isInfraredOnlySource(groupHasKind(group, CameraSourceKind::Color), matchedInfrared);
}

// Same id and same name as a device in the list: the entry the device list itself reports.
bool matchesDeviceName(const CameraInfo& camera, const std::vector<CameraDeviceRecord>& devices)
{
    for (const CameraDeviceRecord& device : devices) {
        if (device.name == camera.name && equalsIgnoreAsciiCase(device.id, camera.id)) {
            return true;
        }
    }
    return false;
}

// Infrared is excluded here too, so a history entry pointing at an IR camera is skipped.
// A built-in camera is skipped while the lid is closed.
const CameraInfo* findSelectable(const std::vector<CameraInfo>& connected, const std::string& id, bool lidClosed)
{
    for (const CameraInfo& camera : connected) {
        if (camera.id == id && !camera.infrared && !(lidClosed && camera.builtIn)) {
            return &camera;
        }
    }
    return nullptr;
}

}  // namespace

std::size_t chooseGroupForDevice(const std::vector<CameraGroupRecord>& groups, const std::string& deviceId,
                                 const std::wstring& deviceName)
{
    std::size_t best = kNoCameraGroup;
    int bestScore = -1;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const CameraGroupRecord& group = groups[i];
        if (!groupHasDevice(group, deviceId)) {
            continue;
        }
        // The name outranks the color source: the device's own group is named after it, while a
        // virtual group that shares the id is not.
        const int score = (!deviceName.empty() && group.name == deviceName ? 2 : 0) +
                          (groupHasKind(group, CameraSourceKind::Color) ? 1 : 0);
        if (score > bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

std::vector<CameraInfo> mergeCameraLists(const std::vector<CameraDeviceRecord>& devices,
                                         const std::vector<CameraGroupRecord>& groups)
{
    std::vector<CameraInfo> cameras;
    cameras.reserve(devices.size());
    for (const CameraDeviceRecord& device : devices) {
        CameraInfo camera;
        camera.id = device.id;
        camera.name = device.name;
        const std::size_t index = chooseGroupForDevice(groups, device.id, device.name);
        camera.infrared = index != kNoCameraGroup && deviceIsInfrared(groups[index], device.id);
        camera.builtIn = device.builtIn && !camera.infrared;
        cameras.push_back(std::move(camera));
    }
    return dedupeCameras(cameras, devices);
}

std::vector<CameraInfo> dedupeCameras(const std::vector<CameraInfo>& cameras,
                                      const std::vector<CameraDeviceRecord>& devices)
{
    std::vector<CameraInfo> result;
    std::vector<bool> consumed(cameras.size(), false);
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        if (consumed[i]) {
            continue;
        }
        std::size_t chosen = i;
        bool chosenNamed = matchesDeviceName(cameras[i], devices);
        consumed[i] = true;
        for (std::size_t j = i + 1; j < cameras.size(); ++j) {
            if (consumed[j] || !equalsIgnoreAsciiCase(cameras[j].id, cameras[i].id)) {
                continue;
            }
            consumed[j] = true;
            if (!chosenNamed && matchesDeviceName(cameras[j], devices)) {
                chosen = j;
                chosenNamed = true;
            }
        }
        result.push_back(cameras[chosen]);
    }
    return result;
}

std::optional<std::size_t> findCameraIndex(const std::vector<CameraInfo>& cameras, const std::optional<std::string>& id)
{
    if (!id || id->empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        if (equalsIgnoreAsciiCase(cameras[i].id, *id)) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<CameraInfo> selectableCameras(const std::vector<CameraInfo>& connected, bool lidClosed)
{
    std::vector<CameraInfo> result;
    for (const CameraInfo& camera : connected) {
        if (!camera.infrared && !(lidClosed && camera.builtIn)) {
            result.push_back(camera);
        }
    }
    return result;
}

CameraChoice chooseCamera(const std::vector<CameraInfo>& connected, const std::vector<std::string>& history,
                          bool lidClosed)
{
    CameraChoice choice;
    for (const std::string& id : history) {
        if (const CameraInfo* camera = findSelectable(connected, id, lidClosed)) {
            choice.effective = *camera;
            return choice;
        }
    }

    // Prefer the first built-in selectable camera; otherwise the first selectable one.
    const CameraInfo* fallback = nullptr;
    for (const CameraInfo& camera : connected) {
        if (camera.infrared || (lidClosed && camera.builtIn)) {
            continue;
        }
        if (camera.builtIn) {
            fallback = &camera;
            break;
        }
        if (fallback == nullptr) {
            fallback = &camera;
        }
    }

    if (fallback != nullptr) {
        choice.effective = *fallback;
        choice.fallbackToDefault = true;
    }
    return choice;
}

std::string lifecycleCameraIdList(const std::vector<std::string>& history, const std::optional<std::string>& selectedId)
{
    if (history.empty()) {
        return selectedId.value_or(std::string());
    }

    std::string list;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (i > 0) {
            list += '\n';
        }
        list += history[i];
    }
    return list;
}

}  // namespace telaviva
