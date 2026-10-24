#include "TestHarness.h"

#include "Platform/DisplayCatalog.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

telaviva::CatalogEntry makeEntry(const std::string& id, bool stable)
{
    telaviva::CatalogEntry entry;
    entry.identity.id = id;
    entry.identity.stable = stable;
    return entry;
}

const wchar_t* kExamplePath = L"\\\\?\\DISPLAY#GSM5BC0#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";

}  // namespace

TEST(display_catalog_diff_added)
{
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("win-A", true)};
    const std::vector<telaviva::CatalogEntry> current{makeEntry("win-A", true), makeEntry("win-B", true)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, current);
    CHECK_EQ(diff.added.size(), std::size_t(1));
    if (diff.added.size() == 1) {
        CHECK_EQ(diff.added[0].identity.id, std::string("win-B"));
    }
    CHECK(diff.removedIds.empty());
}

TEST(display_catalog_diff_removed)
{
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("win-A", true), makeEntry("win-B", true)};
    const std::vector<telaviva::CatalogEntry> current{makeEntry("win-A", true)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, current);
    CHECK(diff.added.empty());
    CHECK_EQ(diff.removedIds.size(), std::size_t(1));
    if (diff.removedIds.size() == 1) {
        CHECK_EQ(diff.removedIds[0], std::string("win-B"));
    }
}

TEST(display_catalog_diff_unchanged)
{
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("win-A", true), makeEntry("win-B", false)};
    const std::vector<telaviva::CatalogEntry> current{makeEntry("win-A", true), makeEntry("win-B", false)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, current);
    CHECK(diff.added.empty());
    CHECK(diff.removedIds.empty());
}

TEST(display_catalog_diff_stable_flag_flip)
{
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("win-X", true)};
    const std::vector<telaviva::CatalogEntry> current{makeEntry("win-X", false)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, current);
    CHECK_EQ(diff.added.size(), std::size_t(1));
    if (diff.added.size() == 1) {
        CHECK_EQ(diff.added[0].identity.id, std::string("win-X"));
        CHECK(!diff.added[0].identity.stable);
    }
    CHECK_EQ(diff.removedIds.size(), std::size_t(1));
    if (diff.removedIds.size() == 1) {
        CHECK_EQ(diff.removedIds[0], std::string("win-X"));
    }
}

TEST(display_catalog_diff_keeps_primary_flag)
{
    // The diff returns the entries it was given, so the primary flag reaches discovery unchanged.
    telaviva::CatalogEntry primary = makeEntry("win-A", true);
    primary.primary = true;
    const std::vector<telaviva::CatalogEntry> current{primary, makeEntry("win-B", true)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs({}, current);
    CHECK_EQ(diff.added.size(), std::size_t(2));
    if (diff.added.size() == 2) {
        CHECK(diff.added[0].primary);
        CHECK(!diff.added[1].primary);
    }
}

TEST(display_catalog_diff_empty_previous)
{
    const std::vector<telaviva::CatalogEntry> current{makeEntry("win-A", true), makeEntry("win-B", true)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs({}, current);
    CHECK_EQ(diff.added.size(), std::size_t(2));
    CHECK(diff.removedIds.empty());
}

TEST(display_catalog_diff_empty_current)
{
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("win-A", true), makeEntry("win-B", true)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, {});
    CHECK(diff.added.empty());
    CHECK_EQ(diff.removedIds.size(), std::size_t(2));
}

TEST(display_catalog_diff_ignores_empty_ids)
{
    // Entries whose EDID did not parse have no id and never produce diff output.
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("", false), makeEntry("win-A", true)};
    const std::vector<telaviva::CatalogEntry> current{makeEntry("", false), makeEntry("", false),
                                                      makeEntry("win-A", true)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, current);
    CHECK(diff.added.empty());
    CHECK(diff.removedIds.empty());

    const telaviva::CatalogDiff onlyEmpty = telaviva::diffCatalogs({makeEntry("", false)}, {});
    CHECK(onlyEmpty.added.empty());
    CHECK(onlyEmpty.removedIds.empty());
}

TEST(display_catalog_diff_new_duplicate_is_added)
{
    // A second identical unstable monitor is a new entry even though its id is known.
    const std::vector<telaviva::CatalogEntry> previous{makeEntry("win-D", false)};
    const std::vector<telaviva::CatalogEntry> current{makeEntry("win-D", false), makeEntry("win-D", false)};
    const telaviva::CatalogDiff diff = telaviva::diffCatalogs(previous, current);
    CHECK_EQ(diff.added.size(), std::size_t(1));
    CHECK(diff.removedIds.empty());
}

TEST(display_catalog_parse_device_path_example)
{
    const auto parsed = telaviva::parseDisplayDevicePath(kExamplePath);
    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    CHECK(parsed->first == L"GSM5BC0");
    CHECK(parsed->second == L"5&1a2b3c&0&UID4352");
}

TEST(display_catalog_parse_device_path_lowercase)
{
    const auto parsed = telaviva::parseDisplayDevicePath(
        L"\\\\?\\display#gsm5bc0#5&1a2b3c&0&uid4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
    CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    CHECK(parsed->first == L"gsm5bc0");
    CHECK(parsed->second == L"5&1a2b3c&0&uid4352");
}

TEST(display_catalog_parse_device_path_malformed)
{
    CHECK(!telaviva::parseDisplayDevicePath(L"\\\\?\\DISPLAY#GSM5BC0").has_value());
    CHECK(!telaviva::parseDisplayDevicePath(L"\\\\?\\DISPLAY##5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}")
               .has_value());
    CHECK(!telaviva::parseDisplayDevicePath(L"DISPLAY#GSM5BC0#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}")
               .has_value());
    CHECK(!telaviva::parseDisplayDevicePath(L"\\\\?\\USB#VID_1234#5&1a2b3c&0#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}")
               .has_value());
}

TEST(display_catalog_parse_device_path_empty)
{
    CHECK(!telaviva::parseDisplayDevicePath(L"").has_value());
}

TEST(display_catalog_parse_device_path_without_interface_guid)
{
    CHECK(!telaviva::parseDisplayDevicePath(L"\\\\?\\DISPLAY#GSM5BC0#5&1a2b3c&0&UID4352").has_value());
    CHECK(!telaviva::parseDisplayDevicePath(L"\\\\?\\DISPLAY#GSM5BC0#5&1a2b3c&0&UID4352#e6f07b5f").has_value());
}
