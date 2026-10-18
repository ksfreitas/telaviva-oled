#include "WinHttpGet.h"

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace telaviva {
namespace {

// Milliseconds: resolve, connect, send, receive. Matches the release contract.
constexpr int kResolveTimeoutMs = 3000;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kSendTimeoutMs = 3000;
constexpr int kReceiveTimeoutMs = 5000;
constexpr std::size_t kBodyCapBytes = 1048576;
constexpr DWORD kChunkBytes = 16384;

struct HandleCloser {
    void operator()(void* handle) const { WinHttpCloseHandle(handle); }
};

// Declared in open order, so the request closes before its connection and session.
using Handle = std::unique_ptr<void, HandleCloser>;

struct Target {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
};

std::optional<std::wstring> toWide(const std::string& text)
{
    if (text.empty()) {
        return std::wstring{};
    }
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (size <= 0) {
        return std::nullopt;
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.data(), length, out.data(), size) <= 0) {
        return std::nullopt;
    }
    return out;
}

// Accepts only https URLs; anything else is refused before any network use.
std::optional<Target> parseHttpsUrl(const std::wstring& url)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    // Asking for pointers into url instead of copies.
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        return std::nullopt;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.lpszHostName == nullptr || parts.dwHostNameLength == 0) {
        return std::nullopt;
    }

    Target target;
    target.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    target.port = parts.nPort;
    if (parts.lpszUrlPath != nullptr) {
        target.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (parts.lpszExtraInfo != nullptr) {
        target.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (target.path.empty()) {
        target.path = L"/";
    }
    return target;
}

// Writes result only when the whole exchange succeeds, including a status code.
bool fetch(const std::string& url, const std::string& userAgent, HttpResult& result)
{
    const std::optional<std::wstring> wideUrl = toWide(url);
    const std::optional<std::wstring> wideAgent = toWide(userAgent);
    if (!wideUrl || !wideAgent) {
        return false;
    }
    const std::optional<Target> target = parseHttpsUrl(*wideUrl);
    if (!target) {
        return false;
    }

    Handle session(WinHttpOpen(wideAgent->c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return false;
    }
    if (!WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                            kReceiveTimeoutMs)) {
        return false;
    }

    Handle connection(WinHttpConnect(session.get(), target->host.c_str(), target->port, 0));
    if (!connection) {
        return false;
    }

    Handle request(WinHttpOpenRequest(connection.get(), L"GET", target->path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        return false;
    }

    // No cookie store, so nothing from earlier answers is sent back.
    DWORD disableCookies = WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disableCookies,
                          static_cast<DWORD>(sizeof(disableCookies)))) {
        return false;
    }

    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpAddRequestHeaders(request.get(), headers, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
        return false;
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = static_cast<DWORD>(sizeof(status));
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }

    std::string body;
    std::vector<char> chunk(kChunkBytes);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), kChunkBytes, &read)) {
            return false;
        }
        if (read == 0) {
            break;
        }
        // A body over the cap is a failure, not a truncated answer.
        if (body.size() + read > kBodyCapBytes) {
            return false;
        }
        body.append(chunk.data(), read);
    }

    result.transportOk = true;
    result.status = static_cast<int>(status);
    result.body = std::move(body);
    return true;
}

}  // namespace

HttpResult WinHttpGet::get(const std::string& url, const std::string& userAgent)
{
    HttpResult result;
    try {
        if (fetch(url, userAgent, result)) {
            return result;
        }
    } catch (...) {
        // Falls through to the failure result; no exception leaves this call.
    }
    return HttpResult{};
}

}  // namespace telaviva
