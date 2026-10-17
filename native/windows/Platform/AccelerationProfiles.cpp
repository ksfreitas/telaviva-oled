#include "AccelerationProfiles.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace telaviva {
namespace {

using Version = std::array<uint16_t, 4>;

constexpr size_t kNpos = std::string_view::npos;
constexpr uint64_t kMaxPciId = 0xFFFF;

// Internal control flow only; never escapes parseAccelerationProfiles.
struct ParseError {
    std::string message;
};

[[noreturn]] void fail(size_t line, const std::string& what)
{
    throw ParseError{"line " + std::to_string(line) + ": " + what};
}

std::optional<Version> parseDriverVersion(std::string_view text)
{
    Version version{};
    size_t part = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i != text.size() && text[i] != '.') {
            continue;
        }
        if (part >= version.size()) {
            return std::nullopt;
        }
        const std::string_view digits = text.substr(start, i - start);
        // Five digits at most, so the accumulator cannot overflow.
        if (digits.empty() || digits.size() > 5) {
            return std::nullopt;
        }
        uint32_t value = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        if (value > 0xFFFF) {
            return std::nullopt;
        }
        version[part] = static_cast<uint16_t>(value);
        ++part;
        start = i + 1;
    }
    if (part != version.size()) {
        return std::nullopt;
    }
    return version;
}

