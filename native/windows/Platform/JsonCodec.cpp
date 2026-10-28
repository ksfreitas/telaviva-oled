#include "JsonCodec.h"

#include <windows.h>

// The C++/WinRT projection headers emit warnings the /W4 build would reject.
#pragma warning(push, 0)
#include <winrt/base.h>
// The collection interfaces behind JsonObject and JsonArray must be projected
// before Windows.Data.Json.h uses them, or the auto-returning members fail (C3779).
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#pragma warning(pop)

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace telaviva {
namespace {

namespace json = winrt::Windows::Data::Json;
using Object = json::JsonObject;

void appendQuoted(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned>(byte));
                out += escape;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

// Shortest round-trip form, so 41.0 is written as 41. JSON has no NaN or infinity.
void appendNumber(std::string& out, double value)
{
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    char buffer[32];
    const char* end = std::to_chars(buffer, buffer + sizeof buffer, value).ptr;
    // to_chars reports a pointer, not a size; append needs (char*, size) to match.
    out.append(buffer, static_cast<std::size_t>(end - buffer));
}

// Builds one JSON object, keeping comma placement in one place.
class ObjectWriter {
public:
    ObjectWriter() : out_("{") {}

    ObjectWriter& addString(std::string_view key, std::string_view value)
    {
        beginMember(key);
        appendQuoted(out_, value);
        return *this;
    }

    ObjectWriter& addNumber(std::string_view key, double value)
    {
        beginMember(key);
        appendNumber(out_, value);
        return *this;
    }

    ObjectWriter& addUnsigned(std::string_view key, uint64_t value)
    {
        beginMember(key);
        out_ += std::to_string(value);
        return *this;
    }

    ObjectWriter& addBool(std::string_view key, bool value)
    {
        beginMember(key);
        out_ += value ? "true" : "false";
        return *this;
    }

    ObjectWriter& addOptionalNumber(std::string_view key, const std::optional<double>& value)
    {
        beginMember(key);
        if (value) {
            appendNumber(out_, *value);
        } else {
            out_ += "null";
        }
        return *this;
    }

    // Embeds json unchanged; the caller guarantees it is valid JSON.
    ObjectWriter& addRaw(std::string_view key, std::string_view fragment)
    {
        beginMember(key);
        out_ += fragment;
        return *this;
    }

    std::string finish()
    {
        out_.push_back('}');
        return std::move(out_);
    }

private:
    void beginMember(std::string_view key)
    {
        if (!first_) {
            out_.push_back(',');
        }
        first_ = false;
        appendQuoted(out_, key);
        out_.push_back(':');
    }

    std::string out_;
    bool first_ = true;
};

std::string channelJson(const MonitorInfo::Channel& channel)
{
    ObjectWriter writer;
    writer.addBool("supported", channel.supported);
    // An unsupported channel carries only the flag; the core treats missing values as absent.
    if (channel.supported) {
        if (channel.raw) {
            writer.addNumber("raw", *channel.raw);
        }
        writer.addNumber("min", 0.0);
        if (channel.max) {
            writer.addNumber("max", *channel.max);
        }
    }
    return writer.finish();
}

// WinRT needs an initialized apartment on every thread that touches it, so this
// runs once per thread rather than once per process.
void ensureApartment()
{
    thread_local bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        // The thread already has another apartment; parsing works there too.
        if (error.code() != static_cast<winrt::hresult>(RPC_E_CHANGED_MODE)) {
            throw std::runtime_error("failed to initialize a WinRT apartment");
        }
    }
}

// Missing keys yield a null IJsonValue, so every reader checks for it first.
json::IJsonValue fieldValue(const Object& object, std::string_view key)
{
    const winrt::hstring name = winrt::to_hstring(key);
    if (!object.HasKey(name)) {
        return nullptr;
    }
    return object.GetNamedValue(name);
}

std::optional<Object> asObject(const json::IJsonValue& value)
{
    if (!value || value.ValueType() != json::JsonValueType::Object) {
        return std::nullopt;
    }
    return value.GetObject();
}

std::optional<Object> objectField(const Object& object, std::string_view key)
{
    return asObject(fieldValue(object, key));
}

std::optional<json::JsonArray> arrayField(const Object& object, std::string_view key)
{
    const json::IJsonValue value = fieldValue(object, key);
    if (!value || value.ValueType() != json::JsonValueType::Array) {
        return std::nullopt;
    }
    return value.GetArray();
}

std::optional<double> numberField(const Object& object, std::string_view key)
{
    const json::IJsonValue value = fieldValue(object, key);
    if (!value || value.ValueType() != json::JsonValueType::Number) {
        return std::nullopt;
    }
    return value.GetNumber();
}

std::optional<bool> boolField(const Object& object, std::string_view key)
{
    const json::IJsonValue value = fieldValue(object, key);
    if (!value || value.ValueType() != json::JsonValueType::Boolean) {
        return std::nullopt;
    }
    return value.GetBoolean();
}

std::optional<std::string> stringField(const Object& object, std::string_view key)
{
    const json::IJsonValue value = fieldValue(object, key);
    if (!value || value.ValueType() != json::JsonValueType::String) {
        return std::nullopt;
    }
    return winrt::to_string(value.GetString());
}

double numberOr(const Object& object, std::string_view key, double fallback)
{
    return numberField(object, key).value_or(fallback);
}

bool boolOr(const Object& object, std::string_view key, bool fallback)
{
    return boolField(object, key).value_or(fallback);
}

std::string stringOr(const Object& object, std::string_view key, std::string_view fallback)
{
    return stringField(object, key).value_or(std::string(fallback));
}

int intOr(const Object& object, std::string_view key, int fallback)
{
    const double value = numberOr(object, key, static_cast<double>(fallback));
    if (!(value >= -2147483648.0 && value <= 2147483647.0)) {
        return fallback;
    }
    return static_cast<int>(value);
}

// Core ids and generations are whole numbers; clamp anything outside the range
// rather than invoking undefined behavior in the conversion.
uint64_t toUnsigned(double value)
{
    if (!(value > 0.0)) {
        return 0;
    }
    if (value >= 18446744073709551616.0) {
        return UINT64_MAX;
    }
    return static_cast<uint64_t>(value);
}

uint64_t unsignedOr(const Object& object, std::string_view key)
{
    return toUnsigned(numberOr(object, key, 0.0));
}

// Keys are kept as written; non-number values are skipped rather than failing the snapshot.
std::map<std::string, double> numberMapField(const Object& object, std::string_view key)
{
    std::map<std::string, double> result;
    const std::optional<Object> nested = objectField(object, key);
    if (!nested) {
        return result;
    }
    auto entries = nested->First();
    while (entries.HasCurrent()) {
        const auto entry = entries.Current();
        const json::IJsonValue value = entry.Value();
        if (value && value.ValueType() == json::JsonValueType::Number) {
            result[winrt::to_string(entry.Key())] = value.GetNumber();
        }
        entries.MoveNext();
    }
    return result;
}

std::set<std::string> stringSetField(const Object& object, std::string_view key)
{
    std::set<std::string> result;
    const std::optional<json::JsonArray> values = arrayField(object, key);
    if (!values) {
        return result;
    }
    for (uint32_t i = 0; i < values->Size(); ++i) {
        const json::IJsonValue value = values->GetAt(i);
        if (value && value.ValueType() == json::JsonValueType::String) {
            result.insert(winrt::to_string(value.GetString()));
        }
    }
    return result;
}

// Keeps the order the core sent; non-string values are skipped.
std::vector<std::string> stringVectorField(const Object& object, std::string_view key)
{
    std::vector<std::string> result;
    const std::optional<json::JsonArray> values = arrayField(object, key);
    if (!values) {
        return result;
    }
    for (uint32_t i = 0; i < values->Size(); ++i) {
        const json::IJsonValue value = values->GetAt(i);
        if (value && value.ValueType() == json::JsonValueType::String) {
            result.push_back(winrt::to_string(value.GetString()));
        }
    }
    return result;
}

// A monitor whose value is not an object with a points array is dropped. Inside a
// kept curve, a point missing a numeric monitor or builtin is dropped alone.
AmbientCurvesSnapshot ambientCurvesField(const Object& object, std::string_view key)
{
    AmbientCurvesSnapshot result;
    const std::optional<Object> curves = objectField(object, key);
    if (!curves) {
        return result;
    }
    auto entries = curves->First();
    while (entries.HasCurrent()) {
        const auto entry = entries.Current();
        const std::optional<Object> curve = asObject(entry.Value());
        const std::optional<json::JsonArray> points =
            curve ? arrayField(*curve, "points") : std::optional<json::JsonArray>{};
        if (points) {
            std::vector<AmbientCurvePointSnapshot> parsed;
            for (uint32_t i = 0; i < points->Size(); ++i) {
                if (const auto point = asObject(points->GetAt(i))) {
                    const std::optional<double> monitor = numberField(*point, "monitor");
                    const std::optional<double> builtin = numberField(*point, "builtin");
                    if (monitor && builtin) {
                        parsed.push_back(AmbientCurvePointSnapshot{*monitor, *builtin});
                    }
                }
            }
            result[winrt::to_string(entry.Key())] = std::move(parsed);
        }
        entries.MoveNext();
    }
    return result;
}

Object parseRoot(const std::string& text)
{
    ensureApartment();
    std::optional<json::JsonValue> parsed;
    try {
        parsed = json::JsonValue::Parse(winrt::to_hstring(std::string_view(text)));
    } catch (const winrt::hresult_error&) {
        throw std::invalid_argument("snapshot is not valid JSON");
    }
    if (parsed->ValueType() != json::JsonValueType::Object) {
        throw std::invalid_argument("snapshot root must be a JSON object");
    }
    return parsed->GetObject();
}

ChannelSnapshot parseChannel(const Object& object)
{
    ChannelSnapshot channel;
    channel.supported = boolOr(object, "supported", false);
    channel.observed = numberField(object, "observed");
    channel.normalized = numberField(object, "normalized");
    channel.raw = numberField(object, "raw");
    channel.desired = numberField(object, "desired");
    channel.min = numberOr(object, "min", 0.0);
    channel.max = numberOr(object, "max", 0.0);
    channel.status = stringOr(object, "status", "");
    return channel;
}

MonitorSnapshot parseMonitor(const Object& object)
{
    MonitorSnapshot monitor;
    monitor.id = stringOr(object, "id", "");
    monitor.name = stringOr(object, "name", "");
    monitor.identityStatus = stringOr(object, "identity_status", "");
    monitor.connectionPath = stringOr(object, "connection_path", "");
    monitor.generation = unsignedOr(object, "generation");
    monitor.displayState = stringOr(object, "display_state", "");
    monitor.follow = stringOr(object, "follow", "");
    monitor.protected_ = boolOr(object, "protected", false);
    if (const auto brightness = objectField(object, "brightness")) {
        monitor.brightness = parseChannel(*brightness);
    }
    if (const auto volume = objectField(object, "volume")) {
        monitor.volume = parseChannel(*volume);
    }
    return monitor;
}

PendingAction parsePendingAction(const Object& object)
{
    PendingAction action;
    action.id = unsignedOr(object, "id");
    action.monitorId = stringOr(object, "monitor_id", "");
    action.generation = unsignedOr(object, "generation");
    action.kind = stringOr(object, "kind", "");
    action.value = numberField(object, "value");
    action.target = numberField(object, "target");
    return action;
}

// Starts an event object with its type and shell clock, which every controls event carries.
ObjectWriter eventWriter(std::string_view type, double now)
{
    ObjectWriter message;
    message.addString("type", type).addNumber("now", now);
    return message;
}

// Joins already-built JSON values into an array.
std::string arrayJson(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out += items[i];
    }
    out.push_back(']');
    return out;
}

