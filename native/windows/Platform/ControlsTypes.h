#pragma once
// Plain value types exchanged with the core's manual-control queue (TvControls).
// JSON conversion lives in JsonCodec.h; these types carry no WinRT dependency.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace telaviva {

// The draft a slider or key control shows. An idle control reports the latest observed value.
struct DraftView {
    bool hasDraft = false;
    double draft = 0.0;
    std::string status = "ready";  // ready | pending | failed
};

// The monitor a key event was resolved to, and the generation it was resolved against.
struct KeyOwnerRef {
    std::string monitorId;
    uint64_t generation = 0;
};

// One instruction the shell executes, in array order, outside its own locks.
struct ControlsEffect {
    enum class Type { SubmitCommand, RunAttempt, SubmitResult, DraftChanged };

    Type type = Type::DraftChanged;
    uint64_t token = 0;
    std::string json;             // command_json (SubmitCommand) or result_json (SubmitResult), passed verbatim
    std::string monitorId;        // RunAttempt and DraftChanged
    bool write = false;           // RunAttempt: op == "write"
    uint8_t code = 0;             // RunAttempt: VCP code (16 brightness, 98 volume)
    std::optional<double> value;  // RunAttempt write: raw value
    std::string feature;          // DraftChanged: "brightness" or "volume"
};

// The parsed result of one controls event.
struct ControlsResult {
    std::vector<ControlsEffect> effects;
    std::optional<double> nextDeadline;  // absolute monotonic seconds; absent when the core sent null
    std::optional<DraftView> draft;      // slider and query_draft results; absent when the value was null
    std::optional<bool> consume;         // key results
};

}  // namespace telaviva