std::string_view trim(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == kNpos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

size_t skipSpaces(std::string_view text, size_t pos)
{
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

int digitValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Reads a decimal or 0x hexadecimal integer at pos. No sign, underscores or leading zeros.
uint64_t parseInteger(std::string_view text, size_t& pos, size_t line)
{
    uint64_t base = 10;
    if (pos + 1 < text.size() && text[pos] == '0' && text[pos + 1] == 'x') {
        base = 16;
        pos += 2;
    }
    const size_t digitsStart = pos;
    uint64_t value = 0;
    while (pos < text.size()) {
        const int digit = digitValue(text[pos]);
        if (digit < 0 || static_cast<uint64_t>(digit) >= base) {
            break;
        }
        const auto d = static_cast<uint64_t>(digit);
        if (value > (std::numeric_limits<uint64_t>::max() - d) / base) {
            fail(line, "integer is too large");
        }
        value = value * base + d;
        ++pos;
    }
    if (pos == digitsStart) {
        fail(line, "expected an integer");
    }
    if (base == 10 && pos - digitsStart > 1 && text[digitsStart] == '0') {
        fail(line, "leading zeros are not allowed");
    }
    return value;
}

struct Value {
    enum class Kind { String, Integer, IntegerArray };
    Kind kind = Kind::String;
    std::string text;
    uint64_t integer = 0;
    std::vector<uint64_t> integers;
};

// Parses exactly one value that fills `text`, allowing spaces and a trailing comment.
Value parseValue(std::string_view text, size_t line)
{
    Value value;
    size_t pos = 0;
    if (!text.empty() && text[0] == '"') {
        const size_t close = text.find('"', 1);
        if (close == kNpos) {
            fail(line, "unterminated string");
        }
        value.kind = Value::Kind::String;
        value.text = std::string(text.substr(1, close - 1));
        for (char c : value.text) {
            if (c == '\\' || static_cast<unsigned char>(c) < 0x20) {
                fail(line, "strings take no escapes and no control characters");
            }
        }
        pos = close + 1;
    } else if (!text.empty() && text[0] == '[') {
        value.kind = Value::Kind::IntegerArray;
        pos = skipSpaces(text, 1);
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
        } else {
            for (;;) {
                pos = skipSpaces(text, pos);
                value.integers.push_back(parseInteger(text, pos, line));
                pos = skipSpaces(text, pos);
                if (pos >= text.size()) {
                    fail(line, "unterminated array");
                }
                if (text[pos] == ']') {
                    ++pos;
                    break;
                }
                if (text[pos] != ',') {
                    fail(line, "expected ',' or ']' in array");
                }
                ++pos;
            }
        }
    } else {
        value.kind = Value::Kind::Integer;
        value.integer = parseInteger(text, pos, line);
    }
    pos = skipSpaces(text, pos);
    if (pos < text.size() && text[pos] != '#') {
        fail(line, "unexpected text after value");
    }
    return value;
}

bool isKeyChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool isValidId(const std::string& text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

// YYYY-MM-DD with a plausible month and day.
bool isValidDate(const std::string& text)
{
    if (text.size() != 10) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        const bool dash = (i == 4 || i == 7);
        const bool digit = text[i] >= '0' && text[i] <= '9';
        if (dash ? text[i] != '-' : !digit) {
            return false;
        }
    }
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

void requireKind(const Value& value, Value::Kind kind, const std::string& key, size_t line)
{
    if (value.kind != kind) {
        fail(line, "wrong type for '" + key + "'");
    }
}

void applyProfileField(AccelerationProfile& profile, const std::string& key, const Value& value, size_t line)
{
    if (key == "id") {
        requireKind(value, Value::Kind::String, key, line);
        if (!isValidId(value.text)) {
            fail(line, "id must be lowercase letters, digits and '-'");
        }
        profile.id = value.text;
    } else if (key == "provider") {
        requireKind(value, Value::Kind::String, key, line);
        if (value.text != "directml") {
            fail(line, "provider must be \"directml\"");
        }
        profile.provider = value.text;
    } else if (key == "vendor_id") {
        requireKind(value, Value::Kind::Integer, key, line);
        if (value.integer > kMaxPciId) {
            fail(line, "vendor_id must fit in 16 bits");
        }
        profile.vendorId = static_cast<uint32_t>(value.integer);
    } else if (key == "device_ids") {
        requireKind(value, Value::Kind::IntegerArray, key, line);
        std::vector<uint32_t> deviceIds;
        for (uint64_t item : value.integers) {
            if (item > kMaxPciId) {
                fail(line, "device id must fit in 16 bits");
            }
            const auto id = static_cast<uint32_t>(item);
            if (std::find(deviceIds.begin(), deviceIds.end(), id) != deviceIds.end()) {
                fail(line, "duplicate device id");
            }
            deviceIds.push_back(id);
        }
        profile.deviceIds = std::move(deviceIds);
    } else if (key == "min_driver") {
        requireKind(value, Value::Kind::String, key, line);
        if (!parseDriverVersion(value.text)) {
            fail(line, "min_driver must be a.b.c.d with four numeric parts");
        }
        profile.minDriver = value.text;
    } else if (key == "max_driver") {
        requireKind(value, Value::Kind::String, key, line);
        if (!value.text.empty() && !parseDriverVersion(value.text)) {
            fail(line, "max_driver must be empty or a.b.c.d with four numeric parts");
        }
        profile.maxDriver = value.text;
    } else if (key == "measurement") {
        requireKind(value, Value::Kind::String, key, line);
        if (value.text.empty()) {
            fail(line, "measurement reference must not be empty");
        }
        profile.measurement = value.text;
    } else if (key == "measured_on") {
        requireKind(value, Value::Kind::String, key, line);
        if (!isValidDate(value.text)) {
            fail(line, "measured_on must be YYYY-MM-DD");
        }
        profile.measuredOn = value.text;
    } else {
        fail(line, "unknown key '" + key + "'");
    }
}

// Checks that a [[profile]] table has every key and is unique, then keeps it.
void finishProfile(const AccelerationProfile& profile, size_t line, const std::set<std::string>& keys,
                   std::set<std::string>& ids, std::vector<AccelerationProfile>& out)
{
    if (!keys.contains("measurement")) {
        fail(line, "profile has no measurement reference");
    }
    for (const char* required : {"id", "provider", "vendor_id", "device_ids", "min_driver", "max_driver",
                                 "measured_on"}) {
        if (!keys.contains(required)) {
            fail(line, std::string("profile is missing '") + required + "'");
        }
    }
    const std::optional<Version> minVersion = parseDriverVersion(profile.minDriver);
    const std::optional<Version> maxVersion = parseDriverVersion(profile.maxDriver);
    if (!profile.maxDriver.empty() && *maxVersion < *minVersion) {
        fail(line, "max_driver is below min_driver");
    }
    if (!ids.insert(profile.id).second) {
        fail(line, "duplicate profile id '" + profile.id + "'");
    }
    out.push_back(profile);
}

ProfileLoadResult parseDocument(std::string_view text)
{
    ProfileLoadResult result;
    bool schemaSeen = false;
    bool inProfile = false;
    size_t profileLine = 0;
    AccelerationProfile current;
    std::set<std::string> keysInProfile;
    std::set<std::string> ids;

    size_t pos = 0;
    size_t lineNo = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == kNpos) {
            eol = text.size();
        }
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        if (line.front() == '[') {
            // Only [[profile]] exists in schema 1; a trailing comment is allowed.
            if (line.substr(0, 2) != "[[") {
                fail(lineNo, "unknown table");
            }
            if (trim(line.substr(0, line.find('#'))) != "[[profile]]") {
                fail(lineNo, "unknown table");
            }
            if (!schemaSeen) {
                fail(lineNo, "schema = 1 must come before the first [[profile]]");
            }
            if (inProfile) {
                finishProfile(current, profileLine, keysInProfile, ids, result.profiles);
            }
            current = AccelerationProfile{};
            keysInProfile.clear();
            inProfile = true;
            profileLine = lineNo;
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == kNpos) {
            fail(lineNo, "expected key = value");
        }
        const std::string_view keyText = trim(line.substr(0, equals));
        if (keyText.empty() || !std::all_of(keyText.begin(), keyText.end(), isKeyChar)) {
            fail(lineNo, "invalid key");
        }
        const std::string key(keyText);
        const std::string_view valueText = trim(line.substr(equals + 1));
        if (valueText.empty()) {
            fail(lineNo, "missing value for '" + key + "'");
        }
        const Value value = parseValue(valueText, lineNo);

        if (!inProfile) {
            if (key != "schema") {
                fail(lineNo, "unknown top-level key '" + key + "'");
            }
            if (schemaSeen) {
                fail(lineNo, "duplicate schema");
            }
            if (value.kind != Value::Kind::Integer) {
                fail(lineNo, "schema must be an integer");
            }
            if (value.integer != 1) {
                fail(lineNo, "unsupported schema version");
            }
            schemaSeen = true;
            continue;
        }
        if (!keysInProfile.insert(key).second) {
            fail(lineNo, "duplicate key '" + key + "'");
        }
        applyProfileField(current, key, value, lineNo);
    }

    if (!schemaSeen) {
        throw ParseError{"missing schema = 1"};
    }
    if (inProfile) {
        finishProfile(current, profileLine, keysInProfile, ids, result.profiles);
    }
    return result;
}

}  // namespace