// Controls observe carries the normalized percentage, never the raw observed value.
std::string controlsChannelJson(const ChannelSnapshot& channel)
{
    ObjectWriter writer;
    writer.addBool("supported", channel.supported)
        .addOptionalNumber("observed", channel.normalized)
        .addNumber("min", channel.min)
        .addNumber("max", channel.max);
    return writer.finish();
}

std::string keyOwnerJson(const KeyOwnerRef& owner)
{
    ObjectWriter writer;
    writer.addString("monitor_id", owner.monitorId).addUnsigned("generation", owner.generation);
    return writer.finish();
}

[[noreturn]] void throwControls(std::string_view detail)
{
    throw std::invalid_argument("controls result " + std::string(detail));
}

bool hasField(const Object& object, std::string_view key)
{
    return static_cast<bool>(fieldValue(object, key));
}

double requireNumber(const Object& object, std::string_view key)
{
    const std::optional<double> value = numberField(object, key);
    if (!value) {
        throwControls(std::string("has a missing or non-numeric '") + std::string(key) + "'");
    }
    return *value;
}

// Tokens, generations and codes are whole, non-negative numbers below 2^64.
uint64_t requireWhole(const Object& object, std::string_view key)
{
    const double value = requireNumber(object, key);
    if (!std::isfinite(value) || value < 0.0 || std::floor(value) != value ||
        value >= 18446744073709551616.0) {
        throwControls(std::string("has a non-integer '") + std::string(key) + "'");
    }
    return static_cast<uint64_t>(value);
}

