#include "CameraSelection.h"

#include <cstddef>

namespace telaviva {

namespace {

// Infrared is excluded here too, so a history entry pointing at an IR camera is skipped.
const CameraInfo* findSelectable(const std::vector<CameraInfo>& connected, const std::string& id)
{
    for (const CameraInfo& camera : connected) {
        if (camera.id == id && !camera.infrared) {
            return &camera;
        }
    }
    return nullptr;
}

}  // namespace

std::vector<CameraInfo> selectableCameras(const std::vector<CameraInfo>& connected)
{
    std::vector<CameraInfo> result;
    for (const CameraInfo& camera : connected) {
        if (!camera.infrared) {
            result.push_back(camera);
        }
    }
    return result;
}

CameraChoice chooseCamera(const std::vector<CameraInfo>& connected, const std::vector<std::string>& history)
{
    CameraChoice choice;
    for (const std::string& id : history) {
        if (const CameraInfo* camera = findSelectable(connected, id)) {
            choice.effective = *camera;
            return choice;
        }
    }

    // Prefer the first built-in selectable camera; otherwise the first selectable one.
    const CameraInfo* fallback = nullptr;
    for (const CameraInfo& camera : connected) {
        if (camera.infrared) {
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
