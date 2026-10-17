#include "InferenceFailureStore.h"

#include "JsonCodec.h"

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace telaviva {
namespace {

constexpr FailureReason kAllReasons[] = {FailureReason::RegisterFailed, FailureReason::SessionFailed,
                                         FailureReason::SelfTestMismatch, FailureReason::SelfTestSlow,
                                         FailureReason::RuntimeError};

std::optional<FailureReason> parseReason(std::string_view name)
{
    for (FailureReason reason : kAllReasons) {
        if (failureReasonName(reason) == name) {
            return reason;
        }
    }
    return std::nullopt;
}

// Same apartment setup as JsonCodec.cpp; returns false when WinRT cannot start here.
bool ensureApartment()
{
    thread_local bool initialized = false;
    if (initialized) {
        return true;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        // The thread already has another apartment; parsing works there too.
        if (error.code() != static_cast<winrt::hresult>(RPC_E_CHANGED_MODE)) {
            return false;
        }
    }
    initialized = true;
    return true;
}

std::string utcTimestampNow()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    if (gmtime_s(&utc, &now) != 0) {
        return "1970-01-01T00:00:00Z";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

// Returns false for any shape other than the schema 1 layout, including wrong types.
bool parseFile(const std::string& text, std::vector<FailureRecord>& out)
{
    try {
        const winrt::Windows::Data::Json::JsonObject root =
            winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(std::string_view(text)));
        if (root.GetNamedNumber(L"schema") != 1) {
            return false;
        }
        const winrt::Windows::Data::Json::JsonArray failures = root.GetNamedArray(L"failures");
        std::vector<FailureRecord> parsed;
        for (uint32_t i = 0; i < failures.Size(); ++i) {
            const winrt::Windows::Data::Json::JsonObject item = failures.GetObjectAt(i);
            const std::optional<FailureReason> reason =
                parseReason(winrt::to_string(item.GetNamedString(L"reason")));
            if (!reason) {
                return false;
            }
            FailureRecord record;
            record.key.profile = winrt::to_string(item.GetNamedString(L"profile"));
            record.key.driver = winrt::to_string(item.GetNamedString(L"driver"));
            record.key.app = winrt::to_string(item.GetNamedString(L"app"));
            record.reason = *reason;
            record.at = winrt::to_string(item.GetNamedString(L"at"));
            parsed.push_back(std::move(record));
        }
        out = std::move(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

std::string_view failureReasonName(FailureReason reason)
{
    switch (reason) {
    case FailureReason::RegisterFailed:
        return "register_failed";
    case FailureReason::SessionFailed:
        return "session_failed";
    case FailureReason::SelfTestMismatch:
        return "self_test_mismatch";
    case FailureReason::SelfTestSlow:
        return "self_test_slow";
    case FailureReason::RuntimeError:
        return "runtime_error";
    }
    return "runtime_error";
}

InferenceFailureStore::InferenceFailureStore(std::filesystem::path directory)
    : file_(std::move(directory))
{
    file_ /= L"inference-failures.json";
    load();
}

bool InferenceFailureStore::hasMarker(const FailureKey& key) const
{
    for (const FailureRecord& record : records_) {
        if (record.key == key) {
            return true;
        }
    }
    return false;
}

void InferenceFailureStore::record(const FailureKey& key, FailureReason reason)
{
    std::erase_if(records_, [&key](const FailureRecord& existing) { return existing.key == key; });
    FailureRecord record;
    record.key = key;
    record.reason = reason;
    record.at = utcTimestampNow();
    records_.push_back(std::move(record));
    save();
}

void InferenceFailureStore::clear(const FailureKey& key)
{
    const size_t before = records_.size();
    std::erase_if(records_, [&key](const FailureRecord& existing) { return existing.key == key; });
    if (records_.size() != before) {
        save();
    }
}

void InferenceFailureStore::clearIfChanged(const std::string& currentDriver, const std::string& currentApp)
{
    const size_t before = records_.size();
    std::erase_if(records_, [&currentDriver, &currentApp](const FailureRecord& existing) {
        return existing.key.driver != currentDriver || existing.key.app != currentApp;
    });
    if (records_.size() != before) {
        save();
    }
}

const std::vector<FailureRecord>& InferenceFailureStore::records() const
{
    return records_;
}

std::filesystem::path InferenceFailureStore::file() const
{
    return file_;
}

void InferenceFailureStore::load()
{
    records_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        return;
    }
    if (!ensureApartment()) {
        // Without WinRT the file cannot be judged, so it is left alone.
        return;
    }
    std::string text;
    try {
        std::ifstream in(file_, std::ios::binary);
        if (!in) {
            return;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (...) {
        return;
    }
    std::vector<FailureRecord> parsed;
    if (parseFile(text, parsed)) {
        records_ = std::move(parsed);
        return;
    }
    quarantine();
}

void InferenceFailureStore::save() const
{
    std::string text = "{\"schema\":1,\"failures\":[";
    for (size_t i = 0; i < records_.size(); ++i) {
        const FailureRecord& record = records_[i];
        if (i > 0) {
            text += ',';
        }
        text += "{\"profile\":" + jsonQuote(record.key.profile) + ",\"driver\":" + jsonQuote(record.key.driver) +
                ",\"app\":" + jsonQuote(record.key.app) + ",\"reason\":" +
                jsonQuote(failureReasonName(record.reason)) + ",\"at\":" + jsonQuote(record.at) + "}";
    }
    text += "]}";

    try {
        std::error_code ec;
        static_cast<void>(std::filesystem::create_directories(file_.parent_path(), ec));

        std::filesystem::path temp = file_;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return;
            }
            out << text;
            out.flush();
            if (!out) {
                out.close();
                static_cast<void>(std::filesystem::remove(temp, ec));
                return;
            }
        }
        // rename replaces an existing target on Windows, so readers see either version.
        std::filesystem::rename(temp, file_, ec);
        if (ec) {
            static_cast<void>(std::filesystem::remove(temp, ec));
        }
    } catch (...) {
        // Persistence is best effort; the in-memory markers still apply this session.
    }
}

void InferenceFailureStore::quarantine() const
{
    std::filesystem::path corrupt = file_;
    corrupt += L".corrupt";
    std::error_code ec;
    std::filesystem::rename(file_, corrupt, ec);
}

}  // namespace telaviva