std::string requireString(const Object& object, std::string_view key)
{
    const std::optional<std::string> value = stringField(object, key);
    if (!value) {
        throwControls(std::string("has a missing or non-string '") + std::string(key) + "'");
    }
    return *value;
}

ControlsEffect parseControlsEffect(const Object& object)
{
    const std::string type = requireString(object, "type");
    ControlsEffect effect;
    if (type == "submit_command") {
        effect.type = ControlsEffect::Type::SubmitCommand;
        effect.token = requireWhole(object, "token");
        effect.json = requireString(object, "command_json");
    } else if (type == "submit_result") {
        effect.type = ControlsEffect::Type::SubmitResult;
        effect.token = requireWhole(object, "token");
        effect.json = requireString(object, "result_json");
    } else if (type == "run_attempt") {
        effect.type = ControlsEffect::Type::RunAttempt;
        effect.token = requireWhole(object, "token");
        effect.monitorId = requireString(object, "monitor_id");
        const std::string op = requireString(object, "op");
        if (op != "read" && op != "write") {
            throwControls("has an unknown 'op'");
        }
        effect.write = op == "write";
        const uint64_t code = requireWhole(object, "code");
        if (code > 255) {
            throwControls("has a VCP code out of range");
        }
        effect.code = static_cast<uint8_t>(code);
        effect.value = numberField(object, "value");
    } else if (type == "draft_changed") {
        effect.type = ControlsEffect::Type::DraftChanged;
        effect.monitorId = requireString(object, "monitor_id");
        effect.feature = requireString(object, "feature");
        if (effect.feature != "brightness" && effect.feature != "volume") {
            throwControls("has an unknown 'feature'");
        }
    } else {
        throwControls("has an unknown effect type");
    }
    return effect;
}

