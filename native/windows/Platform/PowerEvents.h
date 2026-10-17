#pragma once
// Turns AC/battery power source and lid state changes into the core's commands. The app
// registers handleMessage as a handler on its hidden window. This header does
// not depend on the app's own headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace telaviva {

// Mirrors the fields GetSystemPowerStatus fills, so the mapping can be tested
// without the OS.
struct IPowerStatusReader {
    virtual ~IPowerStatusReader() = default;
    // Returns false when the status cannot be read.
    virtual bool read(uint8_t& acLineStatus, uint8_t& batteryFlag) = 0;
};

class SystemPowerStatusReader : public IPowerStatusReader {
public:
    bool read(uint8_t& acLineStatus, uint8_t& batteryFlag) override;
};

// Pure mapping from GetSystemPowerStatus fields. std::nullopt means unknown.
std::optional<bool> onBatteryFromStatus(uint8_t acLineStatus, uint8_t batteryFlag);

// onPower runs on the UI thread from handleMessage and publishCurrent. onLid runs
// on the UI thread from handleMessage; true means the lid is open.
struct PowerEventCallbacks {
    std::function<void(std::optional<bool> onBattery)> onPower;
    std::function<void(bool open)> onLid;
};

class PowerEvents {
public:
    PowerEvents(IPowerStatusReader& reader, PowerEventCallbacks callbacks);

    PowerEvents(const PowerEvents&) = delete;
    PowerEvents& operator=(const PowerEvents&) = delete;

    // Reads the status and reports it once. Used at startup and after resume.
    void publishCurrent();

    // Returns std::nullopt for messages it does not consume or that must also
    // reach other handlers. Never throws.
    std::optional<LRESULT> handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Thin wrappers over the OS registration (not unit-tested). registerFor
    // replaces any earlier registration; unregisterFor releases the last one.
    static bool registerFor(HWND window);
    static void unregisterFor();

private:
    IPowerStatusReader& reader_;
    PowerEventCallbacks callbacks_;
};

}  // namespace telaviva
