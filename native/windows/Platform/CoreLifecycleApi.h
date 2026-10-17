#pragma once
// Thin RAII wrapper over the core camera lifecycle ABI (tv_camera_lifecycle_*). It adds
// no policy: each call sends one JSON event and returns the core's result text unchanged.
// The core reads no clock, so every timed event carries steady-clock seconds from the caller.
#include <mutex>
#include <string>

// Opaque handle defined by the core in include/telaviva_core.h. Declared here so this
// header does not pull the C ABI into every includer.
struct TvCameraLifecycle;

namespace telaviva {

// Seam for the lifecycle driver, so tests can replace the real core.
struct ILifecycleApi {
    virtual ~ILifecycleApi() = default;
    // Returns the core's {"value": ..., "state": {...}} text. Returns an empty string on an
    // ABI error, such as a rejected event; a rejected event leaves the lifecycle unchanged.
    virtual std::string step(const std::string& eventJson) = 0;
};

// Owns one TvCameraLifecycle. Created in the constructor and destroyed in the destructor.
// step() is serialized by a mutex, so the driver may call it from timer and camera threads.
class CoreLifecycleApi : public ILifecycleApi {
public:
    CoreLifecycleApi();
    ~CoreLifecycleApi() override;

    CoreLifecycleApi(const CoreLifecycleApi&) = delete;
    CoreLifecycleApi& operator=(const CoreLifecycleApi&) = delete;

    std::string step(const std::string& eventJson) override;

private:
    std::mutex mutex_;
    // Null when creation failed; step() then returns an empty string.
    TvCameraLifecycle* handle_ = nullptr;
};

}  // namespace telaviva
