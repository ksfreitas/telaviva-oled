#pragma once
// Scripted camera for tests. The test decides when frames, failures and device
// changes happen; the fake records open and close calls and counts deliveries.
// Not thread-safe; tests drive it from one thread.
#include "Platform/CameraSelection.h"
#include "Platform/CameraStatus.h"
#include "Platform/ICameraBackend.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace telaviva {

class FakeCamera final : public ICameraBackend {
public:
    struct OpenCall {
        std::string id;
        double targetFps;
    };

    struct Counters {
        int enumerate = 0;
        int open = 0;
        int close = 0;
        int frames = 0;
        int failures = 0;
        int devicesChanged = 0;
    };

    std::vector<CameraInfo> enumerate() override
    {
        ++counters_.enumerate;
        return devices_;
    }

    bool open(const std::string& cameraId, double targetFps, ICameraBackendCallbacks& callbacks) override
    {
        ++counters_.open;
        openCalls_.push_back(OpenCall{cameraId, targetFps});
        if (openFailure_) {
            callbacks.onFailure(*openFailure_);
            return false;
        }
        callbacks_ = &callbacks;
        open_ = true;
        return true;
    }

    void close() override
    {
        ++counters_.close;
        open_ = false;
        callbacks_ = nullptr;
    }

    bool isOpen() const override { return open_; }

    void setDevices(std::vector<CameraInfo> devices) { devices_ = std::move(devices); }

    // Makes the next open() calls fail with this failure until cleared.
    void setOpenFailure(const CameraFailure& failure) { openFailure_ = failure; }
    void clearOpenFailure() { openFailure_.reset(); }

    // Builds a BGRA frame with every pixel set to the same value. Events are
    // dropped while closed, matching a backend that has stopped delivering.
    void emitFrame(int width, int height, uint8_t value, double steadySeconds = 0.0)
    {
        if (!open_ || callbacks_ == nullptr) {
            return;
        }
        CameraFrame frame;
        frame.width = width;
        frame.height = height;
        frame.stride = width * 4;
        frame.steadySeconds = steadySeconds;
        const std::size_t length = static_cast<std::size_t>(frame.stride) * static_cast<std::size_t>(height);
        frame.bgra.assign(length, value);
        for (std::size_t i = 3; i < length; i += 4) {
            frame.bgra[i] = 0xFF;
        }
        ++counters_.frames;
        callbacks_->onFrame(frame);
    }

    void emitFailure(const CameraFailure& failure)
    {
        if (!open_ || callbacks_ == nullptr) {
            return;
        }
        ++counters_.failures;
        callbacks_->onFailure(failure);
    }

    void emitDevicesChanged()
    {
        if (!open_ || callbacks_ == nullptr) {
            return;
        }
        ++counters_.devicesChanged;
        callbacks_->onDevicesChanged();
    }

    const std::vector<OpenCall>& openCalls() const { return openCalls_; }
    const Counters& counters() const { return counters_; }

private:
    std::vector<CameraInfo> devices_;
    std::optional<CameraFailure> openFailure_;
    std::vector<OpenCall> openCalls_;
    Counters counters_;
    ICameraBackendCallbacks* callbacks_ = nullptr;
    bool open_ = false;
};

}  // namespace telaviva