// Slider results carry no has_draft and always report a draft; query_draft results carry it.
DraftView parseDraftValue(const Object& object)
{
    DraftView view;
    if (hasField(object, "has_draft")) {
        const std::optional<bool> hasDraft = boolField(object, "has_draft");
        if (!hasDraft) {
            throwControls("has a non-boolean 'has_draft'");
        }
        view.hasDraft = *hasDraft;
    } else {
        view.hasDraft = true;
    }
    view.draft = requireNumber(object, "draft");
    view.status = requireString(object, "status");
    if (view.status != "ready" && view.status != "pending" && view.status != "failed") {
        throwControls("has an unknown draft 'status'");
    }
    return view;
}

void applyControlsValue(const json::IJsonValue& value, ControlsResult& result)
{
    if (!value) {
        throwControls("is missing 'value'");
    }
    if (value.ValueType() == json::JsonValueType::Null) {
        return;
    }
    const std::optional<Object> object = asObject(value);
    if (!object) {
        throwControls("has a 'value' that is neither null nor an object");
    }
    if (hasField(*object, "consume")) {
        const std::optional<bool> consume = boolField(*object, "consume");
        if (!consume) {
            throwControls("has a non-boolean 'consume'");
        }
        result.consume = *consume;
    } else {
        result.draft = parseDraftValue(*object);
    }
}

}  // namespace

std::string jsonQuote(std::string_view text)
{
    std::string out;
    appendQuoted(out, text);
    return out;
}

