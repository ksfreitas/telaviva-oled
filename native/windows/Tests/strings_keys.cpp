// Checks the Windows UI string tables. Both locales must define the same keys,
// no value may be blank or use "Mac" wording, and keys must be plain identifiers.
// Reads TelaViva/Strings/<locale>/Resources.resw; a missing file fails the tests that need it.
#include <windows.h>

#include "TestHarness.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

using Entries = std::map<std::string, std::string>;

// Same layout rule as the JSON fixtures: the env var points at Tests\fixtures,
// otherwise the exe lives in native\windows\build\x64\Release.
fs::path stringsDirectory()
{
    char* override = nullptr;
    size_t length = 0;
    if (_dupenv_s(&override, &length, "TELAVIVA_TEST_FIXTURES") == 0 && override != nullptr) {
        const fs::path fixtures(override);
        std::free(override);
        return fixtures / ".." / ".." / "TelaViva" / "Strings";
    }
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return fs::path(buffer).parent_path() / ".." / ".." / ".." / "TelaViva" / "Strings";
}

fs::path reswPath(const char* locale)
{
    return stringsDirectory() / locale / "Resources.resw";
}

// Decodes the five predefined XML entities in one pass so "&amp;lt;" stays "&lt;".
std::string unescapeXml(const std::string& text)
{
    static const std::pair<std::string_view, char> entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bool decoded = false;
        for (const auto& [entity, ch] : entities) {
            if (std::string_view(text).substr(i).starts_with(entity)) {
                out.push_back(ch);
                i += entity.size() - 1;
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            out.push_back(text[i]);
        }
    }
    return out;
}

