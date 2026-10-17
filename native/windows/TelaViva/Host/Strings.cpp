#include "pch.h"

#include "Strings.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Globalization.h>

#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace telaviva::host {

namespace {

std::mutex g_mutex;

// Resolved tag ("en-US" or "pt-BR") that Str passes as the Language qualifier.
// Empty until the first lookup or setLanguage, which resolves the default "auto".
std::wstring g_language;

std::wstring widen(std::string_view text)
{
    std::wstring out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

// The first preferred UI language decides; the list is ordered by user preference.
std::wstring resolveAuto()
{
    try {
        const auto languages = winrt::Windows::Globalization::ApplicationLanguages::Languages();
        if (languages.Size() > 0) {
            const winrt::hstring first = languages.GetAt(0);
            const std::wstring_view tag = first;
            if (tag.size() >= 2 && std::towlower(tag[0]) == L'p' && std::towlower(tag[1]) == L't') {
                return L"pt-BR";
            }
        }
    } catch (...) {
        // Fall through to the English default when the language list is unavailable.
    }
    return L"en-US";
}

std::wstring currentLanguage()
{
    {
        std::lock_guard lock(g_mutex);
        if (!g_language.empty()) {
            return g_language;
        }
    }
    // Resolve outside the lock; WinRT calls can be slow. A concurrent setLanguage wins.
    std::wstring resolved = resolveAuto();
    std::lock_guard lock(g_mutex);
    if (g_language.empty()) {
        g_language = std::move(resolved);
    }
    return g_language;
}

}  // namespace

std::wstring Str(std::string_view key)
{
    const std::wstring fallback = L"[" + widen(key) + L"]";
    try {
        // The .resw file name gives the top-level map; dots in the key become path separators.
        std::wstring path = L"Resources/";
        for (char c : key) {
            path.push_back(c == '.' ? L'/' : static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }

        // Created once; the ResourceManager constructor is thread-safe for function-local statics.
        static const winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager manager{};
        // A fresh context per lookup keeps the language override free of shared mutable state.
        const auto context = manager.CreateResourceContext();
        context.QualifierValues().Insert(L"Language", currentLanguage());

        const auto candidate = manager.MainResourceMap().GetValue(path, context);
        const winrt::hstring value = candidate.ValueAsString();
        if (!value.empty()) {
            return std::wstring(value);
        }
    } catch (...) {
        // Lookup must never throw; a missing or broken resource shows as [key].
    }
    return fallback;
}

void setLanguage(std::string_view bcp47)
{
    std::wstring resolved;
    if (bcp47 == "en-US") {
        resolved = L"en-US";
    } else if (bcp47 == "pt-BR") {
        resolved = L"pt-BR";
    } else {
        resolved = resolveAuto();
    }

    std::lock_guard lock(g_mutex);
    g_language = std::move(resolved);
}

}  // namespace telaviva::host