std::string buildMonitorDiscovered(const MonitorInfo& monitor)
{
    ObjectWriter body;
    body.addString("id", monitor.id)
        .addString("name", monitor.name)
        .addString("identity_status", monitor.identityStable ? "stable" : "ambiguous")
        .addString("connection_path", monitor.connectionPath)
        .addBool("primary", monitor.primary)
        .addRaw("brightness", channelJson(monitor.brightness))
        .addRaw("volume", channelJson(monitor.volume));

    ObjectWriter message;
    message.addString("type", "monitor_discovered").addRaw("monitor", body.finish());
    return message.finish();
}

std::string buildMonitorDisconnected(const std::string& monitorId)
{
    ObjectWriter message;
    message.addString("type", "monitor_disconnected").addString("monitor_id", monitorId);
    return message.finish();
}

std::string buildMonitorDisplayState(const std::string& monitorId, uint64_t generation, const std::string& state)
{
    ObjectWriter message;
    message.addString("type", "monitor_display_state")
        .addString("monitor_id", monitorId)
        .addUnsigned("generation", generation)
        .addString("state", state);
    return message.finish();
}

std::string buildPower(std::optional<bool> onBattery)
{
    ObjectWriter message;
    message.addString("type", "power");
    if (onBattery) {
        message.addBool("on_battery", *onBattery);
    } else {
        message.addRaw("on_battery", "null");
    }
    return message.finish();
}

std::string buildScreensaver(const std::string& state)
{
    ObjectWriter message;
    message.addString("type", "screensaver").addString("state", state);
    return message.finish();
}

std::string buildKeepAwakeSession(bool active)
{
    ObjectWriter message;
    message.addString("type", "keep_awake_session").addBool("active", active);
    return message.finish();
}

std::string buildPresenceObservation(const std::string& status, uint64_t capturedAtMs, uint64_t cameraGeneration,
                                     const std::optional<std::string>& cameraStatus)
{
    ObjectWriter message;
    message.addString("type", "presence_observation")
        .addString("status", status)
        .addUnsigned("captured_at_ms", capturedAtMs)
        .addUnsigned("camera_generation", cameraGeneration);
    if (cameraStatus) {
        message.addString("camera_status", *cameraStatus);
    }
    return message.finish();
}

std::string buildCameraStatus(const std::string& status, std::optional<uint64_t> generation)
{
    ObjectWriter message;
    message.addString("type", "camera_status").addString("status", status);
    if (generation) {
        message.addUnsigned("generation", *generation);
    }
    return message.finish();
}

std::string buildBuiltinDisplay(bool available, std::optional<double> brightness, bool calibrating, bool protectable,
                                bool holding)
{
    ObjectWriter message;
    message.addString("type", "builtin_display")
        .addBool("available", available)
        .addOptionalNumber("brightness", brightness)
        .addBool("calibrating", calibrating)
        .addBool("protectable", protectable)
        .addBool("holding", holding);
    return message.finish();
}

std::string buildSetBrightness(const std::string& monitorId, double value,
                               std::optional<uint64_t> connectionGeneration)
{
    ObjectWriter message;
    message.addString("type", "set_brightness").addString("monitor_id", monitorId).addNumber("value", value);
    if (connectionGeneration) {
        message.addUnsigned("connection_generation", *connectionGeneration);
    }
    return message.finish();
}

std::string buildSetVolume(const std::string& monitorId, double value, std::optional<uint64_t> connectionGeneration)
{
    ObjectWriter message;
    message.addString("type", "set_volume").addString("monitor_id", monitorId).addNumber("value", value);
    if (connectionGeneration) {
        message.addUnsigned("connection_generation", *connectionGeneration);
    }
    return message.finish();
}

std::string buildHardwareResult(uint64_t actionId, uint64_t generation, bool ok, std::optional<double> value,
                                const std::optional<std::string>& error)
{
    ObjectWriter message;
    message.addString("type", "hardware_result")
        .addUnsigned("action_id", actionId)
        .addUnsigned("generation", generation)
        .addBool("ok", ok);
    if (value) {
        message.addNumber("value", *value);
    }
    if (error) {
        message.addString("error", *error);
    }
    return message.finish();
}

std::string buildUpdateSettings(const std::string& settingsJson)
{
    ObjectWriter message;
    message.addString("type", "update_settings").addRaw("settings", settingsJson);
    return message.finish();
}