ProfileLoadResult parseAccelerationProfiles(std::string_view text)
{
    ProfileLoadResult result;
    try {
        result = parseDocument(text);
    } catch (const ParseError& error) {
        result = ProfileLoadResult{};
        result.error = error.message;
    } catch (const std::exception&) {
        result = ProfileLoadResult{};
        result.error = "acceleration profiles could not be parsed";
    }
    return result;
}

ProfileLoadResult loadAccelerationProfiles(const std::filesystem::path& file)
{
    ProfileLoadResult result;
    try {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec) || ec) {
            result.error = "acceleration profiles file is missing";
            return result;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            result.error = "acceleration profiles file cannot be opened";
            return result;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) {
            result.error = "acceleration profiles file cannot be read";
            return result;
        }
        return parseAccelerationProfiles(text);
    } catch (const std::exception&) {
        result = ProfileLoadResult{};
        result.error = "acceleration profiles file cannot be read";
    }
    return result;
}

const AccelerationProfile* matchProfile(const std::vector<AccelerationProfile>& profiles, const AdapterInfo& adapter)
{
    if (adapter.software) {
        return nullptr;
    }
    const std::optional<Version> driver = parseDriverVersion(adapter.driverVersion);
    if (!driver) {
        return nullptr;
    }
    for (const AccelerationProfile& profile : profiles) {
        if (profile.vendorId != adapter.vendorId) {
            continue;
        }
        if (!profile.deviceIds.empty() &&
            std::find(profile.deviceIds.begin(), profile.deviceIds.end(), adapter.deviceId) ==
                profile.deviceIds.end()) {
            continue;
        }
        const std::optional<Version> minVersion = parseDriverVersion(profile.minDriver);
        if (!minVersion || *driver < *minVersion) {
            continue;
        }
        if (!profile.maxDriver.empty()) {
            const std::optional<Version> maxVersion = parseDriverVersion(profile.maxDriver);
            if (!maxVersion || *driver > *maxVersion) {
                continue;
            }
        }
        return &profile;
    }
    return nullptr;
}

}  // namespace telaviva