// Returns an empty map when the file is missing, so the tests fail instead of crashing.
Entries loadResw(const fs::path& file)
{
    Entries entries;
    if (!fs::is_regular_file(file)) {
        return entries;
    }
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const std::string xml = text.str();

    // Values may span lines, so [\s\S] is used instead of '.'.
    static const std::regex entry(R"re(<data\s+name="([^"]*)"[^>]*>\s*<value>([\s\S]*?)</value>)re");
    for (std::sregex_iterator it(xml.begin(), xml.end(), entry), end; it != end; ++it) {
        entries.emplace((*it)[1].str(), unescapeXml((*it)[2].str()));
    }
    return entries;
}

Entries loadLocale(const char* locale)
{
    return loadResw(reswPath(locale));
}

// Lists keys present in wanted but absent from available, joined for one failure message.
std::string missingKeys(const Entries& wanted, const Entries& available)
{
    std::string out;
    for (const auto& entry : wanted) {
        if (available.find(entry.first) == available.end()) {
            if (!out.empty()) {
                out += ", ";
            }
            out += entry.first;
        }
    }
    return out;
}

bool isBlank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

}  // namespace

TEST(strings_keys_files_exist_and_have_entries)
{
    for (const char* locale : {"en-US", "pt-BR"}) {
        const fs::path file = reswPath(locale);
        const bool exists = fs::is_regular_file(file);
        if (!exists) {
            std::printf("missing: %s\n", file.string().c_str());
        }
        CHECK(exists);
        const Entries entries = loadLocale(locale);
        if (entries.empty()) {
            std::printf("no entries in: %s\n", file.string().c_str());
        }
        CHECK(!entries.empty());
    }
}

TEST(strings_keys_match_between_locales)
{
    const Entries en = loadLocale("en-US");
    const Entries pt = loadLocale("pt-BR");

    const std::string enMissing = missingKeys(pt, en);
    if (!enMissing.empty()) {
        std::printf("en-US is missing keys: %s\n", enMissing.c_str());
    }
    CHECK_EQ(enMissing, std::string());

    const std::string ptMissing = missingKeys(en, pt);
    if (!ptMissing.empty()) {
        std::printf("pt-BR is missing keys: %s\n", ptMissing.c_str());
    }
    CHECK_EQ(ptMissing, std::string());
}

TEST(strings_values_not_blank)
{
    std::string offenders;
    for (const char* locale : {"en-US", "pt-BR"}) {
        for (const auto& entry : loadLocale(locale)) {
            if (isBlank(entry.second)) {
                if (!offenders.empty()) {
                    offenders += ", ";
                }
                offenders += std::string(locale) + ":" + entry.first;
            }
        }
    }
    CHECK_EQ(offenders, std::string());
}

TEST(strings_values_avoid_mac_wording)
{
    // "Mac" must be a whole word, so text such as "Machine" stays allowed.
    static const std::regex wholeMac(R"(\bMac\b)");
    std::string offenders;
    for (const char* locale : {"en-US", "pt-BR"}) {
        for (const auto& entry : loadLocale(locale)) {
            const std::string& value = entry.second;
            if (value.find("MacBook") != std::string::npos || std::regex_search(value, wholeMac)) {
                if (!offenders.empty()) {
                    offenders += ", ";
                }
                offenders += std::string(locale) + ":" + entry.first;
            }
        }
    }
    CHECK_EQ(offenders, std::string());
}

TEST(strings_keys_use_identifier_characters)
{
    static const std::regex keyPattern(R"([A-Za-z0-9._]+)");
    std::string offenders;
    for (const char* locale : {"en-US", "pt-BR"}) {
        for (const auto& entry : loadLocale(locale)) {
            if (!std::regex_match(entry.first, keyPattern)) {
                if (!offenders.empty()) {
                    offenders += ", ";
                }
                offenders += std::string(locale) + ":" + entry.first;
            }
        }
    }
    CHECK_EQ(offenders, std::string());
}

TEST(strings_dropped_keys_absent)
{
    // macOS-only keys: permission rows, the brightness step and the unused presenceUnavailable.
    static const char* const droppedKeys[] = {
        "hardwareKeys.brightnessStep",
        "hardwareKeys.requestPermission",
        "hardwareKeys.openAccessibility",
        "hardwareKeys.accessibility",
        "hardwareKeys.inputMonitoring",
        "hardwareKeys.granted",
        "hardwareKeys.notGranted",
        "hardwareKeys.requestInputMonitoring",
        "hardwareKeys.openInputMonitoring",
        "hardwareKeys.status.inputMonitoringRequired",
        "hardwareKeys.status.permissionRequired",
        "hardwareKeys.status.permissionRevoked",
        "presenceUnavailable",
        // Windows popup presence line, removed with the flyout presence row.
        "flyout.presence.detected",
        "flyout.presence.absent",
        "flyout.presence.lowlight",
        "flyout.presence.off",
    };
    std::string offenders;
    for (const char* locale : {"en-US", "pt-BR"}) {
        const Entries entries = loadLocale(locale);
        for (const char* key : droppedKeys) {
            if (entries.find(key) != entries.end()) {
                if (!offenders.empty()) {
                    offenders += ", ";
                }
                offenders += std::string(locale) + ":" + key;
            }
        }
    }
    CHECK_EQ(offenders, std::string());
}

TEST(strings_no_key_is_also_a_scope)
{
    // RESW dotted keys become resource paths, so "a" and "a.b" together fail PRI278.
    // Entries are sorted, so keys under "<key>." start at lower_bound.
    std::string offenders;
    for (const char* locale : {"en-US", "pt-BR"}) {
        const Entries entries = loadLocale(locale);
        for (const auto& leaf : entries) {
            const std::string scope = leaf.first + ".";
            const auto child = entries.lower_bound(scope);
            if (child != entries.end() && child->first.compare(0, scope.size(), scope) == 0) {
                if (!offenders.empty()) {
                    offenders += ", ";
                }
                offenders += std::string(locale) + ":" + leaf.first + " is a scope of " + child->first;
            }
        }
    }
    CHECK_EQ(offenders, std::string());
}

TEST(strings_pt_br_avoids_ajustes)
{
    // "Ajustes" is Apple wording; Windows pt-BR uses "Configurações".
    std::string offenders;
    for (const auto& entry : loadLocale("pt-BR")) {
        if (entry.second.find("Ajustes") != std::string::npos) {
            if (!offenders.empty()) {
                offenders += ", ";
            }
            offenders += entry.first;
        }
    }
    CHECK_EQ(offenders, std::string());
}

TEST(strings_windows_overrides)
{
    struct Override {
        const char* key;
        const char* en;
        const char* pt;
    };
    // "Configurações" is split so the UTF-8 bytes do not depend on the source encoding.
    static const Override overrides[] = {
        {"settings.title", "Settings", "Configura\xc3\xa7\xc3\xb5"
                                 "es"},
        {"quit", "Quit", "Sair"},
        {"launch", "Start when I sign in", "Iniciar ao entrar no Windows"},
    };
    const Entries en = loadLocale("en-US");
    const Entries pt = loadLocale("pt-BR");
    for (const auto& override : overrides) {
        const auto enIt = en.find(override.key);
        const auto ptIt = pt.find(override.key);
        const std::string enValue = enIt == en.end() ? std::string("<missing>") : enIt->second;
        const std::string ptValue = ptIt == pt.end() ? std::string("<missing>") : ptIt->second;
        CHECK_EQ(enValue, std::string(override.en));
        CHECK_EQ(ptValue, std::string(override.pt));
    }
}

TEST(strings_keys_advanced_page_keys_exist_in_both_locales)
{
    // Keys read by the Advanced page (inference device). Each must have a non-blank value in both locales.
    static const char* const advancedKeys[] = {
        "settings.tab.advanced",
        "inference.title",
        "inference.help",
        "inference.discreteHelp",
        "inference.auto",
        "inference.cpu",
        "inference.kind.integrated",
        "inference.kind.discrete",
        "inference.missing",
        "inference.inUse",
        "inference.notDecided",
        "inference.notice.unavailable",
        "inference.notice.failed",
    };
    std::string offenders;
    for (const char* locale : {"en-US", "pt-BR"}) {
        const Entries entries = loadLocale(locale);
        for (const char* key : advancedKeys) {
            const auto found = entries.find(key);
            if (found == entries.end() || isBlank(found->second)) {
                if (!offenders.empty()) {
                    offenders += ", ";
                }
                offenders += std::string(locale) + ":" + key;
            }
        }
    }
    CHECK_EQ(offenders, std::string());
}