std::string buildControlsConfigure(double now, double brightnessStep, double volumeStep, bool keysEnabled,
                                   const std::optional<std::optional<std::string>>& volumeOwner)
{
    ObjectWriter message = eventWriter("configure", now);
    message.addNumber("brightness_step", brightnessStep)
        .addNumber("volume_step", volumeStep)
        .addBool("keys_enabled", keysEnabled);
    // Absent leaves the owner unchanged; null clears it; a string names the owning monitor.
    if (volumeOwner) {
        if (*volumeOwner) {
            message.addString("volume_owner", **volumeOwner);
        } else {
            message.addRaw("volume_owner", "null");
        }
    }
    return message.finish();
}

std::string buildControlsObserve(double now, const Snapshot& snapshot)
{
    std::vector<std::string> monitors;
    for (const MonitorSnapshot& monitor : snapshot.monitors) {
        ObjectWriter entry;
        entry.addString("id", monitor.id)
            .addUnsigned("generation", monitor.generation)
            .addString("display_state", monitor.displayState)
            .addString("identity_status", monitor.identityStatus)
            .addRaw("brightness", controlsChannelJson(monitor.brightness))
            .addRaw("volume", controlsChannelJson(monitor.volume));
        monitors.push_back(entry.finish());
    }

    std::vector<std::string> actions;
    for (const PendingAction& action : snapshot.pendingActions) {
        ObjectWriter entry;
        entry.addUnsigned("id", action.id)
            .addUnsigned("generation", action.generation)
            .addString("monitor_id", action.monitorId)
            .addString("kind", action.kind)
            .addOptionalNumber("value", action.value);
        actions.push_back(entry.finish());
    }

    ObjectWriter message = eventWriter("observe", now);
    message.addUnsigned("revision", snapshot.revision)
        .addRaw("monitors", arrayJson(monitors))
        .addRaw("pending_actions", arrayJson(actions));
    return message.finish();
}

std::string buildControlsSlider(double now, const std::string& monitorId, const std::string& feature, double value)
{
    ObjectWriter message = eventWriter("slider", now);
    message.addString("monitor_id", monitorId).addString("feature", feature).addNumber("value", value);
    return message.finish();
}

std::string buildControlsKey(double now, const std::string& key, bool down, bool repeat, bool fine,
                             const std::optional<KeyOwnerRef>& owner)
{
    ObjectWriter message = eventWriter("key", now);
    message.addString("key", key)
        .addString("phase", down ? "down" : "up")
        .addBool("repeat", repeat)
        .addBool("fine", fine);
    if (owner) {
        message.addRaw("owner", keyOwnerJson(*owner));
    } else {
        message.addRaw("owner", "null");
    }
    return message.finish();
}

std::string buildControlsQueryDraft(double now, const std::string& monitorId, const std::string& feature)
{
    ObjectWriter message = eventWriter("query_draft", now);
    message.addString("monitor_id", monitorId).addString("feature", feature);
    return message.finish();
}

std::string buildControlsMonitorProfile(double now, const std::string& monitorId, const std::string& manufacturer)
{
    ObjectWriter message = eventWriter("monitor_profile", now);
    message.addString("monitor_id", monitorId).addString("manufacturer", manufacturer);
    return message.finish();
}

std::string buildControlsMonitorRemoved(double now, const std::string& monitorId)
{
    ObjectWriter message = eventWriter("monitor_removed", now);
    message.addString("monitor_id", monitorId);
    return message.finish();
}

std::string buildControlsDisplayPower(double now, bool on)
{
    ObjectWriter message = eventWriter("display_power", now);
    message.addBool("on", on);
    return message.finish();
}

std::string buildControlsSubmitted(double now, uint64_t token, bool accepted)
{
    ObjectWriter message = eventWriter("submitted", now);
    message.addUnsigned("token", token).addBool("accepted", accepted);
    return message.finish();
}

std::string buildControlsAttemptDone(double now, uint64_t token, bool ok, const std::string& error,
                                     std::optional<double> value)
{
    ObjectWriter message = eventWriter("attempt_done", now);
    message.addUnsigned("token", token).addBool("ok", ok);
    // The error name is sent only on failure, as the contract requires.
    if (!ok) {
        message.addString("error", error);
    }
    message.addOptionalNumber("value", value);
    return message.finish();
}

