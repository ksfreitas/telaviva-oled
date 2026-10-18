#pragma once
// Thin RAII wrapper over the core controls ABI (tv_controls_*). It adds no policy: each call
// sends one JSON event and returns the core's result text unchanged.
#include <mutex>
#include <string>

// Opaque handle defined by the core in include/telaviva_core.h. Declared here so this
// header does not pull the C ABI into every includer.
struct TvControls;

namespace telaviva {

// Seam for the controls driver, so tests can replace the real core.
struct IControlsApi {
    virtual ~IControlsApi() = default;
    // One JSON event in, one JSON result out; an empty string means the ABI call failed.
    virtual std::string step(const std::string& eventJson) = 0;
};

// Owns one TvControls. Created in the constructor and destroyed in the destructor.
// step() is serialized by a mutex, so the driver may call it from several threads.
class CoreControlsApi final : public IControlsApi {
public:
    CoreControlsApi();
    ~CoreControlsApi() override;

    CoreControlsApi(const CoreControlsApi&) = delete;
    CoreControlsApi& operator=(const CoreControlsApi&) = delete;

    std::string step(const std::string& eventJson) override;

private:
    std::mutex mutex_;
    // Null when creation failed; step() then returns an empty string.
    TvControls* handle_ = nullptr;
};

}  // namespace telaviva
