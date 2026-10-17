#include "CoreLifecycleApi.h"

#include <telaviva_core.h>

#include <cstddef>
#include <cstdint>

namespace telaviva {

CoreLifecycleApi::CoreLifecycleApi()
{
    // Write to a local first, so a failed create never leaves a stale pointer in handle_.
    TvCameraLifecycle* created = nullptr;
    if (tv_camera_lifecycle_create(&created) == TV_OK) {
        handle_ = created;
    }
}

CoreLifecycleApi::~CoreLifecycleApi()
{
    // The destructor runs when no other thread can call step(), so it needs no lock.
    tv_camera_lifecycle_destroy(handle_);
}

std::string CoreLifecycleApi::step(const std::string& eventJson)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ == nullptr) {
        return std::string();
    }

    TvBuffer buffer{nullptr, 0};
    const int32_t status = tv_camera_lifecycle_step(handle_, reinterpret_cast<const uint8_t*>(eventJson.data()),
                                                    eventJson.size(), &buffer);
    // The core writes no buffer when a step fails, so there is nothing to free.
    if (status != TV_OK) {
        return std::string();
    }

    std::string result;
    if (buffer.data != nullptr) {
        result.assign(reinterpret_cast<const char*>(buffer.data), buffer.len);
    }
    tv_buffer_free(&buffer);
    return result;
}

}  // namespace telaviva