std::string buildControlsTick(double now)
{
    return eventWriter("tick", now).finish();
}

std::string buildControlsShutdown(double now)
{
    return eventWriter("shutdown", now).finish();
}

uint64_t keepAwakeRemainingMilliseconds(uint64_t validForMs, double decodedAtSteadySeconds, double nowSteadySeconds)
{
    if (validForMs == 0) {
        return 0;
    }
    // Written as a negated comparison so NaN also counts as no elapsed time.
    double elapsed = nowSteadySeconds - decodedAtSteadySeconds;
    if (!(elapsed > 0.0)) {
        elapsed = 0.0;
    }
    const uint64_t elapsedMs = toUnsigned(elapsed * 1000.0);
    return validForMs > elapsedMs ? validForMs - elapsedMs : 0;
}

Snapshot parseSnapshot(const std::string& text)
{
    const double decodedAtSteadySeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const Object root = parseRoot(text);

    Snapshot snapshot;
    snapshot.decodedAtSteadySeconds = decodedAtSteadySeconds;
    snapshot.revision = unsignedOr(root, "revision");
    snapshot.status = stringOr(root, "status", "");

    if (const auto settings = objectField(root, "settings")) {
        snapshot.settings.presence_protection_enabled = boolOr(*settings, "presence_protection_enabled", false);
        snapshot.settings.hardware_keys_enabled = boolOr(*settings, "hardware_keys_enabled", false);
        snapshot.settings.ambient_follow_enabled = boolOr(*settings, "ambient_follow_enabled", false);
        snapshot.settings.locale = stringOr(*settings, "locale", "");
        snapshot.settings.presence_sampling_interval_s = numberOr(*settings, "presence_sampling_interval_s", 0.0);
        snapshot.settings.volume_key_step = intOr(*settings, "volume_key_step", 0);
        snapshot.settings.selected_camera_id = stringField(*settings, "selected_camera_id");
        snapshot.settings.return_sampling_interval_s = numberOr(*settings, "return_sampling_interval_s", 1.0);
        snapshot.settings.suspend_presence_on_battery = boolOr(*settings, "suspend_presence_on_battery", true);
        snapshot.settings.presence_min_brightness_percent = numberOr(*settings, "presence_min_brightness_percent", 20.0);
        snapshot.settings.always_on_camera = boolOr(*settings, "always_on_camera", true);
        snapshot.settings.screensaver_protection_enabled = boolOr(*settings, "screensaver_protection_enabled", false);
        snapshot.settings.keep_awake_with_presence = boolOr(*settings, "keep_awake_with_presence", false);
        snapshot.settings.launch_at_login = boolOr(*settings, "launch_at_login", false);
        snapshot.settings.brightness_key_step = intOr(*settings, "brightness_key_step", 5);
        snapshot.settings.screensaver_targets = numberMapField(*settings, "screensaver_targets");
        snapshot.settings.protection_disabled = stringSetField(*settings, "protection_disabled");
        snapshot.settings.inference_device = stringOr(*settings, "inference_device", "auto");
        snapshot.settings.ambientCurves = ambientCurvesField(*settings, "ambient_curves");
        snapshot.settings.builtin_auto_brightness_off_on_protection =
            boolOr(*settings, "builtin_auto_brightness_off_on_protection", false);
        snapshot.settings.monitored_display_id = stringField(*settings, "monitored_display_id");
        snapshot.settings.camera_history = stringVectorField(*settings, "camera_history");
        snapshot.settings.presence_min_brightness_by_display =
            numberMapField(*settings, "presence_min_brightness_by_display");
    }

    if (const auto monitored = objectField(root, "monitored_display")) {
        snapshot.monitoredDisplay.id = stringField(*monitored, "id");
        snapshot.monitoredDisplay.automatic = boolOr(*monitored, "automatic", false);
    }

    if (const auto builtin = objectField(root, "builtin_protection")) {
        snapshot.builtinProtection.protectable = boolOr(*builtin, "protectable", false);
        snapshot.builtinProtection.protected_ = boolOr(*builtin, "protected", false);
        snapshot.builtinProtection.available = boolOr(*builtin, "available", false);
        snapshot.builtinProtection.reasons = stringVectorField(*builtin, "reasons");
        snapshot.builtinProtection.targetPercent = numberField(*builtin, "target_percent");
        snapshot.builtinProtection.holdAutoBrightness = boolOr(*builtin, "hold_auto_brightness", false);
        snapshot.builtinProtection.holding = boolOr(*builtin, "holding", false);
    }

    if (const auto monitors = arrayField(root, "monitors")) {
        for (uint32_t i = 0; i < monitors->Size(); ++i) {
            if (const auto object = asObject(monitors->GetAt(i))) {
                snapshot.monitors.push_back(parseMonitor(*object));
            }
        }
    }

    if (const auto actions = arrayField(root, "pending_actions")) {
        for (uint32_t i = 0; i < actions->Size(); ++i) {
            if (const auto object = asObject(actions->GetAt(i))) {
                snapshot.pendingActions.push_back(parsePendingAction(*object));
            }
        }
    }

    if (const auto ambient = objectField(root, "ambient")) {
        snapshot.ambient.enabled = boolOr(*ambient, "enabled", false);
        snapshot.ambient.available = boolOr(*ambient, "available", false);
        snapshot.ambient.active = boolOr(*ambient, "active", false);
        snapshot.ambient.calibrating = boolOr(*ambient, "calibrating", false);
        snapshot.ambient.builtinPercent = numberField(*ambient, "builtin_percent");
    }

    if (const auto camera = objectField(root, "camera")) {
        snapshot.camera.mode = stringOr(*camera, "mode", "");
        snapshot.camera.captureMode = stringOr(*camera, "capture_mode", "");
        snapshot.camera.status = stringOr(*camera, "status", "unknown");
        snapshot.camera.displayStatus = stringOr(*camera, "display_status", "");
        snapshot.camera.nextSampleInMs = numberField(*camera, "next_sample_in_ms");
        snapshot.camera.retryInMs = numberField(*camera, "retry_in_ms");
        snapshot.camera.generation = unsignedOr(*camera, "generation");
        snapshot.camera.presence = stringField(*camera, "presence");
    }

    if (const auto power = objectField(root, "power")) {
        snapshot.power.onBattery = boolField(*power, "on_battery");
    }

    if (const auto screensaver = objectField(root, "screensaver")) {
        snapshot.screensaver.state = stringOr(*screensaver, "state", "unknown");
    }

    if (const auto keepAwake = objectField(root, "keep_awake")) {
        snapshot.keepAwake.enabled = boolOr(*keepAwake, "enabled", false);
        snapshot.keepAwake.requested = boolOr(*keepAwake, "requested", false);
        snapshot.keepAwake.observationId = unsignedOr(*keepAwake, "observation_id");
        snapshot.keepAwake.generation = unsignedOr(*keepAwake, "generation");
        snapshot.keepAwake.validForMs = unsignedOr(*keepAwake, "valid_for_ms");
        snapshot.keepAwake.reason = stringOr(*keepAwake, "reason", "");
    }

    return snapshot;
}

ControlsResult parseControlsResult(const std::string& text)
{
    const Object root = parseRoot(text);

    ControlsResult result;
    const std::optional<json::JsonArray> effects = arrayField(root, "effects");
    if (!effects) {
        throwControls("is missing 'effects'");
    }
    for (uint32_t i = 0; i < effects->Size(); ++i) {
        const std::optional<Object> object = asObject(effects->GetAt(i));
        if (!object) {
            throwControls("has an effect that is not an object");
        }
        result.effects.push_back(parseControlsEffect(*object));
    }

    applyControlsValue(fieldValue(root, "value"), result);

    const json::IJsonValue deadline = fieldValue(root, "next_deadline");
    if (!deadline) {
        throwControls("is missing 'next_deadline'");
    }
    if (deadline.ValueType() == json::JsonValueType::Number) {
        result.nextDeadline = deadline.GetNumber();
    } else if (deadline.ValueType() != json::JsonValueType::Null) {
        throwControls("has a 'next_deadline' that is neither a number nor null");
    }
    return result;
}

}  // namespace telaviva
